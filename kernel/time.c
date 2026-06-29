/* kernel/time.c — POSIX clocks, timers, nanosleep, itimers (P8) */

#include "kernel/time.h"
#include "drivers/timer/pit.h"
#include "kernel/lib/errno.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/signal.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/lib/kprintf.h"
#include <stdint.h>

/* CMOS RTC ports */
#define CMOS_CMD  0x70
#define CMOS_DATA 0x71

uint64_t kernel_boot_epoch_sec = 0;

/* ---- CMOS helpers (no driver needed) ---- */
static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_CMD, 0x80 | reg);
    return inb(CMOS_DATA);
}

static int cmos_updating(void) {
    return cmos_read(0x0A) & 0x80;
}

static uint8_t bcd2bin(uint8_t v) {
    return ((v >> 4) * 10) + (v & 0x0F);
}

/* Read CMOS RTC and convert to Unix epoch (UTC, ignores DST) */
static uint64_t cmos_read_epoch(void) {
    while (cmos_updating()) {}

    int s  = bcd2bin(cmos_read(0x00));
    int m  = bcd2bin(cmos_read(0x02));
    int h  = bcd2bin(cmos_read(0x04));
    int d  = bcd2bin(cmos_read(0x07));
    int mo = bcd2bin(cmos_read(0x08));
    int y  = bcd2bin(cmos_read(0x09)) + 2000;

    static const int days_per_month[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};

    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));

    uint64_t days = 0;
    for (int yr = 1970; yr < y; yr++) {
        days += 365 + ((yr%4==0 && (yr%100!=0 || yr%400==0)) ? 1 : 0);
    }
    for (int mn = 1; mn < mo; mn++) {
        days += days_per_month[mn] + (mn == 2 && leap ? 1 : 0);
    }
    days += d - 1;

    return days * 86400ULL + h * 3600ULL + m * 60ULL + s;
}

void time_init_cmos(void) {
    kernel_boot_epoch_sec = cmos_read_epoch();
    kprintf("[time] CMOS RTC epoch: %llu\n", (unsigned long long)kernel_boot_epoch_sec);
}

/* ---- clock helpers ---- */
static inline uint64_t ticks_to_ns(uint64_t ticks) {
    uint32_t freq = timer_get_frequency();
    if (freq == 0) freq = 100;
    return ticks * (1000000000ULL / freq);
}

void kernel_clock_gettime(int clockid, struct kernel_timespec *ts) {
    if (!ts) return;

    uint64_t ticks = timer_get_ticks();
    uint64_t ns    = ticks_to_ns(ticks);
    uint32_t freq  = timer_get_frequency();

    if (clockid == CLOCK_REALTIME) {
        uint64_t total_sec = kernel_boot_epoch_sec + (ticks / freq);
        uint64_t rem_ns    = ticks_to_ns(ticks % freq);
        ts->tv_sec  = (int64_t)total_sec;
        ts->tv_nsec = (int64_t)rem_ns;
    } else {
        /* MONOTONIC + fallback */
        ts->tv_sec  = (int64_t)(ns / 1000000000ULL);
        ts->tv_nsec = (int64_t)(ns % 1000000000ULL);
    }
}

void kernel_clock_getres(int clockid, struct kernel_timespec *ts) {
    if (!ts) return;
    uint32_t freq = timer_get_frequency();
    if (freq == 0) freq = 100;

    ts->tv_sec  = 0;
    ts->tv_nsec = 1000000000L / freq;   /* 10ms @ 100Hz */
}

/* ---- nanosleep ---- */
int kernel_nanosleep(const struct kernel_timespec *req, struct kernel_timespec *rem) {
    if (!req || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        return -EINVAL;
    }

    uint64_t freq = timer_get_frequency();
    if (freq == 0) freq = 100;

    uint64_t total_ticks = (uint64_t)req->tv_sec * freq +
                           (uint64_t)req->tv_nsec * freq / 1000000000ULL;

    if (total_ticks == 0) return 0;

    uint64_t start = timer_get_ticks();
    uint64_t deadline = start + total_ticks;

    tcb_t *cur = sched_current();

    while (timer_get_ticks() < deadline) {
        if (cur && (cur->sig_pending & ~cur->sig_mask)) {
            /* Interrupted by signal */
            if (rem) {
                uint64_t now = timer_get_ticks();
                uint64_t left = (now < deadline) ? (deadline - now) : 0;
                rem->tv_sec  = (int64_t)(left / freq);
                rem->tv_nsec = (int64_t)((left % freq) * (1000000000ULL / freq));
            }
            cur->sleep_deadline = 0;
            return -EINTR;
        }
        if (cur) {
            cur->sleep_deadline = deadline;
            cur->state = THREAD_BLOCKED;
        }
        sched_yield();
    }
    if (cur) cur->sleep_deadline = 0;
    return 0;
}

/* ---- gettimeofday / time ---- */
int kernel_gettimeofday(struct kernel_timeval *tv, void *tz) {
    (void)tz;
    if (!tv) return -EFAULT;

    struct kernel_timespec ts;
    kernel_clock_gettime(CLOCK_REALTIME, &ts);

    tv->tv_sec  = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

time_t kernel_time(time_t *t) {
    struct kernel_timespec ts;
    kernel_clock_gettime(CLOCK_REALTIME, &ts);

    if (t) *t = (time_t)ts.tv_sec;
    return (time_t)ts.tv_sec;
}

/* ---- itimer support ---- */
int kernel_getitimer(int which, struct itimer_state *val) {
    if (which < 0 || which > 2 || !val) return -EINVAL;

    tcb_t *cur = sched_current();
    if (!cur) return -ESRCH;

    val->interval_ticks = cur->itimers[which].interval_ticks;
    val->deadline_ticks = cur->itimers[which].deadline_ticks;
    return 0;
}

int kernel_setitimer(int which, const struct itimer_state *new, struct itimer_state *old) {
    if (which < 0 || which > 2 || !new) return -EINVAL;

    tcb_t *cur = sched_current();
    if (!cur) return -ESRCH;

    if (old) {
        old->interval_ticks = cur->itimers[which].interval_ticks;
        old->deadline_ticks = cur->itimers[which].deadline_ticks;
    }

    cur->itimers[which].interval_ticks = new->interval_ticks;
    cur->itimers[which].deadline_ticks = new->deadline_ticks;

    return 0;
}

/* Called from signal_tick() (PIT IRQ) for ITIMER_REAL */
void itimer_tick_real(uint64_t current_ticks) {
    tcb_t *list[64];
    int n = thread_get_all(list, 64);

    for (int i = 0; i < n; i++) {
        tcb_t *t = list[i];
        if (!t || t->state == THREAD_DEAD) continue;

        struct itimer_state *it = &t->itimers[ITIMER_REAL];
        if (it->deadline_ticks && current_ticks >= it->deadline_ticks) {
            signal_send(t, SIGALRM);

            if (it->interval_ticks) {
                it->deadline_ticks = current_ticks + it->interval_ticks;
            } else {
                it->deadline_ticks = 0; /* one-shot */
            }
        }
    }
}

/* Called from scheduler for VIRTUAL / PROF */
void itimer_account_cpu(uint64_t ticks, int in_kernel) {
    tcb_t *cur = sched_current();
    if (!cur) return;

    cur->cpu_ticks += ticks;

    /* ITIMER_VIRTUAL: only when running in userspace */
    if (!in_kernel) {
        struct itimer_state *vit = &cur->itimers[ITIMER_VIRTUAL];
        if (vit->deadline_ticks && cur->cpu_ticks >= vit->deadline_ticks) {
            signal_send(cur, SIGVTALRM);
            if (vit->interval_ticks) {
                vit->deadline_ticks = cur->cpu_ticks + vit->interval_ticks;
            } else {
                vit->deadline_ticks = 0;
            }
        }
    }

    /* ITIMER_PROF: always (user + kernel) */
    struct itimer_state *pit = &cur->itimers[ITIMER_PROF];
    if (pit->deadline_ticks && cur->cpu_ticks >= pit->deadline_ticks) {
        signal_send(cur, SIGPROF);
        if (pit->interval_ticks) {
            pit->deadline_ticks = cur->cpu_ticks + pit->interval_ticks;
        } else {
            pit->deadline_ticks = 0;
        }
    }
}
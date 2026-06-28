#include "kernel/time_types.h"
#ifndef AURALITE_KERNEL_TIME_H
#define AURALITE_KERNEL_TIME_H

#include <stdint.h>

/* POSIX timespec / timeval (kernel copy; must match libc) */
struct kernel_timespec {
    int64_t tv_sec;
    long    tv_nsec;
};

struct kernel_timeval {
    int64_t tv_sec;
    long    tv_usec;
};

/* Clock IDs */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3

/* ITIMER which */
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

/* Per-process itimer state (stored in tcb_t) */
struct itimer_state {
    uint64_t interval_ticks;
    uint64_t deadline_ticks;
};

/* Global boot epoch (seconds since Unix epoch, UTC) */
extern uint64_t kernel_boot_epoch_sec;

/* Initialize CMOS RTC epoch at boot (called from pit_init) */
void time_init_cmos(void);

/* Core clock functions */
void kernel_clock_gettime(int clockid, struct kernel_timespec *ts);
void kernel_clock_getres(int clockid, struct kernel_timespec *ts);

/* nanosleep: returns 0 on success, -EINTR on signal, fills rem if interrupted */
int kernel_nanosleep(const struct kernel_timespec *req, struct kernel_timespec *rem);

/* gettimeofday / time */
int kernel_gettimeofday(struct kernel_timeval *tv, void *tz);
time_t kernel_time(time_t *t);

/* itimer support */
int kernel_getitimer(int which, struct itimer_state *val);
int kernel_setitimer(int which, const struct itimer_state *new, struct itimer_state *old);

/* Called from PIT IRQ (signal_tick) for ITIMER_REAL */
void itimer_tick_real(uint64_t current_ticks);

/* Called from scheduler for VIRTUAL/PROF */
void itimer_account_cpu(uint64_t ticks, int in_kernel);

#endif /* AURALITE_KERNEL_TIME_H */
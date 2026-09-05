/* pit.c — 8254 Programmable Interval Timer driver + global tick counter.
 *
 * Channel 0 is programmed in mode 3 (square-wave generator) with a divisor
 * derived from the PIT's 1193182 Hz base clock. Its output feeds IRQ 0, whose
 * handler increments a volatile monotonic counter. timer_sleep_ms spins on
 * that counter, using `hlt` to idle the CPU between ticks.
 */

#include <stdint.h>
#include "drivers/timer/pit.h"
#include "kernel/arch/arch.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/lib/selftest.h"
#include "kernel/gui/gui.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/signal.h"
#include "kernel/lib/kprintf.h"
#include "kernel/fs/buffer_cache.h"   /* RESIDUE2 T3: bc_tick() 1 Hz drain */

#define TIMER_TAG "[timer] "

/*
 * Command byte for channel 0:
 *   bits 7-6 = 00  -> select channel 0
 *   bits 5-4 = 11  -> access mode: lobyte then hibyte
 *   bits 3-1 = 010 -> operating mode 2 (rate generator)
 *   bit  0    = 0  -> binary counting (not BCD)
 *   => 0b00110100 = 0x34
 *
 * MODE 2, NOT MODE 3 — a measured lesson (2026-08-21).  Mode 3's
 * square wave has TWO output transitions per period, and QEMU 10's
 * PIT delivers a vector-32 interrupt for each of them: the guest
 * measured 13.0M TSC between IRQs in mode 3 and exactly 26.0M in
 * mode 2 (same divisor 11932), i.e. mode 3 ticked at 200 Hz while
 * claiming 100.  /proc/uptime ran 2.02x wall clock (20 s real -> 40.4 s
 * guest, measured via a timed serial session) — the "OS clock runs
 * twice as fast" bug.  Mode 2 emits ONE pulse per period, which is
 * why every production kernel programs channel 0 with 0x34; the
 * interrupt rate now matches the programmed rate on QEMU and on the
 * datasheet alike. */
#define PIT_CMD_CHAN0_LOHI_MODE2 0x34

static volatile uint64_t timer_ticks    = 0;
static uint32_t          timer_freq_hz  = 0;

/* Vector-32 handler: bump the monotonic counter, then drive the scheduler.
 * Minimal work at interrupt level (safety rule 9): just a counter increment
 * and a quantum check.  sched_tick is a no-op until the scheduler is ready.
 *
 * This one vector has two hardware sources (SMP step 3.2):
 *   - on the BSP: the legacy PIT, delivered as IRQ0 via LINT0/ExtINT;
 *   - on every AP: that CPU's own Local APIC timer (periodic, calibrated in
 *     smp.c), deliberately re-using vector 32 so dispatch lands here.
 * The wall-clock book-keeping (timer_ticks, alarm/nanosleep deadlines via
 * signal_tick) must advance at exactly 100 Hz SYSTEM-WIDE, so only the BSP
 * bumps it; AP ticks drive their own cpu's scheduler (preemption, per-cpu
 * usage accounting) without touching the shared time base. */
static void timer_irq_handler(struct registers *regs) {
    (void)regs;
    struct cpu_local *me = cpu_local_ready ? get_cpu_local() : NULL;
    if (!me || me->cpu_id == 0) {
        timer_ticks++;
        if (sched_is_ready()) {
            signal_tick(timer_ticks);   /* fire elapsed alarm() deadlines (SIGALRM) */
        }
        /* OPT_PLAN.md O4: the compositor sleeps between events now; the
         * taskbar clock and notification expiry need one wake a second.
         * gui_poke() is a no-op until gui_init() and IRQ-safe after the
         * O4 wait_queue fix. */
        if (timer_freq_hz != 0 && (timer_ticks % timer_freq_hz) == 0) {
            gui_poke();
            /* RESIDUE2 T3 (writeback): the seconds tick is also the
             * buffer cache's periodic drain — a hard kill of the VM can
             * lose at most ~1 s of deferred stores.  No-op before
             * bc_init(). */
            bc_tick();
        }
    }
    if (sched_is_ready()) {
        sched_tick();
    }
}

void pit_init(uint32_t frequency) {
    /* Clamp to the valid 16-bit divisor range (1..65535). */
    if (frequency == 0) {
        frequency = PIT_DEFAULT_FREQUENCY;
    }
    if (frequency > PIT_BASE_FREQUENCY) {
        frequency = (uint32_t)PIT_BASE_FREQUENCY;
    }

    /* Divisor = base_clock / target_freq.  Round to nearest for best accuracy. */
    uint32_t divisor = (uint32_t)((PIT_BASE_FREQUENCY + frequency / 2) / frequency);
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 65535) {
        divisor = 65535;
    }

    /* Record the ACTUAL frequency the hardware will produce (divisor-rounded). */
    timer_freq_hz = (uint32_t)(PIT_BASE_FREQUENCY / divisor);

    /* Program channel 0: command, then low byte, then high byte. */
    outb(PIT_MODE_CMD, PIT_CMD_CHAN0_LOHI_MODE2);
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    /* P8: initialize CMOS RTC epoch before unmasking IRQ0, so early timer
     * interrupts cannot run signal/scheduler tick code while RTC probing is in
     * progress. */
    extern void time_init_cmos(void);
    time_init_cmos();

    /* Hook IRQ 0 and unmask it (irq_register_handler unmasks automatically). */
    timer_ticks = 0;
    irq_register_handler(TIMER_IRQ, timer_irq_handler);

    kprintf(TIMER_TAG "PIT channel 0: mode 2, divisor %u -> %u Hz"
            " (%u Hz requested)\n",
            (unsigned)divisor, (unsigned)timer_freq_hz, (unsigned)frequency);
}

uint64_t timer_get_ticks(void) {
    return timer_ticks;
}

uint32_t timer_get_frequency(void) {
    return timer_freq_hz;
}

void timer_sleep_ms(uint64_t ms) {
    if (timer_freq_hz == 0 || ms == 0) {
        return;
    }
    /* Ticks to wait, rounded: (ms * freq + 500) / 1000 for nearest rounding. */
    uint64_t ticks_to_wait = (ms * (uint64_t)timer_freq_hz + 500ULL) / 1000ULL;
    if (ticks_to_wait == 0) {
        ticks_to_wait = 1;
    }
    uint64_t target = timer_ticks + ticks_to_wait;

    /* Before the scheduler exists (this is also called from
     * timer_self_test() during early boot, pre-sched_init()), there is no
     * thread to block: fall back to the original busy-wait. Once threads
     * are running, genuinely block via sleep_deadline (the same mechanism
     * kernel_nanosleep()/the stdin SYS_READ path use) instead of spinning
     * with "sti; pause": every background poller in the kernel (GUI
     * compositor, USB hotplug monitor, HID polling, ...) calls this
     * function on its own kernel thread, so a plain busy-wait here meant
     * the CPU never actually reached the idle loop as long as any of them
     * were alive -- making /proc/loadavg (and anything reading it, like a
     * system monitor) report 100% "busy" nearly all the time, regardless
     * of real load. */
    if (sched_is_ready()) {
        tcb_t *cur = sched_current();
        if (cur) {
            while (timer_ticks < target) {
                uint64_t rflags;
                __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));
                if (timer_ticks >= target) {
                    if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
                    break;
                }
                cur->sleep_deadline = target;
                cur->state = THREAD_BLOCKED;
                schedule();
                if (rflags & 0x200ULL) {
                    __asm__ volatile ("sti" ::: "memory");
                }
            }
            cur->sleep_deadline = 0;
            return;
        }
    }

    while (timer_ticks < target) {
        /* In SMP/QEMU the legacy timer interrupt may be delivered to another
         * vCPU, so HLT on the current CPU can sleep forever even though the
         * global tick counter is advancing.  Keep IF enabled but poll with
         * PAUSE for boot/self-test sleeps (no scheduler yet to block on). */
        __asm__ volatile ("sti; pause" ::: "memory");
    }
}

void timer_self_test(void) {
    /* OPT_PLAN.md O2: the VERIFICATION window follows the self-test
     * knob; the divisor programming above is untouched.  FULL keeps the
     * historical 1-second wait, byte-identical output, +/-5% band.  FAST
     * verifies over 100 ms -- ~10 ticks at 100 Hz, so the band widens to
     * +/-20% to absorb +/-1-tick quantisation; a divisor that is wrong
     * by 10x (the failure this test exists for) still misses it by
     * miles.  OFF skips loudly. */
    uint64_t window_ms = selftest_scale(1000, 100);
    if (window_ms == 0) {
        kprintf(TIMER_TAG "self-test: SKIPPED (selftest=off)\n");
        return;
    }
    if (window_ms == 1000) {
        kprintf(TIMER_TAG "self-test: measuring 1-second delay...\n");
    } else {
        kprintf(TIMER_TAG "self-test: measuring %llu-ms delay...\n",
                (unsigned long long)window_ms);
    }

    uint64_t start_ticks = timer_ticks;
    /* Sleep the window using a busy-wait independent of the tick counter:
       this makes the test self-contained.  We read the counter before and
       after and verify the tick delta matches the configured frequency. */
    timer_sleep_ms(window_ms);
    uint64_t end_ticks = timer_ticks;
    uint64_t elapsed   = end_ticks - start_ticks;

    /* Expected ticks over the window.  +/-5% band at 1 s, +/-20% at
       100 ms (see above). */
    uint64_t expected = (uint64_t)timer_freq_hz * window_ms / 1000;
    uint64_t slack = (window_ms == 1000) ? expected / 20 : expected / 5;
    if (slack == 0) slack = 1;
    uint64_t lo = expected - slack;
    uint64_t hi = expected + slack;

    kprintf(TIMER_TAG "expected ~%llu ticks, measured %llu ticks (band %llu-%llu)\n",
            (unsigned long long)expected,
            (unsigned long long)elapsed,
            (unsigned long long)lo,
            (unsigned long long)hi);

    if (elapsed >= lo && elapsed <= hi) {
        /* Integer percentage of the expected count over the window. */
        uint64_t pct = (elapsed * 100 + expected / 2) / expected;
        if (window_ms == 1000) {
            kprintf(TIMER_TAG "PASS: %llu ticks in 1s (%llu%% of %u Hz)\n",
                    (unsigned long long)elapsed,
                    (unsigned long long)pct,
                    (unsigned)timer_freq_hz);
        } else {
            kprintf(TIMER_TAG "PASS: %llu ticks in %llums (%llu%% of expected)\n",
                    (unsigned long long)elapsed,
                    (unsigned long long)window_ms,
                    (unsigned long long)pct);
        }
    } else {
        kprintf(TIMER_TAG "FAIL: tick count %llu outside %llu-%llu band\n",
                (unsigned long long)elapsed,
                (unsigned long long)lo,
                (unsigned long long)hi);
    }
}

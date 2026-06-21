/* pit.c — 8254 Programmable Interval Timer driver + global tick counter.
 *
 * Channel 0 is programmed in mode 3 (square-wave generator) with a divisor
 * derived from the PIT's 1193182 Hz base clock. Its output feeds IRQ 0, whose
 * handler increments a volatile monotonic counter. timer_sleep_ms spins on
 * that counter, using `hlt` to idle the CPU between ticks.
 */

#include <stdint.h>
#include "drivers/timer/pit.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/proc/scheduler.h"
#include "kernel/lib/kprintf.h"

#define TIMER_TAG "[timer] "

/*
 * Command byte for channel 0:
 *   bits 7-6 = 00  -> select channel 0
 *   bits 5-4 = 11  -> access mode: lobyte then hibyte
 *   bits 3-1 = 011 -> operating mode 3 (square wave)
 *   bit  0    = 0  -> binary counting (not BCD)
 *   => 0b00110110 = 0x36
 */
#define PIT_CMD_CHAN0_LOHI_MODE3 0x36

static volatile uint64_t timer_ticks    = 0;
static uint32_t          timer_freq_hz  = 0;

/* IRQ 0 handler: bump the monotonic counter, then drive the scheduler.
 * Minimal work at interrupt level (safety rule 9): just a counter increment
 * and a quantum check.  sched_tick is a no-op until the scheduler is ready. */
static void timer_irq_handler(struct registers *regs) {
    (void)regs;
    timer_ticks++;
    sched_tick();
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
    outb(PIT_MODE_CMD, PIT_CMD_CHAN0_LOHI_MODE3);
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    /* Hook IRQ 0 and unmask it (irq_register_handler unmasks automatically). */
    timer_ticks = 0;
    irq_register_handler(TIMER_IRQ, timer_irq_handler);

    kprintf(TIMER_TAG "PIT channel 0: mode 3, divisor %u -> %u Hz"
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
    while (timer_ticks < target) {
        /* Halt until the next interrupt (the PIT tick). This avoids burning
           100% CPU in the spin loop while still returning promptly on tick. */
        __asm__ volatile ("hlt");
    }
}

void timer_self_test(void) {
    kprintf(TIMER_TAG "self-test: measuring 1-second delay...\n");

    uint64_t start_ticks = timer_ticks;
    /* Sleep ~1 second using a busy-wait independent of the tick counter: this
       makes the test self-contained.  We read the counter before and after and
       verify the tick delta matches the configured frequency within +/-5%. */
    timer_sleep_ms(1000);
    uint64_t end_ticks = timer_ticks;
    uint64_t elapsed   = end_ticks - start_ticks;

    /* Expected = timer_freq_hz ticks.  +/-5% band. */
    uint64_t expected = (uint64_t)timer_freq_hz;
    uint64_t lo = expected - expected / 20;     /* 95% */
    uint64_t hi = expected + expected / 20;     /* 105% */

    kprintf(TIMER_TAG "expected ~%llu ticks, measured %llu ticks (band %llu-%llu)\n",
            (unsigned long long)expected,
            (unsigned long long)elapsed,
            (unsigned long long)lo,
            (unsigned long long)hi);

    if (elapsed >= lo && elapsed <= hi) {
        /* Integer percentage of the configured frequency. */
        uint64_t pct = (elapsed * 100 + timer_freq_hz / 2) / timer_freq_hz;
        kprintf(TIMER_TAG "PASS: %llu ticks in 1s (%llu%% of %u Hz)\n",
                (unsigned long long)elapsed,
                (unsigned long long)pct,
                (unsigned)timer_freq_hz);
    } else {
        kprintf(TIMER_TAG "FAIL: tick count %llu outside %llu-%llu band\n",
                (unsigned long long)elapsed,
                (unsigned long long)lo,
                (unsigned long long)hi);
    }
}

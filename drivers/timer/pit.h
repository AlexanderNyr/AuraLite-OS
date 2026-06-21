#ifndef AURALITE_DRIVERS_TIMER_PIT_H
#define AURALITE_DRIVERS_TIMER_PIT_H

#include <stdint.h>

/*
 * 8254 Programmable Interval Timer driver.
 *
 * PIT channel 0 is wired to IRQ 0 (remapped to CPU vector 32 by the PIC in
 * Phase 2). We program it in mode 3 (square wave) at a configurable frequency,
 * register an IRQ 0 handler that increments a global monotonic tick counter,
 * and provide a busy-wait sleep based on that counter.
 *
 * The LAPIC timer (per-CPU, needed for SMP) is a Phase 6 follow-up; the PIT is
 * sufficient and more reliable for the single-CPU gate criterion.
 */

/* 8254 I/O ports (Intel 82C54 datasheet). */
#define PIT_CHANNEL0_DATA 0x40
#define PIT_CHANNEL1_DATA 0x41
#define PIT_CHANNEL2_DATA 0x42
#define PIT_MODE_CMD      0x43

/* The PIT's fixed input clock: 1193182 Hz (derived from the NTSC colorburst). */
#define PIT_BASE_FREQUENCY 1193182ULL

/* Default tick rate: 100 Hz -> ~10ms granularity. */
#define PIT_DEFAULT_FREQUENCY 100

/* IRQ 0 carries the PIT channel-0 interrupt (remapped to vector 32). */
#define TIMER_IRQ 0

/*
 * Configure channel 0 in mode 3 (square wave) at the given frequency, register
 * the IRQ 0 handler, and unmask IRQ 0.  `frequency` is clamped to [1, 65535].
 */
void pit_init(uint32_t frequency);

/* Global monotonic tick count since pit_init(). */
uint64_t timer_get_ticks(void);

/* Configured ticks-per-second. */
uint32_t timer_get_frequency(void);

/*
 * Busy-wait (with `hlt`) for at least `ms` milliseconds.  Requires interrupts
 * to be enabled so the PIT can advance the counter.
 */
void timer_sleep_ms(uint64_t ms);

/* Gate self-test: sleep 1 second, verify the measured tick rate is within
 * +/-5% of the configured frequency. */
void timer_self_test(void);

#endif /* AURALITE_DRIVERS_TIMER_PIT_H */

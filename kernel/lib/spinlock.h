#ifndef NOVOS_LIB_SPINLOCK_H
#define NOVOS_LIB_SPINLOCK_H

#include <stdint.h>

/*
 * Test-and-set spinlock for kernel data shared across execution contexts.
 *
 *   spinlock_acquire / _release        — plain; caller guarantees no
 *                                        self-deadlock from an interrupt.
 *   spinlock_acquire_irqsave / _restore — also saves/restores RFLAGS.IF, for
 *                                         data touched (now or later) from IRQ
 *                                         context. Safe to nest with code that
 *                                         already runs with interrupts off.
 *
 * No fairness/tickets yet; sufficient for the pre-SMP kernel. Marked NOT SMP
 * SAFE in the sense that per-CPU setups (Phase 12) will add per-CPU data and
 * smarter contention handling.
 */

typedef struct {
    volatile uint8_t locked;
} spinlock_t;

void spinlock_init(spinlock_t *lock);
void spinlock_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);

/* Returns the RFLAGS captured before disabling interrupts; pass to _restore. */
uint64_t spinlock_acquire_irqsave(spinlock_t *lock);
void     spinlock_release_irqrestore(spinlock_t *lock, uint64_t rflags);

#endif /* NOVOS_LIB_SPINLOCK_H */

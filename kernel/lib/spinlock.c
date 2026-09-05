/* spinlock.c — test-and-set spinlocks (C11 atomics since the V6 sweep).
 *
 * The x86_64 original was LOCK CMPXCHG + pause + a plain-store
 * release, all inline asm.  The C11 spelling below lowers to the
 * same shape on x86 (lock cmpxchg / pause via arch_cpu_relax / a
 * mov-store: release ordering is free on x86's TSO memory model) and
 * to lr.b/sc.b + fence on riscv64 -- one source, three targets, no
 * __asm__ outside kernel/arch/ (RISCV_PLAN V6, ratchet 4's opening
 * payment).
 *
 * The irqsave pair rides arch_irq_save/arch_irq_restore (arch.h):
 * pushfq;cli / sti on x86, csrrc/csrs sstatus.SIE on riscv64.
 */

#include <stdatomic.h>

#include "kernel/lib/spinlock.h"
#include "kernel/arch/arch.h"

void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
}

void spinlock_acquire(spinlock_t *lock) {
    for (;;) {
        uint8_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(
                (_Atomic uint8_t *)&lock->locked, &expected, 1,
                memory_order_acquire, memory_order_relaxed)) {
            break;                 /* we observed 0 and atomically stored 1 */
        }
        while (atomic_load_explicit((_Atomic uint8_t *)&lock->locked,
                                    memory_order_relaxed)) {
            arch_cpu_relax();      /* spin on a cached read until released */
        }
    }
}

int spinlock_try_acquire(spinlock_t *lock) {
    uint8_t expected = 0;
    return atomic_compare_exchange_strong_explicit(
        (_Atomic uint8_t *)&lock->locked, &expected, 1,
        memory_order_acquire, memory_order_relaxed) ? 1 : 0;
}

void spinlock_release(spinlock_t *lock) {
    atomic_store_explicit((_Atomic uint8_t *)&lock->locked, 0,
                          memory_order_release);
}

uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
    arch_irqflags_t flags = arch_irq_save();
    spinlock_acquire(lock);
    return flags;
}

void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) {
    spinlock_release(lock);
    arch_irq_restore(flags);
}

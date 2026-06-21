/* spinlock.c — x86_64 test-and-set spinlocks. */

#include "kernel/lib/spinlock.h"

static inline void cpu_pause(void) {
    /* Hint to the CPU that we are in a spin-wait loop (saves power / avoids
       a memory-ordering violation on hyperthreaded cores). Intel SDM Vol.2. */
    __asm__ volatile ("pause");
}

void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
}

void spinlock_acquire(spinlock_t *lock) {
    for (;;) {
        uint8_t expected = 0;
        /* LOCK CMPXCHG: if *lock == AL(0) then *lock = 1, ZF=1; else AL=*lock.
           The "+a" constraint ties `expected` to AL and reads back the result. */
        __asm__ volatile (
            "lock cmpxchg %[new], %[lock]"
            : [lock] "+m" (lock->locked), "+a" (expected)
            : [new] "q" ((uint8_t)1)
            : "cc", "memory");
        if (expected == 0) {
            break;                 /* we observed 0 and atomically stored 1 */
        }
        while (lock->locked) {     /* spin on a cached read until released */
            cpu_pause();
        }
    }
}

void spinlock_release(spinlock_t *lock) {
    /* A plain store with a compiler barrier suffices; x86 stores have
       release semantics for aligned byte stores. */
    __asm__ volatile ("movb $0, %[lock]" :: [lock] "m" (lock->locked) : "memory");
}

uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
    uint64_t rflags;
    /* Capture RFLAGS then clear IF atomically with respect to further code. */
    __asm__ volatile ("pushfq; popq %0; cli" : "=rm" (rflags) :: "memory");
    spinlock_acquire(lock);
    return rflags;
}

void spinlock_release_irqrestore(spinlock_t *lock, uint64_t rflags) {
    spinlock_release(lock);
    /* Only re-enable interrupts if they were enabled on entry. */
    if (rflags & 0x200ULL) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

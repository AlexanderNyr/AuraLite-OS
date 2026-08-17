/* kernel/arch/riscv64/irqflags.h -- interrupt masking + spin-wait
 * primitives (RISCV_PLAN V6, D6).  riscv64 backend.
 *
 * The reason this header family exists: cli/sti/hlt/pause have no
 * meaning here.  The same four contracts are sstatus.SIE csr ops, a
 * wfi, and a fence -- and with these four defined, the portable
 * files' spin/sleep/critical-section idioms compile unchanged on the
 * third architecture.
 */

#ifndef AURALITE_ARCH_RISCV64_IRQFLAGS_H
#define AURALITE_ARCH_RISCV64_IRQFLAGS_H

#include <stdint.h>

typedef uint64_t arch_irqflags_t;

#define ARCH_RV_SSTATUS_SIE (1UL << 1)

static inline arch_irqflags_t arch_irq_save(void)
{
    uint64_t prev;
    /* csrrc: read old sstatus, clear SIE -- one instruction, no
     * window between the read and the mask (the pushfq;cli pair's
     * atomicity, achieved by ISA design instead of adjacency). */
    __asm__ volatile ("csrrc %0, sstatus, %1"
                      : "=r"(prev) : "r"(ARCH_RV_SSTATUS_SIE) : "memory");
    return prev;
}

static inline void arch_irq_restore(arch_irqflags_t flags)
{
    if (flags & ARCH_RV_SSTATUS_SIE) {
        __asm__ volatile ("csrs sstatus, %0"
                          :: "r"(ARCH_RV_SSTATUS_SIE) : "memory");
    }
}

/* wfi wakes on a pending-and-enabled interrupt; with SIE set this is
 * the sti;hlt contract in one instruction (wfi with SIE clear parks
 * until an interrupt is PENDING, then falls through without taking
 * it -- different machine, same portable meaning: "sleep until
 * something happens"). */
static inline void arch_wait_for_interrupt(void)
{
    __asm__ volatile ("csrsi sstatus, 2");
    __asm__ volatile ("wfi" ::: "memory");
}

/* No pause hint in rv64gc (Zihintpause is optional and virt lacks
 * it); a fence keeps the spin loop from saturating the memory
 * system, which is the half of pause's contract that matters off
 * hyperthreads. */
static inline void arch_cpu_relax(void)
{
    __asm__ volatile ("fence rw, rw" ::: "memory");
}

#endif /* AURALITE_ARCH_RISCV64_IRQFLAGS_H */

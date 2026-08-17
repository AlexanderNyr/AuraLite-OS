/* kernel/arch/i386/irqflags.h -- interrupt masking + spin-wait
 * primitives (RISCV_PLAN V6, D6).  i386 backend.
 *
 * EFLAGS is 32 bits but the saved-state type is uint64_t on every
 * backend -- portable signatures must not change width per arch (the
 * I6 lesson generalised).  pushfd's value zero-extends; the IF test
 * reads the same bit 9.
 */

#ifndef AURALITE_ARCH_I386_IRQFLAGS_H
#define AURALITE_ARCH_I386_IRQFLAGS_H

#include <stdint.h>

typedef uint64_t arch_irqflags_t;

static inline arch_irqflags_t arch_irq_save(void)
{
    uint32_t eflags;
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(eflags) :: "memory");
    return (arch_irqflags_t)eflags;
}

static inline void arch_irq_restore(arch_irqflags_t flags)
{
    if (flags & 0x200ULL) {                    /* EFLAGS.IF */
        __asm__ volatile ("sti" ::: "memory");
    }
}

/* The sti;hlt fusion is measured history on this arch: I7's first
 * boot deadlocked on a bare hlt inside an interrupt gate (IF clear),
 * a prompt on screen and the machine asleep under it. */
static inline void arch_wait_for_interrupt(void)
{
    __asm__ volatile ("sti; hlt" ::: "memory");
}

static inline void arch_cpu_relax(void)
{
    __asm__ volatile ("pause" ::: "memory");
}

#endif /* AURALITE_ARCH_I386_IRQFLAGS_H */

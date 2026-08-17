/* kernel/arch/x86_64/irqflags.h -- interrupt masking + spin-wait
 * primitives (RISCV_PLAN V6, D6).
 *
 * The x86_64 backend for arch.h's second forwarding block.  The
 * bodies are the exact instruction sequences the portable files
 * carried inline before the V6 sweep -- same constraints, same
 * clobbers, same IF-bit test -- so migrating a call site is a rename,
 * and the byte-identity control on the migration batches stays
 * satisfiable.  (cpu.h's irq_save/irq_restore predate this header
 * and remain for its existing users; this is the one the SWEEP
 * migrates portable code onto, alongside the wait/relax pair cpu.h
 * never had.)
 */

#ifndef AURALITE_ARCH_X86_64_IRQFLAGS_H
#define AURALITE_ARCH_X86_64_IRQFLAGS_H

#include <stdint.h>

/* Saved interrupt state: RFLAGS here.  All three backends agree on
 * the width (uint64_t) so portable signatures never change per arch;
 * only bit meanings differ, and only the backend looks at bits. */
typedef uint64_t arch_irqflags_t;

static inline arch_irqflags_t arch_irq_save(void)
{
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
    return rflags;
}

static inline void arch_irq_restore(arch_irqflags_t flags)
{
    if (flags & 0x200ULL) {                    /* RFLAGS.IF */
        __asm__ volatile ("sti" ::: "memory");
    }
}

/* Sleep until the next interrupt, interrupts ENABLED for the wait --
 * the sti;hlt idiom (both idle loops use it; the I7 keyboard bug is
 * why the sti is fused rather than separate). */
static inline void arch_wait_for_interrupt(void)
{
    __asm__ volatile ("sti; hlt" ::: "memory");
}

/* Spin-wait hint (Intel SDM Vol.2: saves power, avoids the memory-
 * order machine clear on hyperthreaded parts). */
static inline void arch_cpu_relax(void)
{
    __asm__ volatile ("pause" ::: "memory");
}

#endif /* AURALITE_ARCH_X86_64_IRQFLAGS_H */

/* kernel/arch/aarch64/irqflags.h -- interrupt masking + spin-wait
 * primitives (RISCV_PLAN V6, D6; fourth backend per ARM64_PLAN A6).
 *
 * The same four contracts, DAIF spelling.  cli/sti/hlt/pause and
 * csrrc/wfi/fence mean nothing here; mrs/msr on DAIF, wfi and yield
 * carry the identical portable meanings -- and with these four
 * defined, every portable spin/sleep/critical-section idiom that
 * survived the V6 sweep compiles unchanged on the fourth ISA.
 *
 * One honest difference from the riscv backend, documented rather
 * than hidden: aarch64 has no single-instruction read-and-mask like
 * `csrrc`.  arch_irq_save is TWO instructions -- `mrs` (read DAIF),
 * then `msr daifset, #2` (mask IRQ).  The window between them is
 * safe for the same reason the x86 `pushfq; cli` pair is safe: an
 * interrupt taken inside the window is DELIVERED, not lost -- it
 * runs early, before the critical section begins, and the saved
 * flags are still the caller's truth (the handler restores DAIF on
 * eret).  What the window can never do is leave the section entered
 * with IRQs unexpectedly on: the daifset lands before the caller's
 * next instruction, unconditionally.
 */

#ifndef AURALITE_ARCH_AARCH64_IRQFLAGS_H
#define AURALITE_ARCH_AARCH64_IRQFLAGS_H

#include <stdint.h>

/* Saved interrupt state: the DAIF register (bits D=9 A=8 I=7 F=6).
 * All four backends agree on the width (uint64_t) so portable
 * signatures never change per arch; only bit meanings differ, and
 * only the backend looks at bits.  Note the polarity flip vs the
 * others: DAIF.I SET means MASKED (x86 IF and sstatus.SIE set mean
 * enabled) -- one more reason no portable file may peek inside. */
typedef uint64_t arch_irqflags_t;

#define ARCH_A64_DAIF_IRQ (1UL << 7)   /* the I bit: 1 = IRQ masked */

static inline arch_irqflags_t arch_irq_save(void)
{
    uint64_t daif;
    __asm__ volatile ("mrs %0, daif" : "=r"(daif) :: "memory");
    __asm__ volatile ("msr daifset, #2" ::: "memory");
    return daif;
}

static inline void arch_irq_restore(arch_irqflags_t flags)
{
    if (!(flags & ARCH_A64_DAIF_IRQ)) {        /* was UNmasked before */
        __asm__ volatile ("msr daifclr, #2" ::: "memory");
    }
}

/* Sleep until the next interrupt with IRQ unmasked for the wait --
 * the sti;hlt contract, DAIF spelling.  The unmask is one
 * instruction before the wfi, not fused, and that is fine on this
 * machine: wfi wakes on a PENDING interrupt even while masked, so
 * the I7 lost-wakeup shape (interrupt fires in the window, sleep
 * forever) cannot happen -- the wfi falls straight through and the
 * now-unmasked IRQ is taken.  This is the exact two-instruction
 * sequence A5c's cons_a64_readline measured its way to (the
 * 45-second prompt-flood log); the header is where it becomes the
 * contract instead of an inline idiom. */
static inline void arch_wait_for_interrupt(void)
{
    __asm__ volatile ("msr daifclr, #2" ::: "memory");
    __asm__ volatile ("wfi" ::: "memory");
}

/* Spin-wait hint: `yield` is architecturally a NOP that tells the
 * core (or an SMT sibling / hypervisor) the thread is spinning --
 * pause's contract verbatim (ARM DDI 0487, YIELD). */
static inline void arch_cpu_relax(void)
{
    __asm__ volatile ("yield" ::: "memory");
}

#endif /* AURALITE_ARCH_AARCH64_IRQFLAGS_H */

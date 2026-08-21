#ifndef AURALITE_ARCH_X86_64_DIAGNOSTICS_H
#define AURALITE_ARCH_X86_64_DIAGNOSTICS_H

#include <stdint.h>
#include "kernel/arch/x86_64/isr.h"

/*
 * diagnostics.c — FIX_R0 (FIXES_PLAN.md): make failures visible.
 *
 * Three pieces live here:
 *
 *   1. A panic/fault dump path that survives a bad stack: an entirely
 *      lock-free, allocation-free, serial-only printer used to emit the
 *      register state, the faulting RIP and a (probed) stack trace BEFORE
 *      the handler touches anything that might fault or hang again —
 *      the kprintf spinlock, the framebuffer console, or a broken
 *      rbp chain.  Stack-walk reads are probed against the kernel page
 *      tables so a garbage frame pointer truncates the trace instead of
 *      escalating the fault.
 *
 *   2. diag_cpu_id(), so every panic and exception message can name the
 *      CPU it happened on (required by the FIX_R2 investigation).
 *
 *   3. The boot-time IST self-check, reporting whether the Interrupt Stack
 *      Table is actually armed (currently it is not — FIX_R1 arms it) so
 *      that change is visible in the boot log, not only in the source.
 */

/* Best-effort current CPU number.  Returns 0 before per-CPU state exists
 * (early boot: only the BSP is running then, so 0 is the truth). */
uint32_t diag_cpu_id(void);

/* Lock-free, allocation-free, bounded-wait serial output (COM1).
 * Safe to call from exception context with interrupts off, with the
 * console lock held, and before uart_init().  Intended for the panic and
 * exception paths only; normal logging goes through kprintf(). */
void diag_early_puts(const char *s);
void diag_early_puthex(uint64_t v);       /* "0x" + 16 digits            */
void diag_early_putdec(uint64_t v);

/* Emit the complete fatal-kernel-exception dump (banner, CPU id, error
 * code, faulting RIP, full register state, CR2 for #PF, probed stack
 * trace) on the serial console, without locks and with probed reads.
 * Called for kernel-mode exceptions BEFORE any kprintf in the handler. */
void diag_early_dump(const struct registers *r, const char *exception_name);

/* Boot-time self-check: report whether the IST is armed for #DF and how
 * many per-CPU IST1 stacks are programmed.  Prints exactly one line. */
void diag_ist_self_check(void);

/* HW_PLAN H0: the CPU feature receipt lines ([cpu] features / IA32_PAT)
 * -- printed at boot, gated by the H-phases.  Lives here because
 * cpuid/rdmsr are x86 by nature (width-sweep ratchet 2 enforced it). */
void diag_cpu_feature_receipts(void);

/* Deliberate kernel fault for the FIX_R0 test gate, triggered from
 * /proc/sysrq-trigger: a write to unmapped address 0.  Returns only if
 * the fault unexpectedly did not happen (which the test then reports). */
void diag_trigger_kernel_fault(void);

/* Deliberate kernel-stack overflow for the FIX_R1 test gate ('o' in
 * /proc/sysrq-trigger): deep recursion whose fatal frame lands in the
 * stack's guard page with RSP already invalid, forcing a #DF. */
void diag_trigger_kernel_stack_overflow(void);

#endif /* AURALITE_ARCH_X86_64_DIAGNOSTICS_H */

/* kernel/arch/i386/isr32.c -- exception/IRQ dispatcher for the i386
 * kernel (I386_PLAN I2).  Sibling of kernel/arch/x86_64/isr.c, at
 * bring-up scope: named exception diagnostics in the FIX_R0 format
 * (register dump + CPU number), IRQ fan-out to registered handlers,
 * and a halt on unhandled kernel faults.  Ring 3 signal translation
 * arrives with Ring 3 itself in I4.
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/i386/isr.h"
#include "kernel/arch/i386/irq32.h"
#include "kernel/arch/i386/kprintf32.h"

/* Intel SDM Vol.3, 6-15: mnemonic for each CPU exception vector.
 * Same table as the x86_64 dispatcher -- the vectors did not change
 * between widths, only the frame around them did. */
static const char *exception_messages[32] = {
    "Division by Zero",            "Debug",
    "Non-Maskable Interrupt",      "Breakpoint",
    "Overflow",                    "Bound Range Exceeded",
    "Invalid Opcode",              "Device Not Available",
    "Double Fault",                "Coprocessor Segment Overrun",
    "Invalid TSS",                 "Segment Not Present",
    "Stack-Segment Fault",         "General Protection Fault",
    "Page Fault",                  "Reserved",
    "x87 Floating-Point Exception","Alignment Check",
    "Machine Check",               "SIMD Floating-Point Exception",
    "Virtualization Exception",    "Control Protection Exception",
    "Unknown", "Unknown", "Unknown", "Unknown",
    "Unknown", "Unknown", "Unknown", "Unknown",
    "Unknown", "Unknown"
};

/* Optional hook: I2's boot self-test arms this to prove a deliberate
 * fault produces a named diagnostic and execution continues. */
static volatile int expect_breakpoint = 0;
static volatile int breakpoint_seen   = 0;

void isr32_expect_breakpoint(void) { expect_breakpoint = 1; }
int  isr32_breakpoint_seen(void)   { return breakpoint_seen; }

static void dump_frame(const struct registers32 *r)
{
    /* FIX_R0 discipline: name the CPU in every diagnostic.  BSP-only on
     * i386 (I386_PLAN D5), so the number is 0 -- printed anyway so the
     * log format matches the x86_64 one and the R2-style "which core?"
     * question can never come back. */
    kprintf32("  cpu=0  vector=%u (%s)  err=%x\n",
              r->vector,
              r->vector < 32 ? exception_messages[r->vector] : "IRQ/soft",
              r->error_code);
    kprintf32("  eip=%x cs=%x eflags=%x\n", r->eip, r->cs, r->eflags);
    kprintf32("  eax=%x ebx=%x ecx=%x edx=%x\n", r->eax, r->ebx, r->ecx, r->edx);
    kprintf32("  esi=%x edi=%x ebp=%x ds=%x\n", r->esi, r->edi, r->ebp, r->ds);
    if (r->vector == 14) {
        uint32_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf32("  cr2=%x (faulting linear address)\n", cr2);
    }
}

void isr_dispatch32(struct registers32 *regs)
{
    /* Hardware IRQs land on vectors 32..47 (PIC remap, irq32.c). */
    if (regs->vector >= 32 && regs->vector <= 47) {
        irq32_dispatch(regs);
        return;
    }

    if (regs->vector == 3 && expect_breakpoint) {
        /* The boot self-test's deliberate int3: report and resume. */
        expect_breakpoint = 0;
        breakpoint_seen   = 1;
        kprintf32("[diag] deliberate #BP self-test:\n");
        dump_frame(regs);
        return;
    }

    kprintf32("\n[diag] UNHANDLED EXCEPTION\n");
    dump_frame(regs);
    kprintf32("[diag] kernel halted.\n");
    for (;;)
        __asm__ volatile("cli; hlt");
}

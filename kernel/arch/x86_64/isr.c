/* isr.c — top-level interrupt/exception dispatcher (called from isr.asm). */

#include <stdint.h>
#include "kernel/arch/x86_64/isr.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/assert.h"

/* Intel SDM Vol.3, 6-15: mnemonic for each CPU exception vector. */
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

static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

/*
 * Best-effort frame-pointer stack walk. Compiled with -fno-omit-frame-pointer,
 * every C function links rbp -> caller's rbp; we follow the chain and print the
 * saved return addresses. Guards keep us inside the mapped higher half so a
 * bad pointer cannot fault inside the exception handler (which would escalate
 * to a triple fault).
 */
static void print_stack_trace(uint64_t rbp) {
    kprintf("  Stack trace:\n");
    for (int i = 0; i < 16; i++) {
        if (rbp == 0) {
            break;
        }
        if (rbp < 0xFFFFFFFF80000000ULL) {   /* must be kernel higher half */
            break;
        }
        uint64_t ret_addr = *(volatile uint64_t *)(rbp + 8);
        uint64_t next_rbp = *(volatile uint64_t *)(rbp);
        kprintf("    [%d] 0x%016llx\n", i, (unsigned long long)ret_addr);
        if (next_rbp <= rbp) {               /* prevent runaway climb */
            break;
        }
        rbp = next_rbp;
    }
}

static void dump_registers(const struct registers *r) {
    kprintf("  RAX=0x%016llx  RBX=0x%016llx  RCX=0x%016llx\n",
            (unsigned long long)r->rax, (unsigned long long)r->rbx,
            (unsigned long long)r->rcx);
    kprintf("  RDX=0x%016llx  RSI=0x%016llx  RDI=0x%016llx\n",
            (unsigned long long)r->rdx, (unsigned long long)r->rsi,
            (unsigned long long)r->rdi);
    kprintf("  RBP=0x%016llx  R8 =0x%016llx  R9 =0x%016llx\n",
            (unsigned long long)r->rbp, (unsigned long long)r->r8,
            (unsigned long long)r->r9);
    kprintf("  R10=0x%016llx  R11=0x%016llx  R12=0x%016llx\n",
            (unsigned long long)r->r10, (unsigned long long)r->r11,
            (unsigned long long)r->r12);
    kprintf("  R13=0x%016llx  R14=0x%016llx  R15=0x%016llx\n",
            (unsigned long long)r->r13, (unsigned long long)r->r14,
            (unsigned long long)r->r15);
    kprintf("  RIP=0x%016llx  CS =0x%04llx  RFLAGS=0x%016llx\n",
            (unsigned long long)r->rip, (unsigned long long)r->cs,
            (unsigned long long)r->rflags);
    kprintf("  RSP=0x%016llx  SS =0x%04llx\n",
            (unsigned long long)r->rsp, (unsigned long long)r->ss);

    /* Page fault: CR2 holds the faulting linear address (Intel SDM 3A, 4.7). */
    if (r->int_no == 14) {
        kprintf("  CR2=0x%016llx (faulting address)\n",
                (unsigned long long)read_cr2());
    }

    print_stack_trace(r->rbp);
}

void isr_handler(struct registers *r) {
    if (r->int_no < 32) {
        /* CPU exception: fatal for now. Print a full dump and halt. */
        const char *msg = exception_messages[r->int_no];
        kprintf("\n[EXCEPTION] %s (vector %llu, error code 0x%016llx)\n",
                msg, (unsigned long long)r->int_no,
                (unsigned long long)r->err_code);
        dump_registers(r);
        kernel_halt();
    } else if (r->int_no < 48) {
        /* Hardware IRQ (PIC-remapped vectors 32-47). */
        irq_dispatch((int)(r->int_no - 32), r);
    } else {
        kprintf("[isr] spurious/unknown interrupt vector %llu\n",
                (unsigned long long)r->int_no);
    }
}

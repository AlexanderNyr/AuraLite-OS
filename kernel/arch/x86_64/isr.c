/* isr.c — top-level interrupt/exception dispatcher (called from isr.asm). */

#include <stdint.h>
#include "kernel/arch/x86_64/isr.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/guard.h"
#include "kernel/proc/signal.h"
#include "kernel/proc/usercopy.h"
#include "kernel/mm/vma.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/assert.h"

/* Map a Ring-3 CPU exception vector to the POSIX signal it raises. */
static int exception_to_signal(uint64_t vec) {
    switch (vec) {
    case 0:  return SIGFPE;    /* #DE divide error */
    case 3:  return SIGTRAP;   /* #BP breakpoint */
    case 4:  return SIGFPE;    /* #OF overflow */
    case 6:  return SIGILL;    /* #UD invalid opcode */
    case 13: return SIGSEGV;   /* #GP general protection */
    case 14: return SIGSEGV;   /* #PF page fault */
    case 17: return SIGBUS;    /* #AC alignment check */
    case 16:
    case 19: return SIGFPE;    /* #MF x87 / #XM SIMD FP */
    default: return 0;
    }
}

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
        const char *msg = exception_messages[r->int_no];

        /* Determine whether this fault came from Ring 3 (user mode). CS & 3
         * gives the CPL: 0 = kernel, 3 = user. */
        int from_user = ((r->cs & 3) == 3);

        /* A write to a user COW page is a recoverable protection fault.  It
         * may be triggered either by user code or by kernel copy_to_user(). */
        if (r->int_no == 14 &&
            paging_handle_cow_fault(read_cr2(), r->err_code)) {
            return;
        }

        /* Lazy VMA fault: resolve demand paging for user mappings. */
        if (r->int_no == 14 && ((r->cs & 3) == 3)) {
            if (handle_user_page_fault(read_cr2(), r->err_code) == 0) {
                return;
            }
        }

        /* Fault-recovering uaccess: if a kernel #PF happens inside
         * copy_from_user/copy_to_user, redirect RIP to the assembly fixup so
         * the copy returns -1 instead of panicking the kernel. */
        if (!from_user && r->int_no == 14 && usercopy_recover_fault(&r->rip)) {
            return;
        }

        /* Stack guard-page diagnosis: if this #PF landed on a known kernel- or
         * user-stack guard page, report it explicitly as a stack overflow.  A
         * kernel-stack guard hit is fatal (we cannot safely continue on a
         * corrupted/overflowing kernel stack); a user-stack guard hit falls
         * through to the normal SIGSEGV path below. */
        if (r->int_no == 14) {
            const char *gdesc = 0;
            enum guard_fault_kind gk =
                guard_classify_fault(read_cr2(), from_user, &gdesc);
            if (gk != GUARD_FAULT_NONE) {
                kprintf("\n[GUARD] %s: CR2=0x%016llx RIP=0x%016llx (%s mode)\n",
                        gdesc ? gdesc : "stack guard page hit",
                        (unsigned long long)read_cr2(),
                        (unsigned long long)r->rip,
                        from_user ? "USER" : "KERNEL");
                if (!from_user) {
                    dump_registers(r);
                    kprintf("[GUARD] kernel stack overflow is fatal; halting.\n");
                    kernel_halt();
                }
                /* User-mode guard hit: let the SIGSEGV path below terminate or
                 * deliver a handler, but the [GUARD] line records the cause. */
            }
        }

        kprintf("\n[EXCEPTION] %s (vector %llu, error code 0x%016llx) "
                "from %s mode\n",
                msg, (unsigned long long)r->int_no,
                (unsigned long long)r->err_code,
                from_user ? "USER" : "KERNEL");

        /* For user-mode faults, map the exception to a POSIX signal.  If the
         * thread has a handler installed (and the signal is not blocked), build
         * the handler frame on its user stack and return via iret; otherwise
         * signal_raise_fault() terminates the thread (default action). */
        if (from_user) {
            int signo = exception_to_signal(r->int_no);
            if (signo && signal_raise_fault(r, signo)) {
                /* Handler frame installed; return to it. */
                return;
            }
            /* Unmapped exception or no handler: dump + terminate. */
            dump_registers(r);
            kprintf("[exception] killing user thread (tid %llu)\n",
                    (unsigned long long)(sched_current() ? sched_current()->id : 0));
            thread_exit_with_code(128 + (int)r->int_no);
        }

        /* Kernel-mode exception: dump full context below. */
        dump_registers(r);

        /* Kernel-mode exception: truly fatal. */
        kernel_halt();
    } else if (r->int_no < 48) {
        /* Hardware IRQ (PIC-remapped vectors 32-47). */
        irq_dispatch((int)(r->int_no - 32), r);
        /* On the way back to Ring 3, deliver any pending unblocked signal
         * (e.g. one posted by kill() from another thread, or SIGALRM). */
        signal_deliver_iret(r);
    } else {
        kprintf("[isr] spurious/unknown interrupt vector %llu\n",
                (unsigned long long)r->int_no);
    }
}

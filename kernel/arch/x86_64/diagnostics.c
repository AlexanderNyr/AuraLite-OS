/* diagnostics.c — FIX_R0 (FIXES_PLAN.md): make failures visible.
 *
 * The existing fatal path prints through kprintf(), which takes the
 * console spinlock, writes to the framebuffer, and walks the rbp chain
 * with raw dereferences.  A fault taken while the lock is held, with a
 * broken framebuffer mapping, or on a corrupt stack therefore produces
 * silence — the machine hangs or resets with nothing on the wire.  This
 * module is the "before anything that might fault again" stage: all its
 * output goes straight to COM1 with a bounded poll, and all its reads of
 * potentially-corrupt memory are probed against the kernel page tables.
 */

#include <stdint.h>
#include "kernel/arch/x86_64/diagnostics.h"
#include "kernel/arch/x86_64/portio.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/arch/x86_64/idt.h"
#include "kernel/arch/x86_64/tss.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/string_fast.h"
#include "kernel/boot_info.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/bsod.h"

/* ------------------------------------------------------------------------
 * Lock-free serial output (COM1), safe from any context.
 * ------------------------------------------------------------------------ */

#define DIAG_UART_BASE 0x3F8
#define DIAG_UART_LSR  (DIAG_UART_BASE + 5)
#define DIAG_UART_THRE 0x20
/* With no UART attached the LSR never reports THRE; a panic path must not
 * spin forever, so the poll is bounded (drop the byte after ~100k polls). */
#define DIAG_UART_SPIN 100000u

static void diag_serial_putc(char c) {
    for (uint32_t i = 0; i < DIAG_UART_SPIN; i++) {
        if (inb(DIAG_UART_LSR) & DIAG_UART_THRE) {
            outb(DIAG_UART_BASE, (uint8_t)c);
            return;
        }
    }
}

void diag_early_puts(const char *s) {
    if (!s) {
        s = "(null)";
    }
    while (*s) {
        diag_serial_putc(*s++);
    }
}

void diag_early_puthex(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    diag_serial_putc('0');
    diag_serial_putc('x');
    for (int i = 15; i >= 0; i--) {
        diag_serial_putc(digits[(v >> (i * 4)) & 0xF]);
    }
}

void diag_early_putdec(uint64_t v) {
    char buf[20];
    int i = 0;
    do {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0 && i < (int)sizeof(buf));
    while (i-- > 0) {
        diag_serial_putc(buf[i]);
    }
}

/* ------------------------------------------------------------------------
 * CPU identification.
 * ------------------------------------------------------------------------ */

uint32_t diag_cpu_id(void) {
    /* The GS-based cpu_local pointer is only valid on CPUs that have run
     * cpu_local_init().  Before that (all of early boot) only the BSP
     * exists, so 0 is the correct answer.  The flag is read first because
     * with GS base never programmed the gs:0 load below would itself
     * fault; a recursion in that case is bounded by the nesting guard in
     * diag_early_dump(). */
    if (!cpu_local_ready) {
        return 0;
    }
    struct cpu_local *c = get_cpu_local();
    if (!c || c->self != c) {
        return 0;
    }
    return (uint32_t)c->cpu_id;
}

/* ------------------------------------------------------------------------
 * Probed reads for the stack walk.
 *
 * A bad rbp chain must truncate the trace, never escalate the fault.  The
 * frames we follow always live in the kernel higher half (kernel image,
 * heap-allocated kernel/IST stacks), so an address outside it is junk.
 * Once paging is up we additionally translate through the KERNEL's PML4 —
 * not the current CR3, which may belong to a user address space whose own
 * half is unrelated — using HHDM-accessed page-table reads, which cannot
 * fault.  Unmapped guard pages are reported as "unmapped" and stop the
 * walk, which is exactly the boundary a stack overflow leaves behind.
 * ------------------------------------------------------------------------ */

#define DIAG_CANON_LOW 0xFFFFFFFF80000000ULL  /* start of the kernel half */

/* 3 levels below PML4 offset the same way; PS marks a huge page at PDPT/PD. */
#define DIAG_PT_ENTRIES 512
#define DIAG_PT_PRESENT 0x1ULL
#define DIAG_PT_PS      (1ULL << 7)
#define DIAG_PT_ADDR    0x000FFFFFFFFFF000ULL

/* Tri-state: 1 = mapped, 0 = definitely unmapped, -1 = cannot tell
 * (paging not initialised yet; caller falls back to range checks only). */
static int diag_kernel_mapped(uint64_t va) {
    uint64_t pml4_phys = paging_get_kernel_pml4();
    if (pml4_phys == 0) {
        return -1;
    }
    uint64_t hhdm  = boot_get_hhdm_offset();
    uint64_t table = pml4_phys & DIAG_PT_ADDR;

    for (int level = 4; level >= 1; level--) {
        int shift = 12 + (level - 1) * 9;
        uint64_t idx = (va >> shift) & (DIAG_PT_ENTRIES - 1);
        uint64_t entry =
            *(volatile uint64_t *)(hhdm + table + idx * sizeof(uint64_t));
        if (!(entry & DIAG_PT_PRESENT)) {
            return 0;
        }
        if (level <= 3 && (entry & DIAG_PT_PS)) {
            return 1;                      /* 1 GiB / 2 MiB leaf: mapped */
        }
        table = entry & DIAG_PT_ADDR;
    }
    return 1;
}

static void diag_trace_frame_out_of_range(uint64_t rbp) {
    diag_early_puts("[diag]   <stop: frame pointer outside the kernel half: ");
    diag_early_puthex(rbp);
    diag_early_puts(">\n");
}

static void diag_early_stack_trace(uint64_t rbp) {
    diag_early_puts("[diag] Stack trace (rbp chain, probed reads):\n");
    for (int i = 0; i < 16; i++) {
        if (rbp == 0) {
            diag_early_puts("[diag]   <end of chain>\n");
            return;
        }
        if (rbp < DIAG_CANON_LOW) {
            diag_trace_frame_out_of_range(rbp);
            return;
        }
        if (rbp & 0x7) {
            diag_early_puts("[diag]   <stop: misaligned frame pointer ");
            diag_early_puthex(rbp);
            diag_early_puts(">\n");
            return;
        }
        if (diag_kernel_mapped(rbp) == 0 ||
            diag_kernel_mapped(rbp + 8) == 0) {
            /* Not mapped in the kernel PML4: a wild pointer or an unmapped
             * guard page.  Stop instead of dereferencing. */
            diag_early_puts("[diag]   <stop: unmapped frame at rbp=");
            diag_early_puthex(rbp);
            diag_early_puts(">\n");
            return;
        }
        uint64_t ret_addr = *(volatile uint64_t *)(rbp + 8);
        uint64_t next_rbp = *(volatile uint64_t *)(rbp);
        diag_early_puts("[diag]   [");
        diag_early_putdec((uint64_t)i);
        diag_early_puts("] ");
        diag_early_puthex(ret_addr);
        diag_early_puts("\n");
        if (next_rbp <= rbp) {           /* prevent a runaway climb */
            diag_early_puts("[diag]   <end of chain>\n");
            return;
        }
        rbp = next_rbp;
    }
    diag_early_puts("[diag]   <trace truncated at 16 frames>\n");
}

/* ------------------------------------------------------------------------
 * The fatal-kernel-exception dump.
 *
 * Nested invocation happens when the dump itself faulted (IOW the one
 * thing that cannot be probed away, e.g. a GS-read fault on a CPU whose
 * GS base was never set).  Print one line and bail out; the outer dump
 * is already on the wire and the handler will halt anyway.
 * ------------------------------------------------------------------------ */

static volatile uint32_t diag_nesting = 0;

void diag_early_dump(const struct registers *r, const char *exception_name) {
    if (diag_nesting != 0) {
        diag_early_puts("[diag] recursive fault inside the diagnostics dump; "
                        "suppressing nested dump\n");
        bsod_show(BSOD_KRECURSE, "fault inside diagnostic dump",
                  diag_cpu_id(), r ? r->rip : 0, 0);
        return;
    }
    diag_nesting++;

    diag_early_puts("\n[diag] === KERNEL EXCEPTION cpu#");
    diag_early_putdec(diag_cpu_id());
    diag_early_puts(": ");
    diag_early_puts(exception_name ? exception_name : "unknown exception");
    diag_early_puts(" (vector ");
    diag_early_putdec(r->int_no);
    diag_early_puts(") ===\n");

    diag_early_puts("[diag] err=");
    diag_early_puthex(r->err_code);
    diag_early_puts(" faulting RIP=");
    diag_early_puthex(r->rip);
    diag_early_puts("\n");

    diag_early_puts("[diag] RSP=");
    diag_early_puthex(r->rsp);
    diag_early_puts(" RBP=");
    diag_early_puthex(r->rbp);
    diag_early_puts(" RFLAGS=");
    diag_early_puthex(r->rflags);
    diag_early_puts("\n");

    diag_early_puts("[diag] RAX=");
    diag_early_puthex(r->rax);
    diag_early_puts(" RBX=");
    diag_early_puthex(r->rbx);
    diag_early_puts(" RCX=");
    diag_early_puthex(r->rcx);
    diag_early_puts("\n");
    diag_early_puts("[diag] RDX=");
    diag_early_puthex(r->rdx);
    diag_early_puts(" RSI=");
    diag_early_puthex(r->rsi);
    diag_early_puts(" RDI=");
    diag_early_puthex(r->rdi);
    diag_early_puts("\n");
    diag_early_puts("[diag] R8 =");
    diag_early_puthex(r->r8);
    diag_early_puts(" R9 =");
    diag_early_puthex(r->r9);
    diag_early_puts(" R10=");
    diag_early_puthex(r->r10);
    diag_early_puts("\n");
    diag_early_puts("[diag] R11=");
    diag_early_puthex(r->r11);
    diag_early_puts(" R12=");
    diag_early_puthex(r->r12);
    diag_early_puts(" R13=");
    diag_early_puthex(r->r13);
    diag_early_puts("\n");
    diag_early_puts("[diag] R14=");
    diag_early_puthex(r->r14);
    diag_early_puts(" R15=");
    diag_early_puthex(r->r15);
    diag_early_puts(" CS=");
    diag_early_puthex(r->cs);
    diag_early_puts("\n");

    /* Intel SDM 3A, 4.7: CR2 holds the faulting linear address of a #PF. */
    if (r->int_no == 14) {
        diag_early_puts("[diag] CR2=");
        diag_early_puthex(read_cr2());
        diag_early_puts("\n");
    }

    diag_early_stack_trace(r->rbp);
    diag_early_puts("[diag] end of dump; the regular handler log follows "
                    "(anything below this line is best-effort)\n");

    diag_nesting--;
}

/* ------------------------------------------------------------------------
 * Boot-time IST self-check (FIX_R0 task 3).
 * ------------------------------------------------------------------------ */

#define DIAG_MAX_TSS_CPUS 32   /* mirrors MAX_TSS_CPUS in tss.c */

void diag_ist_self_check(void) {
    uint32_t ist1_cpus = 0;
    uint64_t bsp_top = 0;
    for (int c = 0; c < DIAG_MAX_TSS_CPUS; c++) {
        uint64_t top = tss_get_ist1_top_for_cpu(c);
        if (top != 0) {
            ist1_cpus++;
            if (bsp_top == 0) {
                bsp_top = top;
            }
        }
    }
    uint8_t df_ist = idt_get_ist(8);       /* vector 8: #DF */

    /* FIX_R1: verify the guard page below the BSP's IST1 stack is really
     * unmapped in the kernel PML4 — "overflowing the IST stack is also
     * caught" should be a readback fact, not a code comment. */
    const char *guard_state = "unknown";
    if (bsp_top != 0) {
        uint64_t guard_va = bsp_top - TSS_IST1_STACK_SIZE - 0x1000ULL;
        int m = diag_kernel_mapped(guard_va);
        guard_state = (m == 0) ? "armed" : (m > 0) ? "MISSING (mapped!)"
                                                   : "unknown";
    }

    /* Single line, stable wording: tests and humans grep for "IST check".
     * Readback, not trust: the numbers come from the TSS/IDT as loaded,
     * so when FIX_R1 arms the IST the line changes on its own. */
    kprintf("[diag] IST check: IST1 stacks programmed for %u CPU slot(s); "
            "IDT #DF gate ist=%u; IST guard %s -- %s\n",
            ist1_cpus, (unsigned)df_ist, guard_state,
            df_ist ? "IST ARMED"
                   : "IST NOT ARMED -- a kernel fault on a bad stack "
                     "triple-faults (FIX_R1 will arm it)");
}

/* ------------------------------------------------------------------------
 * Deliberate fault for the FIX_R0 integration test gate.
 * ------------------------------------------------------------------------ */

__attribute__((noinline))
void diag_trigger_kernel_fault(void) {
    /* Write to the never-mapped address 0: a guaranteed kernel #PF.
     * The address sits in a variable so the compiler cannot technically
     * see a null-pointer store and helpfully optimise the fault away. */
    volatile uintptr_t addr = 0;
    *(volatile uint64_t *)addr = 0xD1A6D1A6D1A6D1A6ULL;
}

/* ------------------------------------------------------------------------
 * Deliberate kernel-stack overflow for the FIX_R1 integration test gate.
 * ------------------------------------------------------------------------ */

__attribute__((noinline))
static void diag_stack_burn(uint64_t depth) {
    /* ~2 KiB frames: guaranteed smaller than the 4 KiB guard page bracketing
     * every kernel stack, so the descent cannot leap over the guard into a
     * mapped neighbour.  The volatile byte writes keep every frame live. */
    volatile char frame[2048];
    frame[0] = (char)depth;
    if (depth > 0) {
        diag_stack_burn(depth - 1);
    }
    frame[2047] = (char)(frame[0] + 1);
}

__attribute__((noinline))
void diag_trigger_kernel_stack_overflow(void) {
    /* Deep recursion on the caller's (16 KiB) kernel stack.  The frame whose
     * prologue lands in the low guard page faults with RSP already pointing
     * at unmapped memory, so the CPU cannot even stack the #PF frame and
     * escalates to #DF — the exact chain FIX_R1 exists to catch:
     *   pre-R1: silent triple-fault reset;
     *   post-R1: #DF runs on IST1, the R0 dump prints, the machine halts. */
    diag_stack_burn(100000);
}

/* ---- HW_PLAN H0: the CPU feature receipts --------------------------------
 *
 * One line per boot, printed before anything acts on the features, so
 * every lane (qemu64, -cpu max, metal) leaves a record of what the CPU
 * actually offered -- the receipt IS the compatibility matrix (HW D4),
 * and H2/H3/H4 gate on these exact lines.  Measured at H0: qemu64 TCG
 * says pat=1 pcid=0 invpcid=0 erms=0; -cpu max TCG says erms=1 and
 * STILL pcid=0 invpcid=0 (which re-scoped H4 into a deferral).
 *
 * Bits: PAT CPUID.1:EDX.16; PCID 1:ECX.17; INVPCID 7.0:EBX.10;
 * ERMS 7.0:EBX.9.  IA32_PAT is MSR 0x277 (reset default
 * 0x0007040600070406 -- no write-combining entry until H3 programs
 * one).  This body lives in the arch tree because cpuid/rdmsr are x86
 * by nature; a first draft in kernel/kernel.c raised width-sweep
 * ratchet 2 to 70/69 -- the ratchet did its job, the code moved. */
void diag_cpu_feature_receipts(void)
{
    uint32_t ecx1, edx1, ebx7;

    cpuid_count(1, 0, 0, 0, &ecx1, &edx1);
    cpuid_count(7, 0, 0, &ebx7, 0, 0);
    kprintf("[cpu]   features: pat=%d pcid=%d invpcid=%d erms=%d\n",
            (int)((edx1 >> 16) & 1), (int)((ecx1 >> 17) & 1),
            (int)((ebx7 >> 10) & 1), (int)((ebx7 >> 9) & 1));
    if ((edx1 >> 16) & 1)
        kprintf("[cpu]   IA32_PAT = 0x%016llx\n",
                (unsigned long long)read_msr(0x277));

    /* HW H2: the receipt's first consumer -- the rep-string backend
     * picks its small-copy crossover off the same CPUID bit and
     * prints the threshold line right under the receipt. */
    string_fast_init();
}

/* kernel/arch/aarch64/user_a64.c -- EL0 + svc (ARM64_PLAN A4).
 *
 * user_rv.c's privilege round trip, the fourth ISA's spelling:
 *
 *   kernel thread -> user_a64_run_image
 *     copy the image to a fresh frame, map it at 0x40000000 with
 *     EL0 R+X -- REAL permissions: W^X holds at EL0 too, and TWICE
 *     (kernel pages are UXN from A3; user text is PXN from here --
 *     each world is non-executable from the other)
 *     user stack below it (EL0 R+W, and pointedly NOT X)
 *     user_enter_a64: trap stack in sp, SP_EL0 := user sp, eret
 *   EL0 -> svc #0 (x8 number / x0-x5 args / x0 return, D4)
 *     the hardware lands on SP_EL1 (no scratch-CSR dance -- the I7
 *     esp0 contract is an architecture feature here), vectors.S
 *     spills, a64_trap routes EC 0x15 here
 *   SYS_A64_EXIT longjmps the kernel thread out of the user context
 *     (a64_setjmp/a64_longjmp_entry -- A3's fault-probe pair IS the
 *     exit trampoline; one mechanism, two tenants, the V3/V4 rhyme)
 *
 * An EL0 fault (the lower-EL vector rows / lower-EL ECs) terminates
 * the image with 128+EC through the same longjmp -- the kernel
 * survives, the negative control depends on it.
 *
 * PAN note (SUM's sibling, opposite default): riscv64 measured that
 * kernel loads from user pages trap until sstatus.SUM is set.  On
 * ARMv8.0 there is NO PAN -- the kernel can touch EL0-accessible
 * pages freely, so this file needs no sum_on/sum_off pair.  PAN
 * arrives at v8.1 as an OPT-IN hardening (SCTLR_EL1.SPAN); the A-plan
 * runs v8.0 semantics and NAMES the residue here instead of silently
 * relying on it: when PAN hardening lands, every user access in this
 * file funnels through the two copy helpers below, which is where
 * the pan_off/pan_on pair will go.
 */

#include <stdint.h>

#include "kernel/arch/aarch64/user_a64.h"
#include "kernel/arch/aarch64/kheap_a64.h"
#include "kernel/arch/aarch64/paging_a64.h"
#include "kernel/arch/aarch64/pl011.h"
#include "kernel/arch/aarch64/pmm_a64.h"
#include "kernel/arch/aarch64/thread_a64.h"

/* context_a64.S */
extern void user_enter_a64(uint64_t entry, uint64_t user_sp,
                           uint64_t trap_stack_top)
    __attribute__((noreturn));
/* vectors.S */
extern int  a64_setjmp(uint64_t *buf);
extern void a64_longjmp_entry(void);

#define USER_STACK_TOP_A64   USER_TEXT_VADDR_A64
#define USER_STACK_VADDR_A64 (USER_STACK_TOP_A64 - PAGE_SIZE_A64)
#define TRAP_STACK_SIZE      8192

/* One image at a time (boot-CPU bring-up, D5). */
static uint64_t exit_jmpbuf[14];
static volatile int user_active;

static void put_udec_(uint64_t v) { pl011_putdec64(v); }
static void put_hex_(uint64_t v)  { pl011_puthex64(v); }

/* The exit unwind: rewrite the frame so eret lands in
 * a64_longjmp_entry AT EL1 with x0 = jmpbuf (setjmp returns 1; the
 * exit code travels in a static because unlike rv_longjmp_entry's
 * a1-carries-code contract, a64_setjmp's return value is fixed at 1
 * -- one less moving part in the assembly, one static here).
 *
 * SPSR: EL1h with IRQ MASKED -- the rv SPIE lesson's DAIF spelling:
 * the eret lands with sp still meaning "the dedicated trap stack",
 * and an IRQ taken before user_a64_run_image restores the kernel
 * stack notion would build a frame over live unwind state.  The
 * lesson transfers by lineage (bought with a -d int session on the
 * rv port); interrupts reopen in run_image once sp is sane. */
static volatile int exit_code_box;

static void user_a64_leave(a64_trap_frame_t *f, int code)
{
    user_active   = 0;
    exit_code_box = code;
    f->elr        = (uint64_t)a64_longjmp_entry;
    f->spsr       = 0x3C5;                 /* EL1h, DAIF all masked */
    f->regs[0]    = (uint64_t)exit_jmpbuf;
}

/* ---- trap_a64.c hooks ------------------------------------------------------ */

void user_a64_syscall(a64_trap_frame_t *f)
{
    uint64_t nr = f->regs[8];              /* x8 */
    uint64_t a0 = f->regs[0];
    uint64_t a1 = f->regs[1];
    uint64_t a2 = f->regs[2];

    if (!user_active) {                    /* svc from EL1 is a bug */
        pl011_puts("[user] stray EL1 svc -- ignored\n");
        return;
    }

    switch (nr) {
    case SYS_A64_WRITE: {
        /* write(fd=x0, buf=x1, len=x2): fd 1, bounds inside the user
         * window, pages probed, then the copy to a KERNEL buffer
         * before printing.  No PAN on v8.0 (see the header comment)
         * -- the bounds+probe pair is what stands between a user
         * pointer and the kernel's console either way. */
        if (a0 != 1 || a2 == 0 || a2 > 4096 ||
            a1 < USER_STACK_VADDR_A64 ||
            a1 + a2 > USER_TEXT_VADDR_A64 + PAGE_SIZE_A64) {
            f->regs[0] = (uint64_t)-1;
            return;
        }
        for (uint64_t va = a1 & ~(uint64_t)(PAGE_SIZE_A64 - 1);
             va < a1 + a2; va += PAGE_SIZE_A64) {
            if (paging_a64_probe(va) == ~0UL) {
                f->regs[0] = (uint64_t)-14;    /* -EFAULT */
                return;
            }
        }
        static char kbuf[4096];
        const char *p = (const char *)a1;
        for (uint64_t i = 0; i < a2; i++)
            kbuf[i] = p[i];
        for (uint64_t i = 0; i < a2; i++)
            pl011_putc(kbuf[i]);
        f->regs[0] = a2;
        return;
    }
    case SYS_A64_GETPID:
        f->regs[0] = (uint64_t)thread_a64_current_tid();
        return;
    case SYS_A64_YIELD:
        f->regs[0] = 0;
        return;
    case SYS_A64_EXIT:
        pl011_puts("[user] exit(");
        put_udec_(a0);
        pl011_puts(") via svc\n");
        user_a64_leave(f, (int)a0);
        return;
    default:
        pl011_puts("[user] unknown syscall ");
        put_udec_(nr);
        pl011_puts(" -> -ENOSYS\n");
        f->regs[0] = (uint64_t)-38;
        return;
    }
}

int user_a64_fault(a64_trap_frame_t *f, uint64_t esr, uint64_t far)
{
    if (!user_active)
        return 0;
    pl011_puts("[user] EL0 fault: ec=");
    put_udec_((esr >> 26) & 0x3F);
    pl011_puts(" far=");
    put_hex_(far);
    pl011_puts(" elr=");
    put_hex_(f->elr);
    pl011_puts(" -- terminating image (code ");
    put_udec_(128 + ((esr >> 26) & 0x3F));
    pl011_puts(")\n");
    user_a64_leave(f, 128 + (int)((esr >> 26) & 0x3F));
    return 1;
}

/* ---- running an image ------------------------------------------------------ */

int user_a64_run_image(const uint8_t *code, uint64_t code_len)
{
    if (code_len > PAGE_SIZE_A64)
        return -1;

    uint64_t text_frame  = pmm_a64_alloc_frame();
    uint64_t stack_frame = pmm_a64_alloc_frame();
    uint8_t *trap_stack  = (uint8_t *)kmalloc_a64(TRAP_STACK_SIZE);
    if (!text_frame || !stack_frame || !trap_stack)
        goto fail_setup;

    /* Copy through the HHDM, then map with REAL permissions: text
     * EL0 R+X (no W; and PXN -- the kernel must never execute user
     * pages, W^X's second axis), stack EL0 R+W (no X anywhere). */
    uint8_t *dst = (uint8_t *)p2v_a64(text_frame);
    for (uint64_t i = 0; i < code_len; i++)
        dst[i] = code[i];

    if (paging_a64_map(USER_TEXT_VADDR_A64, text_frame,
                       A64_MAP_RX_USER) != 0 ||
        paging_a64_map(USER_STACK_VADDR_A64, stack_frame,
                       A64_MAP_RW_USER) != 0)
        goto fail_mapped;

    user_active = 1;

    int rc = a64_setjmp(exit_jmpbuf);
    if (rc == 0) {
        /* First arrival: down to EL0.  Never returns -- the image
         * leaves through SYS_A64_EXIT or a contained fault, both of
         * which longjmp back here with rc = 1. */
        user_enter_a64(USER_TEXT_VADDR_A64,
                       USER_STACK_TOP_A64 - 16,
                       (uint64_t)trap_stack + TRAP_STACK_SIZE);
    }

    /* Landed from user_a64_leave: the code sits in the box.  IRQ is
     * masked across the unwind (the rv SPIE lesson's DAIF spelling);
     * reopen now that sp is this kernel stack again. */
    __asm__ volatile("msr daifclr, #2");
    (void)rc;
    paging_a64_unmap(USER_TEXT_VADDR_A64);
    paging_a64_unmap(USER_STACK_VADDR_A64);
    kfree_a64(trap_stack);
    pmm_a64_free_frame(text_frame);
    pmm_a64_free_frame(stack_frame);
    return exit_code_box;

fail_mapped:
    paging_a64_unmap(USER_TEXT_VADDR_A64);
    paging_a64_unmap(USER_STACK_VADDR_A64);
fail_setup:
    if (trap_stack)
        kfree_a64(trap_stack);
    if (text_frame)
        pmm_a64_free_frame(text_frame);
    if (stack_frame)
        pmm_a64_free_frame(stack_frame);
    return -1;
}

/* ---- the [user] gate ---------------------------------------------------------
 *
 * Two flat EL0 images, hand-assembled A64 (the prog_ok tradition:
 * encodings hand-checked, and the smoke test's exact-string assert
 * is the real reviewer).
 *
 * prog_ok:   write(1, msg@+0x48, 10); getpid(); exit(42)
 * prog_priv: mrs x0, sctlr_el1 -- privileged read from EL0, must be
 *            contained as 128 + EC 0x18 (trapped MSR/MRS... measured:
 *            QEMU raises EC 0x00 Unknown for this at EL0, which the
 *            result records -- the assertion tracks the measurement).
 */

/* Bytes MEASURED, not hand-rolled: assembled with the tree's own
 * clang from the .S in the comment, objcopy'd, od'd, pasted -- the
 * V0 pad-byte lesson says the assembler is the reviewer, and the
 * literal pool the assembler emitted (msg address at +0x30, ldr
 * literal) is kept exactly as it fell out. */
static const uint8_t prog_ok[] = {
    /*  mov x8,#1; mov x0,#1; ldr x1,=0x40000048; mov x2,#10; svc #0
     *  mov x8,#39; svc #0;  mov x8,#60; mov x0,#42; svc #0; udf     */
    0x28, 0x00, 0x80, 0xD2,   /* mov  x8, #1            */
    0x20, 0x00, 0x80, 0xD2,   /* mov  x0, #1            */
    0x41, 0x01, 0x00, 0x58,   /* ldr  x1, [pc+0x28] = literal @+0x30 */
    0x42, 0x01, 0x80, 0xD2,   /* mov  x2, #10           */
    0x01, 0x00, 0x00, 0xD4,   /* svc  #0                */
    0xE8, 0x04, 0x80, 0xD2,   /* mov  x8, #39           */
    0x01, 0x00, 0x00, 0xD4,   /* svc  #0                */
    0x88, 0x07, 0x80, 0xD2,   /* mov  x8, #60           */
    0x40, 0x05, 0x80, 0xD2,   /* mov  x0, #42           */
    0x01, 0x00, 0x00, 0xD4,   /* svc  #0                */
    0x00, 0x00, 0x00, 0x00,   /* udf #0 (never runs)    */
    0x00, 0x00, 0x00, 0x00,   /* pad to the literal     */
    0x48, 0x00, 0x00, 0x40,   /* +0x30: .quad 0x40000048 (lo) */
    0x00, 0x00, 0x00, 0x00,   /*                        (hi) */
    0x00, 0x00, 0x00, 0x00,   /* pad: msg at +0x48      */
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    /* +0x48: the message */
    'A', '6', '4', '-', 'U', '-', 'O', 'K', '!', '\n',
};

static const uint8_t prog_priv[] = {
    0x00, 0x10, 0x38, 0xD5,   /* mrs x0, sctlr_el1 -- EL1 register,
                               * from EL0: trap, never a value */
    0x88, 0x07, 0x80, 0xD2,   /* mov x8, #60 (not reached) */
    0x01, 0x00, 0x00, 0xD4,   /* svc #0 */
};

int user_a64_selftest(void)
{
    pl011_puts("[user] self-test: entering EL0...\n");

    int code = user_a64_run_image(prog_ok, sizeof(prog_ok));
    if (code != 42) {
        pl011_puts("[user] FAIL: expected exit 42, got ");
        put_udec_((uint64_t)code);
        pl011_puts("\n");
        return -1;
    }
    pl011_puts("[user] exit code 42 round-tripped\n");

    pl011_puts("[user] negative control: privileged mrs from EL0...\n");
    code = user_a64_run_image(prog_priv, sizeof(prog_priv));
    if (code < 128) {
        pl011_puts("[user] FAIL: privileged op NOT contained, code ");
        put_udec_((uint64_t)code);
        pl011_puts("\n");
        return -1;
    }
    pl011_puts("[user] PASS: privileged op contained (code ");
    put_udec_((uint64_t)code);
    pl011_puts("), kernel intact\n");
    return 0;
}

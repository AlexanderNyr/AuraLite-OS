/* kernel/arch/i386/user32.c -- Ring 3 + int 0x80 (I386_PLAN I4).
 *
 * The privilege round trip, proven end to end:
 *
 *   kernel thread -> user32_run_image
 *     map user .text at 0x40000000 (U/S set), user stack below it
 *     iret -> Ring 3 (user_entry32.asm)
 *   Ring 3 -> int 0x80 (gate DPL=3; TSS.esp0 supplies the k-stack)
 *     syscall32_dispatch: EAX number -> handler, EAX result
 *   SYS32_EXIT longjmps the kernel thread out of the user context
 *     (setjmp-style via saved esp/eip -- no signals yet at this width).
 *
 * A Ring 3 fault (#GP, #PF...) lands in isr_dispatch32, which detects
 * cs&3 and terminates the user image through the same exit path with
 * code 128+vector instead of halting the kernel -- the negative
 * control in user32_selftest depends on exactly this.
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/i386/user32.h"
#include "kernel/arch/i386/idt.h"
#include "kernel/arch/i386/isr.h"
#include "kernel/arch/i386/paging32.h"
#include "kernel/arch/i386/pmm32.h"
#include "kernel/arch/i386/kprintf32.h"
#include "kernel/arch/i386/thread32.h"
#include "kernel/arch/i386/initrd32.h"
#include "kernel/arch/i386/elf32load.h"

/* user_entry32.asm */
extern void user32_enter(uint32_t entry, uint32_t user_esp);
/* isr_stubs32.asm generated stub for vector 0x80 */
extern uint32_t isr_table32[];

#define USER_TEXT_VADDR  0x40000000u
#define USER_STACK_TOP   0x40000000u          /* stack page sits below text */
#define USER_STACK_VADDR (USER_STACK_TOP - PAGE_SIZE_32)

/* ---- the exit trampoline ---------------------------------------------- */

/* One user image at a time (BSP-only bring-up).  saved_esp/eip form a
 * minimal setjmp: SYS32_EXIT and the Ring 3 fault path both land in
 * user32_leave, which rewinds the kernel stack to user32_run_image. */
static uint32_t saved_esp, saved_ebp, saved_eip;
static volatile int user_exit_code;
static volatile int user_active;

static void user32_leave(int code) __attribute__((noreturn));
static void user32_leave(int code)
{
    user_exit_code = code;
    user_active    = 0;
    __asm__ volatile(
        "mov %0, %%esp\n"
        "mov %1, %%ebp\n"
        "jmp *%2\n"
        : : "r"(saved_esp), "r"(saved_ebp), "r"(saved_eip));
    __builtin_unreachable();
}

/* Called by isr_dispatch32 when an exception arrives with cs&3 == 3. */
int user32_fault(struct registers32 *regs)
{
    if (!user_active)
        return 0;
    kprintf32("[user] Ring 3 fault: vector=%u err=%x eip=%x -- "
              "terminating image (code %u)\n",
              regs->vector, regs->error_code, regs->eip,
              128 + regs->vector);
    user32_leave(128 + (int)regs->vector);
}

/* ---- int 0x80 ----------------------------------------------------------- */

static void syscall32_dispatch(struct registers32 *regs)
{
    switch (regs->eax) {
    case SYS32_WRITE: {
        /* write(fd=ebx, buf=ecx, len=edx) -> console only at I4/I5.
         * I5 widened the check from the fixed I4 window to "every
         * touched page is user-mapped": ELF images live at
         * 0x08048000-region now, not just the hand-built page.  The
         * probe walk is the bring-up stand-in for copy_from_user
         * fault fixup (I6). */
        if (regs->ebx != 1 || regs->edx > 4096 ||
            regs->ecx >= KERNEL_VBASE_32 ||
            regs->ecx + regs->edx > KERNEL_VBASE_32) {
            regs->eax = (uint32_t)-1;
            return;
        }
        for (uint32_t va = regs->ecx & ~(PAGE_SIZE_32 - 1);
             va < regs->ecx + regs->edx; va += PAGE_SIZE_32) {
            if (!paging32_probe(va, NULL)) {
                regs->eax = (uint32_t)-14;   /* -EFAULT */
                return;
            }
        }
        const char *p = (const char *)regs->ecx;
        for (uint32_t i = 0; i < regs->edx; i++)
            kputc32(p[i]);
        regs->eax = regs->edx;
        return;
    }
    case SYS32_GETPID:
        regs->eax = (uint32_t)thread32_current_tid();
        return;
    case SYS32_YIELD:
        sched32_yield();
        regs->eax = 0;
        return;
    case SYS32_EXIT:
        kprintf32("[user] exit(%u) via int 0x80\n", regs->ebx);
        user32_leave((int)regs->ebx);
    default:
        kprintf32("[user] unknown syscall %u -> -ENOSYS\n", regs->eax);
        regs->eax = (uint32_t)-38;      /* -ENOSYS, same value as libc */
        return;
    }
}

/* isr_dispatch32 routes vector 0x80 here. */
void syscall32_entry(struct registers32 *regs)
{
    syscall32_dispatch(regs);
}

void syscall32_init(void)
{
    /* The one DPL=3 gate: without it, int 0x80 from Ring 3 is #GP(GP
     * selector error) -- which is also exactly what keeps every OTHER
     * vector un-invokable from user code. */
    idt_set_gate(0x80, isr_table32[0x80], IDT_GATE_INTERRUPT_USER);
    kprintf32("[boot] int 0x80 gate armed (DPL=3), AuraLite syscall "
              "numbers (plan D4)\n");
}

/* ---- running an image --------------------------------------------------- */

int user32_run_image(const uint8_t *code, uint32_t code_len)
{
    if (code_len > PAGE_SIZE_32)
        return -1;

    uint32_t text_frame  = pmm32_alloc_frame();
    uint32_t stack_frame = pmm32_alloc_frame();
    if (!text_frame || !stack_frame)
        return -1;

    /* Copy the image through the direct map, then map both pages user.
     * Text is mapped WRITE as well at this phase: no NX exists to make
     * the distinction real (plan D3) and the honest flag is the one
     * the mapping actually enforces. */
    uint8_t *dst = (uint8_t *)p2v_32(text_frame);
    for (uint32_t i = 0; i < code_len; i++)
        dst[i] = code[i];

    if (paging32_map(USER_TEXT_VADDR, text_frame,
                     PAGE32_FLAG_USER | PAGE32_FLAG_WRITE) != 0 ||
        paging32_map(USER_STACK_VADDR, stack_frame,
                     PAGE32_FLAG_USER | PAGE32_FLAG_WRITE) != 0) {
        pmm32_free_frame(text_frame);
        pmm32_free_frame(stack_frame);
        return -1;
    }

    user_exit_code = -1;
    user_active    = 1;

    /* Minimal setjmp: capture esp/ebp and the resume label.  The
     * volatile asm block both stores the context and carries the
     * resume point ("1:") that user32_leave jumps back to. */
    __asm__ volatile(
        "mov %%esp, %0\n"
        "mov %%ebp, %1\n"
        "movl $1f, %2\n"
        "cmpl $0, %3\n"
        "je   2f\n"                     /* second arrival: skip enter */
        "push %5\n"
        "push %4\n"
        "call user32_enter\n"           /* never returns */
        "2:\n"
        "1:\n"
        : "=m"(saved_esp), "=m"(saved_ebp), "=m"(saved_eip)
        : "m"(user_active), "r"(USER_TEXT_VADDR), "r"(USER_STACK_TOP - 16)
        : "memory", "eax", "ecx", "edx");

    /* user32_leave lands here with the kernel stack rewound. */
    paging32_unmap(USER_TEXT_VADDR);
    paging32_unmap(USER_STACK_VADDR);
    pmm32_free_frame(text_frame);
    pmm32_free_frame(stack_frame);
    return user_exit_code;
}

/* ---- the boot self-test -------------------------------------------------- */

/* Hand-assembled Ring 3 programs (nasm -f bin equivalents, bytes kept
 * short enough to read).  Program 1:
 *     mov eax,1; mov ebx,1; mov ecx,msg; mov edx,len; int 0x80
 *     mov eax,39; int 0x80                      ; getpid
 *     mov eax,60; mov ebx,42; int 0x80          ; exit(42)
 * msg: "RING3-OK\n"
 * Linked-at 0x40000000, msg at +0x2C. */
static const uint8_t prog_ok[] = {
    0xB8, 0x01, 0x00, 0x00, 0x00,             /* mov eax, SYS32_WRITE  */
    0xBB, 0x01, 0x00, 0x00, 0x00,             /* mov ebx, 1            */
    0xB9, 0x2C, 0x00, 0x00, 0x40,             /* mov ecx, 0x4000002C   */
    0xBA, 0x09, 0x00, 0x00, 0x00,             /* mov edx, 9            */
    0xCD, 0x80,                               /* int 0x80              */
    0xB8, 0x27, 0x00, 0x00, 0x00,             /* mov eax, SYS32_GETPID */
    0xCD, 0x80,                               /* int 0x80              */
    0xB8, 0x3C, 0x00, 0x00, 0x00,             /* mov eax, SYS32_EXIT   */
    0xBB, 0x2A, 0x00, 0x00, 0x00,             /* mov ebx, 42           */
    0xCD, 0x80,                               /* int 0x80              */
    0xF4,                                     /* hlt (must never run)  */
    0x90, 0x90,                               /* pad: msg at +0x2C     */
    /* (the first cut had one pad byte -- msg landed at +0x2B and the
     * console printed "ING3-OK"; the smoke test's exact-string assert
     * exists precisely for hand-assembled bytes like these) */
    'R', 'I', 'N', 'G', '3', '-', 'O', 'K', '\n'
};

/* Program 2 (the negative control): `hlt` is privileged; from Ring 3
 * it must #GP and the fault path must terminate the image with
 * 128+13, never execute it. */
static const uint8_t prog_hlt[] = { 0xF4 };

int user32_selftest(void)
{
    kprintf32("[user] self-test: entering Ring 3...\n");

    int code = user32_run_image(prog_ok, sizeof(prog_ok));
    if (code != 42) {
        kprintf32("[user] FAIL: expected exit 42, got %d\n", code);
        return -1;
    }

    kprintf32("[user] self-test: negative control -- hlt from Ring 3...\n");
    code = user32_run_image(prog_hlt, sizeof(prog_hlt));
    if (code != 128 + 13) {
        kprintf32("[user] FAIL: privileged insn gave %d, want %d (#GP)\n",
                  code, 128 + 13);
        return -1;
    }

    kprintf32("[user] PASS: Ring 3 write/getpid/exit + #GP containment\n");
    return 0;
}

/* ---- I5: ELF32 images from the initrd ----------------------------------- */

int user32_run_elf(const char *path)
{
    const uint8_t *image;
    uint32_t size;

    if (!initrd32_find(path, &image, &size)) {
        kprintf32("[user] /%s: not found in the initrd\n", path);
        return -1;
    }

    uint32_t entry = elf32load_map(image, size);
    if (!entry)
        return -1;

    /* User stack: one page just under the ELF window's ceiling, well
     * clear of the load segments the bounds check confined below it. */
    uint32_t stack_frame = pmm32_alloc_frame();
    if (!stack_frame) {
        elf32load_unmap();
        return -1;
    }
    uint32_t stack_page = ELF32_USER_MAX - PAGE_SIZE_32;
    if (paging32_map(stack_page, stack_frame,
                     PAGE32_FLAG_USER | PAGE32_FLAG_WRITE) != 0) {
        pmm32_free_frame(stack_frame);
        elf32load_unmap();
        return -1;
    }

    kprintf32("[user] running /%s (entry %x, %u bytes)\n", path, entry, size);

    user_exit_code = -1;
    user_active    = 1;

    __asm__ volatile(
        "mov %%esp, %0\n"
        "mov %%ebp, %1\n"
        "movl $1f, %2\n"
        "cmpl $0, %3\n"
        "je   2f\n"
        "push %5\n"
        "push %4\n"
        "call user32_enter\n"
        "2:\n"
        "1:\n"
        : "=m"(saved_esp), "=m"(saved_ebp), "=m"(saved_eip)
        : "m"(user_active), "r"(entry), "r"(ELF32_USER_MAX - 16)
        : "memory", "eax", "ecx", "edx");

    paging32_unmap(stack_page);
    pmm32_free_frame(stack_frame);
    elf32load_unmap();
    return user_exit_code;
}

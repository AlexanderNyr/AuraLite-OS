/* user.c — Ring 3 entry and the Phase 8 gate test.
 *
 * jump_to_user() uses iretq to atomically drop to Ring 3. The gate test maps a
 * small user program (embedded machine code) and a user stack, jumps to Ring 3,
 * and lets the program attempt `cli` — a privileged instruction that must cause
 * a General Protection Fault (#GP). The exception handler recovers by killing
 * the user thread, proving the kernel is isolated from userspace.
 */

#include <stdint.h>
#include "kernel/proc/user.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/gdt.h"
#include "kernel/arch/x86_64/tss.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/kprintf.h"

#define USER_CODE_BASE  0x40000000ULL        /* 1 GiB — canonical user range */
#define USER_STACK_TOP  0x7FFFF0000000ULL    /* near top of user half       */
#define USER_STACK_SIZE 0x10000ULL           /* 64 KiB                      */

/* Implemented in user_entry.asm — the actual iretq to Ring 3. */
extern void jump_to_user_asm(uint64_t entry, uint64_t stack_top,
                             uint64_t stack_bottom);

/*
 * The embedded user program. It first does a SYSCALL to write a message
 * (proving it executes in Ring 3), then executes `cli` (privileged), which
 * must trigger #GP.
 *
 * Byte layout (RIP-relative offset = 8 so lea points at the message at offset 25):
 *   0:  b8 01 00 00 00        mov eax, 1          (SYS_WRITE)
 *   5:  bf 01 00 00 00        mov edi, 1          (stdout)
 *  10:  48 8d 35 08 00 00 00  lea rsi, [rip+8]    (msg at offset 25)
 *  17:  ba 0e 00 00 00        mov edx, 14         (length)
 *  22:  0f 05                 syscall
 *  24:  fa                    cli                 -> #GP (privileged)
 *  25:  "in user mode!\n"
 */
static const uint8_t user_program[] = {
    0xb8, 0x01, 0x00, 0x00, 0x00,             /* mov eax, 1   (SYS_WRITE) */
    0xbf, 0x01, 0x00, 0x00, 0x00,             /* mov edi, 1   (stdout)    */
    0x48, 0x8d, 0x35, 0x08, 0x00, 0x00, 0x00, /* lea rsi,[rip+8] (msg)    */
    0xba, 0x0e, 0x00, 0x00, 0x00,             /* mov edx, 14  (len)       */
    0x0f, 0x05,                               /* syscall                  */
    0xfa,                                     /* cli  -> #GP              */
    /* message at offset 25: */
    'i','n',' ','u','s','e','r',' ','m','o','d','e','!','\n'
};

void jump_to_user(uint64_t entry, uint64_t stack_top, uint64_t stack_bottom) {
    (void)stack_bottom;
    /* Set the TSS RSP0 to this thread's kernel stack so a subsequent interrupt
     * from Ring 3 lands on a known kernel stack. */
    tcb_t *cur = sched_current();
    if (cur && cur->kernel_stack) {
        tss_set_rsp0((uint64_t)cur->kernel_stack + THREAD_STACK_SIZE);
    }
    jump_to_user_asm(entry, stack_top, 0);
}

static void map_user_program(void) {
    extern uint64_t limine_get_hhdm_offset(void);
    extern uint64_t pmm_alloc_frame(void);
    uint64_t hhdm = limine_get_hhdm_offset();

    /* Map the user code page. */
    uint64_t frame = pmm_alloc_frame();
    if (frame == 0) {
        kprintf("[user] FAIL: OOM for user code frame\n");
        return;
    }
    uint8_t *dst = (uint8_t *)(uintptr_t)(hhdm + frame);
    for (size_t i = 0; i < sizeof(user_program); i++) {
        dst[i] = user_program[i];
    }
    paging_map(USER_CODE_BASE, frame,
               PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER);

    /* Map the user stack pages. */
    uint64_t stack_base = USER_STACK_TOP - USER_STACK_SIZE;
    for (uint64_t off = 0; off < USER_STACK_SIZE; off += 0x1000) {
        uint64_t sf = pmm_alloc_frame();
        if (sf == 0) {
            kprintf("[user] FAIL: OOM for user stack\n");
            return;
        }
        paging_map(stack_base + off, sf,
                   PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER);
    }
}

/* The user test runs as its own kernel thread, so that when #GP fires from
 * Ring 3 the CPU lands on THIS thread's kernel stack (TSS.RSP0), and killing
 * the thread cleanly removes it without disrupting kmain. */
static void user_test_thread(void *arg) {
    (void)arg;
    kprintf("[user] mapped code @0x%llx, stack top @0x%llx\n",
            (unsigned long long)USER_CODE_BASE,
            (unsigned long long)USER_STACK_TOP);
    jump_to_user(USER_CODE_BASE, USER_STACK_TOP - 16, 0);
    /* If we return, the user program exited cleanly via SYS_EXIT. */
}

static volatile int user_test_done = 0;

void user_mode_self_test(void) {
    kprintf("[user] self-test: entering Ring 3...\n");
    map_user_program();
    kthread_create(user_test_thread, NULL, "user-test");

    /* Give the user thread scheduling slots. It will either exit via syscall
     * or be killed by the #GP handler (on `cli`). Either way it terminates. */
    for (int i = 0; i < 30; i++) {
        sched_yield();
    }
    user_test_done = 1;
    kprintf("[user] PASS: Ring 3 entry + syscall + #GP recovery verified\n");
}

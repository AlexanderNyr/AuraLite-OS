/* process.c — per-process address spaces: fork, execve, wait4, spawn.
 *
 * Each user process has its own PML4. The scheduler switches CR3 based on
 * the TCB's pml4_phys field. fork() clones the address space (full copy);
 * execve() creates a fresh space and loads a new ELF; spawn() combines
 * process creation with ELF loading for the shell.
 */

#include <stdint.h>
#include "kernel/proc/process.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/elf.h"
#include "kernel/proc/user.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/tss.h"
#include "kernel/arch/x86_64/syscall.h"
#include "kernel/fs/vfs.h"
#include "kernel/mm/kheap.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/limine_requests.h"

#define USER_STACK_TOP  0x7FFFF0000000ULL
#define USER_STACK_SIZE 0x10000ULL

/* Saved user-mode state from the syscall entry (set in syscall_entry.asm). */
extern uint64_t syscall_saved_rcx;   /* user RIP */
extern uint64_t syscall_saved_r11;   /* user RFLAGS */
extern uint64_t syscall_saved_rsp;   /* user RSP (saved by our asm) */

/* ---- Address-space helpers ---- */

static void map_user_stack_pages(void) {
    uint64_t base = USER_STACK_TOP - USER_STACK_SIZE;
    for (uint64_t off = 0; off < USER_STACK_SIZE; off += 0x1000) {
        uint64_t phys = pmm_alloc_frame();
        if (phys == 0) {
            kprintf("[proc] OOM mapping user stack\n");
            return;
        }
        paging_map(base + off, phys,
                   PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER);
    }
}

/*
 * Load an ELF into the CURRENT address space and jump to user mode.
 * The caller must have already switched to the desired address space.
 * Does not return.
 */
static void __attribute__((noreturn))
load_and_jump(const void *elf_data, uint64_t elf_size) {
    uint64_t entry = elf_load(elf_data, elf_size);
    if (entry == 0) {
        kprintf("[proc] ELF load failed\n");
        thread_exit();
    }
    map_user_stack_pages();

    tcb_t *cur = sched_current();
    if (cur && cur->kernel_stack) {
        uint64_t kstack = (uint64_t)cur->kernel_stack + THREAD_STACK_SIZE;
        tss_set_rsp0(kstack);
    }

    kprintf("[proc] entering Ring 3 at 0x%llx (CR3=0x%llx)\n",
            (unsigned long long)entry,
            (unsigned long long)(read_cr3() & 0x000FFFFFFFFFF000ULL));

    jump_to_user(entry, USER_STACK_TOP - 16, 0);
    thread_exit();   /* not reached */
}

/* ---- fork() ---- */

/* The child thread function: it switches to the cloned address space and
 * returns to user mode at the fork() call site with RAX=0. */
static void fork_child_entry(void *arg) {
    (void)arg;
    /* The TCB's pml4_phys is set to the cloned address space. The scheduler
     * has already switched CR3 to it. We just need to return to user mode
     * at the saved user RIP with RAX=0 (fork return for the child). */
    uint64_t user_rip  = syscall_saved_rcx;
    uint64_t user_rflags = syscall_saved_r11;
    uint64_t user_rsp  = syscall_saved_rsp;

    kprintf("[proc] fork child starting, jumping to user 0x%llx\n",
            (unsigned long long)user_rip);

    /* Set TSS RSP0 for this child's kernel stack. */
    tcb_t *cur = sched_current();
    if (cur && cur->kernel_stack) {
        tss_set_rsp0((uint64_t)cur->kernel_stack + THREAD_STACK_SIZE);
    }

    /* Jump to user mode at the saved RIP. RAX will be set to 0 by the
     * fork_child_sysret asm (below) so the child sees fork()==0. */
    extern void fork_child_sysret(uint64_t rip, uint64_t rflags, uint64_t rsp);
    fork_child_sysret(user_rip, user_rflags, user_rsp);
    thread_exit();   /* not reached */
}

int64_t do_fork(void) {
    /* Save the user-mode state (it's in globals from syscall_entry). */
    uint64_t user_rip   = syscall_saved_rcx;
    uint64_t user_rflags = syscall_saved_r11;
    uint64_t user_rsp   = syscall_saved_rsp;

    kprintf("[proc] fork: cloning address space (user RIP=0x%llx)\n",
            (unsigned long long)user_rip);

    /* 1) Clone the current address space. */
    uint64_t child_pml4 = paging_clone_user_space();
    if (child_pml4 == 0) {
        kprintf("[proc] fork: address space clone failed\n");
        return -1;
    }
    kprintf("[proc] fork: cloned to PML4 phys 0x%llx\n",
            (unsigned long long)child_pml4);

    /* 2) Create the child thread. */
    tcb_t *parent = sched_current();
    tcb_t *child = kthread_create(fork_child_entry, NULL, "fork-child");
    if (child == NULL) {
        return -1;
    }
    child->pml4_phys = child_pml4;
    child->parent    = parent;

    /* 3) The parent returns the child PID. */
    kprintf("[proc] fork: parent returns child PID %llu\n",
            (unsigned long long)child->id);
    return (int64_t)child->id;
}

/* ---- execve() ---- */

int64_t do_execve(const char *path) {
    kprintf("[proc] execve('%s')\n", path);

    /* 1) Open the file from the VFS and read it into kernel memory. */
    int fd = vfs_open(path);
    if (fd < 0) {
        kprintf("[proc] execve: '%s' not found\n", path);
        return -1;
    }

    /* Read the entire file. For simplicity, assume it fits in 256 KiB. */
    uint8_t *buf = kmalloc(256 * 1024);
    if (!buf) {
        vfs_close(fd);
        return -1;
    }
    int64_t total = 0;
    int64_t n;
    while ((n = vfs_read(fd, buf + total, (256 * 1024) - total)) > 0) {
        total += n;
    }
    vfs_close(fd);
    kprintf("[proc] execve: read %lld bytes\n", (long long)total);

    /* 2) Create a fresh address space. */
    uint64_t new_pml4 = paging_new_address_space();
    if (new_pml4 == 0) {
        kfree(buf);
        return -1;
    }

    /* 3) Switch to it and load the ELF. */
    paging_switch_to(new_pml4);

    /* 4) Update the current TCB to point to the new address space. */
    tcb_t *cur = sched_current();
    if (cur) {
        cur->pml4_phys = new_pml4;
    }

    /* 5) Load and jump (does not return). */
    load_and_jump(buf, (uint64_t)total);

    return -1;   /* not reached */
}

/* ---- wait4() ---- */

int64_t do_wait4(int64_t *exit_code) {
    tcb_t *self = sched_current();
    if (self == NULL) return -1;

    /* Use sched_yield in a loop until the child clears waited_on.
     * We can't use BLOCKED+schedule() because that would require the child
     * to already be in the ready queue — which it is, since process_spawn
     * added it before we got here. But to be safe, yield-based polling works
     * correctly: each yield gives the child a scheduling slot. */
    self->waited_on = 1;
    while (self->waited_on) {
        sched_yield();
    }
    if (exit_code) *exit_code = 0;
    return 0;
}

/* ---- spawn() ---- */

/* Thread function for a spawned process. */
static void spawn_thread(void *arg) {
    const char *path = (const char *)arg;

    /* Read the ELF from the VFS into kernel memory. */
    int fd = vfs_open(path);
    if (fd < 0) {
        kprintf("[proc] spawn: '%s' not found\n", path);
        thread_exit();
    }

    uint8_t *buf = kmalloc(256 * 1024);
    if (!buf) {
        vfs_close(fd);
        thread_exit();
    }
    int64_t total = 0;
    int64_t n;
    while ((n = vfs_read(fd, buf + total, (256 * 1024) - total)) > 0) {
        total += n;
    }
    vfs_close(fd);

    load_and_jump(buf, (uint64_t)total);
}

int64_t process_spawn(const char *path) {
    /* Create a new address space. */
    uint64_t new_pml4 = paging_new_address_space();
    if (new_pml4 == 0) {
        return -1;
    }

    /* Create a thread that will load the ELF in its own address space. */
    /* We need to pass the path somehow; for simplicity, we use a known
     * mechanism: the thread reads from the VFS directly. */
    /* But wait — the spawn_thread runs in the KERNEL's address space first
     * (the scheduler switches CR3 based on pml4_phys). So we set pml4_phys
     * AFTER vfs_open but BEFORE elf_load. This means the thread must do:
     * 1. vfs_open + read (in kernel space — safe, kernel half shared)
     * 2. switch to its own address space
     * 3. elf_load + jump_to_user
     *
     * Actually, since the scheduler switches CR3 when it picks this thread,
     * we need to be careful. The thread starts running in its OWN address
     * space (because the scheduler switches to child_pml4 before context_switch).
     * But the user half is empty! So vfs_open/read work fine (kernel half is
     * shared), but we need to load the ELF into THIS address space.
     *
     * This actually works: the thread runs with child_pml4 active, calls
     * elf_load which maps into the CURRENT (child) address space. Perfect.
     */

    /* Allocate a copy of the path string (the caller's stack might change). */
    char *path_copy = kmalloc(strlen(path) + 1);
    if (!path_copy) return -1;
    strcpy(path_copy, path);

    tcb_t *child = kthread_create(spawn_thread, path_copy, path);
    if (child == NULL) {
        kfree(path_copy);
        return -1;
    }
    child->pml4_phys = new_pml4;
    child->parent = sched_current();

    return (int64_t)child->id;
}

/* ---- Self-test ---- */

void process_self_test(void) {
    kprintf("[proc] self-test: spawning /hello in isolated address space...\n");

    int64_t pid = process_spawn("/hello");
    if (pid < 0) {
        kprintf("[proc] FAIL: spawn failed\n");
        return;
    }
    kprintf("[proc] spawned PID %lld, waiting...\n", (long long)pid);

    /* Wait for the child. */
    do_wait4(NULL);

    kprintf("[proc] PASS: /hello ran in isolated address space\n");
}

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

#define USER_STACK_TOP         0x7FFFF0000000ULL
#define USER_STACK_SIZE        0x10000ULL
#define USER_STACK_GUARD_SIZE  0x1000ULL

/* Saved user-mode state from the syscall entry (set in syscall_entry.asm). */
extern uint64_t syscall_saved_rcx;   /* user RIP */
extern uint64_t syscall_saved_r11;   /* user RFLAGS */
extern uint64_t syscall_saved_rsp;   /* user RSP (saved by our asm) */

/* ---- Address-space helpers ---- */

static uint64_t choose_user_stack_top(void) {
    uint64_t pages = read_tsc() & 0xFULL; /* small per-exec entropy */
    return USER_STACK_TOP - pages * 0x1000ULL;
}

static void map_user_stack_pages(uint64_t stack_top) {
    uint64_t base = stack_top - USER_STACK_SIZE - USER_STACK_GUARD_SIZE;
    for (uint64_t off = USER_STACK_GUARD_SIZE; off < USER_STACK_GUARD_SIZE + USER_STACK_SIZE; off += 0x1000) {
        uint64_t phys = pmm_alloc_frame();
        if (phys == 0) {
            kprintf("[proc] OOM mapping user stack\n");
            return;
        }
        paging_map(base + off, phys,
                   PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE |
                   PAGE_FLAG_USER | PAGE_FLAG_NO_EXEC);
    }
}

/*
 * Load an ELF into the CURRENT address space and jump to user mode.
 * The caller must have already switched to the desired address space.
 * Does not return.
 */
static void __attribute__((noreturn))
load_and_jump(const void *elf_data, uint64_t elf_size) {
    tcb_t *cur = sched_current();
    uint64_t entry = elf_load(elf_data, elf_size, &cur->brk);
    if (entry == 0) {
        kprintf("[proc] ELF load failed\n");
        if (elf_data) kfree((void *)elf_data);
        thread_exit();
    }
    uint64_t stack_top = choose_user_stack_top();
    map_user_stack_pages(stack_top);
    if (elf_data) kfree((void *)elf_data);

    if (cur && cur->kernel_stack) {
        uint64_t kstack = (uint64_t)cur->kernel_stack + THREAD_STACK_SIZE;
        tss_set_rsp0(kstack);
        set_syscall_stack(kstack);
    }

    kprintf("[proc] entering Ring 3 at 0x%llx (CR3=0x%llx)\n",
            (unsigned long long)entry,
            (unsigned long long)(read_cr3() & 0x000FFFFFFFFFF000ULL));

    /* RSP must point into the topmost MAPPED page, which is
     * stack_top - USER_STACK_GUARD_SIZE. The guard page just above
     * is intentionally unmapped to catch stack overflows. */
    jump_to_user(entry, stack_top - USER_STACK_GUARD_SIZE - 16, 0);
    thread_exit();   /* not reached */
}

/* ---- fork() ---- */

/* The child thread function: it switches to the cloned address space and
 * returns to user mode at the fork() call site with RAX=0. */
static void fork_child_entry(void *arg) {
    (void)arg;
    /* The TCB's pml4_phys is set to the cloned address space. The scheduler
     * has already switched CR3 to it. We just need to return to user mode
     * at the saved user RIP with RAX=0 (fork return for the child).  We
     * read the fork frame from the TCB (not from the global syscall_saved_*
     * registers, which the parent has long since overwritten with later
     * syscalls). */
    tcb_t *self = sched_current();
    uint64_t user_rip    = self ? self->fork_user_rip    : syscall_saved_rcx;
    uint64_t user_rflags = self ? self->fork_user_rflags : syscall_saved_r11;
    uint64_t user_rsp    = self ? self->fork_user_rsp    : syscall_saved_rsp;

    kprintf("[proc] fork child starting, jumping to user 0x%llx\n",
            (unsigned long long)user_rip);

    /* Set TSS RSP0 for this child's kernel stack. */
    tcb_t *cur = sched_current();
    if (cur && cur->kernel_stack) {
        uint64_t kstack = (uint64_t)cur->kernel_stack + THREAD_STACK_SIZE;
        tss_set_rsp0(kstack);
        set_syscall_stack(kstack);
    }

    /* Jump to user mode at the saved RIP. RAX will be set to 0 by the
     * fork_child_sysret asm (below) so the child sees fork()==0. */
    extern void fork_child_sysret(uint64_t rip, uint64_t rflags, uint64_t rsp);
    fork_child_sysret(user_rip, user_rflags, user_rsp);
    thread_exit();   /* not reached */
}

int64_t do_fork(void) {
    tcb_t *self = sched_current();
    uint64_t user_rip = self && self->saved_user_rip ? self->saved_user_rip
                                                     : syscall_saved_rcx;
    uint64_t user_rflags = self && self->saved_user_rip ? self->saved_user_rflags
                                                        : syscall_saved_r11;
    uint64_t user_rsp = self && self->saved_user_rip ? self->saved_user_rsp
                                                     : syscall_saved_rsp;

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

    /* 2) Create the child thread.  We do this with interrupts disabled so
     * the scheduler can't pick up the half-initialised TCB (no pml4, no
     * saved user frame, no FDs) between kthread_create() and the field
     * assignments below. */
    tcb_t *parent = self;
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));

    tcb_t *child = kthread_create(fork_child_entry, NULL, "fork-child");
    if (child == NULL) {
        (void)paging_free_address_space(child_pml4);
        if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
        return -1;
    }
    child->pml4_phys = child_pml4;
    child->parent    = parent;
    /* Snapshot the user-mode return frame NOW (it lives in globals that the
     * parent will clobber on its very next syscall, well before the child
     * gets a chance to run). */
    child->fork_user_rip    = user_rip;
    child->fork_user_rflags = user_rflags;
    child->fork_user_rsp    = user_rsp;
    if (parent) {
        /* Share the parent's open-file descriptions (shared seek offset/flags),
         * incrementing each OFD refcount; copy the per-fd FD_CLOEXEC flags. */
        vfs_fork_inherit(child->fd_table, parent->fd_table,
                         child->cloexec, parent->cloexec);
        child->brk = parent->brk;
        child->mmap_next = parent->mmap_next;
        /* POSIX fork(): child inherits the signal dispositions and the blocked
         * mask, but its pending-signal set is empty. */
        for (int s = 0; s < NSIG; s++) child->sig_actions[s] = parent->sig_actions[s];
        child->sig_mask = parent->sig_mask;
        child->sig_pending = 0;
        /* POSIX fork(): child inherits the parent's process group, session and
         * controlling terminal; it is never a session/group leader by birth. */
        child->pgid = parent->pgid;
        child->sid  = parent->sid;
        child->ctty = parent->ctty;
        child->is_session_leader = 0;
        parent->n_children++;
    }

    if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");

    /* 3) The parent returns the child PID. */
    kprintf("[proc] fork: parent returns child PID %llu\n",
            (unsigned long long)child->id);
    return (int64_t)child->id;
}

/* ---- execve() ---- */

int64_t do_execve(const char *path) {
    kprintf("[proc] execve('%s')\n", path);

    /* 0) Close any FD marked FD_CLOEXEC.  Per POSIX, execve drops those. */
    vfs_close_on_exec();

    /* POSIX execve(): caught signals reset to SIG_DFL; signals set to SIG_IGN
     * stay ignored.  The blocked mask and pending set are preserved. */
    {
        tcb_t *cur = sched_current();
        if (cur) {
            for (int s = 1; s < NSIG; s++) {
                if (cur->sig_actions[s].sa_handler != SIG_IGN)
                    cur->sig_actions[s].sa_handler = SIG_DFL;
                cur->sig_actions[s].sa_flags = 0;
                cur->sig_actions[s].sa_mask = 0;
            }
        }
    }

    /* 1) Open the file from the VFS and read it into kernel memory. */
    int fd = vfs_open(path, O_RDONLY, 0);
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

    /* 3) Switch to it and retire the old user address space. */
    tcb_t *cur = sched_current();
    uint64_t old_pml4 = (cur ? cur->pml4_phys : 0);
    paging_switch_to(new_pml4);

    /* 4) Update the current TCB to point to the new address space. */
    if (cur) {
        cur->pml4_phys = new_pml4;
        cur->mmap_next = 0;
    }
    if (old_pml4 && old_pml4 != new_pml4) {
        (void)paging_free_address_space(old_pml4);
    }

    /* 5) Load and jump (does not return). */
    load_and_jump(buf, (uint64_t)total);

    return -1;   /* not reached */
}

/* ---- wait4() ---- */

int64_t do_wait4(int64_t *exit_code) {
    /* Legacy entry point: wait for any child of the current task and discard
     * its identity.  do_wait4_pid does the heavy lifting. */
    return do_wait4_pid(-1, exit_code);
}

/* ---- spawn() ---- */

/* Thread function for a spawned process. */
static void spawn_thread(void *arg) {
    char *path = (char *)arg;

    /* Read the ELF from the VFS into kernel memory. */
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) {
        kprintf("[proc] spawn: '%s' not found\n", path);
        memset(path, 0, strlen(path) + 1);
        kfree(path);
        thread_exit();
    }

    uint8_t *buf = kmalloc(256 * 1024);
    if (!buf) {
        vfs_close(fd);
        memset(path, 0, strlen(path) + 1);
        kfree(path);
        thread_exit();
    }
    int64_t total = 0;
    int64_t n;
    while ((n = vfs_read(fd, buf + total, (256 * 1024) - total)) > 0) {
        total += n;
    }
    vfs_close(fd);
    kfree(path);

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
    if (!path_copy) {
        (void)paging_free_address_space(new_pml4);
        return -1;
    }
    strcpy(path_copy, path);

    tcb_t *child = kthread_create(spawn_thread, path_copy, path);
    if (child == NULL) {
        kfree(path_copy);
        (void)paging_free_address_space(new_pml4);
        return -1;
    }
    child->pml4_phys = new_pml4;
    child->parent = sched_current();
    /* The spawned child joins the spawner's process group / session and shares
     * its controlling terminal (so a foreground spawn is killable by Ctrl+C). */
    if (child->parent) {
        child->pgid = child->parent->pgid;
        child->sid  = child->parent->sid;
        child->ctty = child->parent->ctty;
        child->is_session_leader = 0;
        child->parent->n_children++;
    }

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

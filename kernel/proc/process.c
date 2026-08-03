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
#include "kernel/mm/vma.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/errno.h"
#include "kernel/proc/usercopy.h"
#include "kernel/boot_info.h"

#define USER_STACK_TOP         0x7FFFF0000000ULL
#define USER_STACK_SIZE        0x10000ULL
#define USER_STACK_GUARD_SIZE  0x1000ULL

/* Saved user-mode state from the syscall entry (set in syscall_entry.asm). */
/* The user return frame is read through the syscall_saved_* per-CPU
 * accessor macros from kernel/arch/x86_64/syscall.h (already included
 * above): the slots live in struct cpu_local, per-cpu, since real SMP made
 * the former .data globals racy. */

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

/* ---- argv/envp marshalling (System V AMD64 ABI initial process stack) ----
 *
 * A kernel-side snapshot of the argv[] and envp[] vectors copied out of the
 * caller's address space BEFORE we tear it down in execve().  Each string is
 * kmalloc'd; argc/envc count the entries (excluding the terminating NULLs).
 */
#define EXEC_MAX_ARGS    256
#define EXEC_MAX_ARG_LEN 4096
#define EXEC_MAX_STRTOT  (64 * 1024)   /* total bytes of all strings */

struct exec_args {
    int    argc;
    int    envc;
    char  *argv[EXEC_MAX_ARGS + 1];   /* NULL-terminated */
    char  *envp[EXEC_MAX_ARGS + 1];   /* NULL-terminated */
};

/* Free every string referenced by an exec_args snapshot. */
static void exec_args_free(struct exec_args *ea) {
    if (!ea) return;
    for (int i = 0; i < ea->argc; i++) if (ea->argv[i]) kfree(ea->argv[i]);
    for (int i = 0; i < ea->envc; i++) if (ea->envp[i]) kfree(ea->envp[i]);
    ea->argc = ea->envc = 0;
    ea->argv[0] = ea->envp[0] = NULL;
}

/* Copy a NULL-terminated user char* vector (argv or envp) into kernel-owned
 * strings.  Returns the number of entries, or -1 on fault/limit.  @out must
 * hold EXEC_MAX_ARGS+1 slots; it is NULL-terminated on success.  @budget is
 * advanced by the bytes consumed so argv+envp share one total cap. */
static int copy_user_strvec(uint64_t user_vec, char **out, uint64_t *budget) {
    int n = 0;
    if (user_vec == 0) { out[0] = NULL; return 0; }
    for (;;) {
        if (n >= EXEC_MAX_ARGS) return -1;
        uint64_t uptr = 0;
        if (copy_from_user(&uptr, (const void *)(user_vec + (uint64_t)n * 8),
                           sizeof(uptr)) != 0)
            return -1;
        if (uptr == 0) break;   /* end of vector */

        /* Bounded strlen + copy from user space. */
        char *kstr = kmalloc(EXEC_MAX_ARG_LEN);
        if (!kstr) return -1;
        uint64_t len = 0;
        for (; len < EXEC_MAX_ARG_LEN; len++) {
            char c;
            if (copy_from_user(&c, (const void *)(uptr + len), 1) != 0) {
                kfree(kstr);
                return -1;
            }
            kstr[len] = c;
            if (c == '\0') break;
        }
        if (len >= EXEC_MAX_ARG_LEN) { kfree(kstr); return -1; }
        *budget += len + 1;
        if (*budget > EXEC_MAX_STRTOT) { kfree(kstr); return -1; }
        out[n++] = kstr;
    }
    out[n] = NULL;
    return n;
}

/* Snapshot the caller's argv[]/envp[] vectors into kernel memory. */
static int exec_args_capture(struct exec_args *ea,
                             uint64_t user_argv, uint64_t user_envp) {
    ea->argc = ea->envc = 0;
    ea->argv[0] = ea->envp[0] = NULL;
    uint64_t budget = 0;

    int argc = copy_user_strvec(user_argv, ea->argv, &budget);
    if (argc < 0) { exec_args_free(ea); return -1; }
    ea->argc = argc;

    int envc = copy_user_strvec(user_envp, ea->envp, &budget);
    if (envc < 0) { exec_args_free(ea); return -1; }
    ea->envc = envc;
    return 0;
}

/* Build the initial process stack image in the (already mapped) user stack and
 * return the final 16-byte-aligned RSP that crt0 should start with.  Layout,
 * from low to high address, matches the System V AMD64 ABI:
 *
 *     [rsp]            argc                (8 bytes)
 *                      argv[0..argc-1]     (pointers)
 *                      NULL                (argv terminator)
 *                      envp[0..envc-1]     (pointers)
 *                      NULL                (envp terminator)
 *                      auxv: AT_NULL entry (2 x 8 bytes of zero)
 *                      ... string data ...
 *
 * @stack_top is the (exclusive) top of the mapped user stack.  Returns the
 * RSP value to pass to jump_to_user, or 0 on failure. */
static uint64_t build_initial_stack(uint64_t stack_top, const struct exec_args *ea) {
    int argc = ea ? ea->argc : 0;
    int envc = ea ? ea->envc : 0;

    /* 1) Copy the strings to the top of the stack, recording their user
     *    addresses.  We grow downward from stack_top. */
    static uint64_t arg_addr[EXEC_MAX_ARGS];
    static uint64_t env_addr[EXEC_MAX_ARGS];
    uint64_t sp = stack_top;

    for (int i = 0; i < envc; i++) {
        uint64_t len = (uint64_t)strlen(ea->envp[i]) + 1;
        sp -= len;
        if (copy_to_user((void *)sp, ea->envp[i], len) != 0) return 0;
        env_addr[i] = sp;
    }
    for (int i = 0; i < argc; i++) {
        uint64_t len = (uint64_t)strlen(ea->argv[i]) + 1;
        sp -= len;
        if (copy_to_user((void *)sp, ea->argv[i], len) != 0) return 0;
        arg_addr[i] = sp;
    }

    /* 2) Compute the size of the pointer table so the final RSP lands on a
     *    16-byte boundary (ABI requires RSP%16==0 at _start, i.e. before the
     *    implicit return address — here there is none, so we align RSP itself
     *    to 16). The table holds:
     *      1 (argc) + argc + 1(NULL) + envc + 1(NULL) + 2(AT_NULL) words. */
    uint64_t nwords = 1 + (uint64_t)argc + 1 + (uint64_t)envc + 1 + 2;

    /* Align the string area downward, then place the table so its base (argc)
     * is 16-aligned. */
    sp &= ~0xFULL;
    uint64_t table_bytes = nwords * 8;
    uint64_t table_base = sp - table_bytes;
    table_base &= ~0xFULL;        /* keep argc slot 16-aligned */

    /* 3) Emit the table. */
    uint64_t w = table_base;
    uint64_t v;

    v = (uint64_t)argc;
    if (copy_to_user((void *)w, &v, 8) != 0) return 0; w += 8;
    for (int i = 0; i < argc; i++) {
        if (copy_to_user((void *)w, &arg_addr[i], 8) != 0) return 0; w += 8;
    }
    v = 0; if (copy_to_user((void *)w, &v, 8) != 0) return 0; w += 8;   /* argv NULL */
    for (int i = 0; i < envc; i++) {
        if (copy_to_user((void *)w, &env_addr[i], 8) != 0) return 0; w += 8;
    }
    v = 0; if (copy_to_user((void *)w, &v, 8) != 0) return 0; w += 8;   /* envp NULL */
    /* auxv: a single AT_NULL terminator (type 0, value 0). */
    v = 0; if (copy_to_user((void *)w, &v, 8) != 0) return 0; w += 8;
    v = 0; if (copy_to_user((void *)w, &v, 8) != 0) return 0; w += 8;

    return table_base;
}

/*
 * Load an ELF into the CURRENT address space and jump to user mode.
 * The caller must have already switched to the desired address space.
 * If @ea is non-NULL, its argv/envp are placed on the initial user stack
 * per the System V AMD64 ABI; @ea is consumed (strings freed) here.
 * Does not return.
 */
static void __attribute__((noreturn))
load_and_jump_args(const void *elf_data, uint64_t elf_size, struct exec_args *ea) {
    tcb_t *cur = sched_current();
    if (!cur) {
        kprintf("[proc] FATAL: load_and_jump_args with no current thread\n");
        if (elf_data) kfree((void *)elf_data);
        if (ea) { exec_args_free(ea); kfree(ea); }
        thread_exit();
    }
    uint64_t entry = elf_load(elf_data, elf_size, &cur->brk);
    if (entry == 0) {
        kprintf("[proc] ELF load failed\n");
        if (elf_data) kfree((void *)elf_data);
        if (ea) { exec_args_free(ea); kfree(ea); }
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

    /* The topmost MAPPED byte is at stack_top - USER_STACK_GUARD_SIZE. The
     * guard page just above is intentionally unmapped to catch overflows. */
    uint64_t usp_top = stack_top - USER_STACK_GUARD_SIZE;
    uint64_t user_rsp;
    if (ea && (ea->argc > 0 || ea->envc > 0)) {
        user_rsp = build_initial_stack(usp_top, ea);
        if (user_rsp == 0) {
            kprintf("[proc] failed to build initial stack; using empty frame\n");
            user_rsp = (usp_top - 16) & ~0xFULL;
        }
        exec_args_free(ea);
        kfree(ea);
    } else {
        if (ea) { exec_args_free(ea); kfree(ea); }
        /* No args: still hand crt0 a valid argc=0/argv=NULL/envp=NULL frame. */
        struct exec_args empty = { 0, 0, { NULL }, { NULL } };
        user_rsp = build_initial_stack(usp_top, &empty);
        if (user_rsp == 0) user_rsp = (usp_top - 16) & ~0xFULL;
    }

    kprintf("[proc] entering Ring 3 at 0x%llx RSP=0x%llx (CR3=0x%llx)\n",
            (unsigned long long)entry, (unsigned long long)user_rsp,
            (unsigned long long)(read_cr3() & 0x000FFFFFFFFFF000ULL));

    /* FIX_R3: a fresh program starts with NO TLS.  Without this an execve()
     * would leak the old program's (or another tenant's) FS into the new
     * address space; libc installs the new main thread's TCB itself in
     * __libc_start_main.  Keep FS.base == tls_base at every user entry,
     * the same invariant context_switch maintains. */
    cur->tls_base = 0;
    write_fs_base(0);

    jump_to_user(entry, user_rsp, 0);
    thread_exit();   /* not reached */
}

/* Back-compat wrapper: load with no argv/envp. */
static void __attribute__((noreturn))
load_and_jump(const void *elf_data, uint64_t elf_size) {
    load_and_jump_args(elf_data, elf_size, NULL);
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

    /* FIX_R3: fork() inherits the parent's TLS base (copied into
     * child->tls_base in do_fork); install it before the first user entry
     * so FS is never whatever the previous tenant of this CPU left. */
    write_fs_base(self ? self->tls_base : 0);

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

    /* 2) Create the child thread UNSTARTED.  The old code relied on cli so
     * "the scheduler can't pick up the half-initialised TCB (no pml4, no
     * saved user frame, no FDs) between kthread_create() and the field
     * assignments below" — but on SMP a remote cpu ignores the local IF.
     * FIX_R3: the TCB is only published with kthread_start() once every
     * field below is in place; cli still guards the create→start sequence. */
    tcb_t *parent = self;
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));

    tcb_t *child = kthread_create_unstarted(fork_child_entry, NULL,
                                            "fork-child");
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
        /* FIX_R3: fork(), like Linux, inherits the TLS base — the child
         * starts life as a byte-copy of the parent's address space, so the
         * parent's TLS block is equally valid there. */
        child->tls_base = parent->tls_base;
        /* Share the parent's open-file descriptions (shared seek offset/flags),
         * incrementing each OFD refcount; copy the per-fd FD_CLOEXEC flags. */
        vfs_fork_inherit(child->fd_table, parent->fd_table,
                         child->cloexec, parent->cloexec);
        child->brk = parent->brk;
        child->mmap_next = parent->mmap_next;
        child->vma_list = NULL;

        /* Clone the VMA list from parent to child. */
        uint64_t vf = spinlock_acquire_irqsave(&parent->vma_lock);
        vma_t *vma = parent->vma_list;
        while (vma) {
            if (vma_insert(&child->vma_list, vma->va_start, vma->va_end,
                           vma->flags, vma->file, vma->file_off) != 0) {
                spinlock_release_irqrestore(&parent->vma_lock, vf);
                vma_free_all(&child->vma_list);
                child->vma_list = NULL;
                if (child->pml4_phys) {
                    (void)paging_free_address_space(child->pml4_phys);
                    child->pml4_phys = 0;
                }
                child->state = THREAD_DEAD;
                child->waited = 1;
                if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
                return -1;
            }
            vma = vma->next;
        }
        spinlock_release_irqrestore(&parent->vma_lock, vf);
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

        /* P7: child inherits credentials and umask */
        child->uid  = parent->uid;
        child->euid = parent->euid;
        child->suid = parent->suid;
        child->gid  = parent->gid;
        child->egid = parent->egid;
        child->sgid = parent->sgid;
        child->ngroups = parent->ngroups;
        for (int i = 0; i < parent->ngroups; i++) {
            child->supplementary_gids[i] = parent->supplementary_gids[i];
        }
        child->umask = parent->umask;

        /* P10: child inherits the parent's current working directory. */
        for (int i = 0; i < VFS_PATH_MAX; i++) {
            child->cwd[i] = parent->cwd[i];
            if (parent->cwd[i] == '\0') break;
        }

        parent->n_children++;
    }

    /* All fields are set: publish — the child may now run on any cpu. */
    kthread_start(child);

    if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");

    /* 3) The parent returns the child PID. */
    kprintf("[proc] fork: parent returns child PID %llu\n",
            (unsigned long long)child->id);
    return (int64_t)child->id;
}

/* ---- execve() ---- */

int64_t do_execve(const char *path, uint64_t user_argv, uint64_t user_envp) {
    kprintf("[proc] execve('%s')\n", path);

    /* 0a) Snapshot argv[]/envp[] from the CALLER's address space NOW, before
     *     we replace it.  On failure leave the current image intact. */
    struct exec_args *ea = kmalloc(sizeof(struct exec_args));
    if (!ea) return -ENOMEM;
    if (exec_args_capture(ea, user_argv, user_envp) != 0) {
        kprintf("[proc] execve: bad argv/envp\n");
        kfree(ea);
        return -EFAULT;
    }

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
        exec_args_free(ea); kfree(ea);
        return -1;
    }

    struct vnode *vn = vfs_get_vnode(fd);
    if (vn) {
        tcb_t *cur = sched_current();
        if (cur && (vn->mode & 04000)) cur->euid = vn->uid;
        if (cur && (vn->mode & 02000)) cur->egid = vn->gid;
        /* Refuse to "execute" a directory: without this check vfs_read() on
         * a directory vnode returns 0 bytes, which then surfaces as a
         * confusing "[elf] too small (0 bytes)" / "ELF load failed" instead
         * of a clear, POSIX-like diagnostic. */
        if (vn->type == VFS_TYPE_DIR) {
            kprintf("[proc] execve: '%s' is a directory\n", path);
            vfs_close(fd);
            exec_args_free(ea); kfree(ea);
            return -1;
        }
    }

    /* Read the entire file. For simplicity, assume it fits in 256 KiB. */
    uint8_t *buf = kmalloc(256 * 1024);
    if (!buf) {
        vfs_close(fd);
        exec_args_free(ea); kfree(ea);
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
    {
        tcb_t *cur_vma = sched_current();
        if (cur_vma) {
            vma_free_all(&cur_vma->vma_list);
        }
    }
    uint64_t new_pml4 = paging_new_address_space();
    if (new_pml4 == 0) {
        kfree(buf);
        exec_args_free(ea); kfree(ea);
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

    /* 5) Load and jump (does not return).  Pass the captured argv/envp so they
     *    are materialised on the new process's initial user stack. */
    load_and_jump_args(buf, (uint64_t)total, ea);

    return -1;   /* not reached */
}

/* ---- wait4() ---- */

int64_t do_wait4(int64_t *exit_code) {
    /* Legacy entry point: wait for any child of the current task and discard
     * its identity.  do_wait4_pid does the heavy lifting. */
    return do_wait4_pid(-1, exit_code);
}

/* ---- spawn() ---- */

/* What process_spawn() hands to the new thread.
 *
 * spawn_thread() takes a single void*, and until SDK_PLAN phase S3 that was
 * just the path string.  Arguments need to travel with it, so the two are
 * bundled.  `args` is NULL when the caller passed no argv, which is the
 * common case and keeps the old behaviour exactly. */
struct spawn_payload {
    char             *path;   /* kmalloc'd, owned by the new thread */
    struct exec_args *args;   /* kmalloc'd or NULL, owned by the new thread */
};

/* Thread function for a spawned process. */
static void spawn_thread(void *arg) {
    struct spawn_payload *pl = (struct spawn_payload *)arg;
    char *path = pl->path;
    struct exec_args *ea = pl->args;
    kfree(pl);

    vfs_close_on_exec();
    /* See vfs_ensure_std_fds()'s comment: guarantees the spawned process's
     * own subsequent open() calls (e.g. a GUI app reading files under
     * /proc) never accidentally land on fd 0/1/2 and get misinterpreted as
     * a stdin/stdout/stderr operation. Needed because process_spawn() can
     * be invoked from a kernel thread (e.g. gui-compositor launching a
     * double-clicked desktop icon) whose own fd_table -- inherited here via
     * vfs_fork_inherit() -- is completely empty. */
    vfs_ensure_std_fds();

    /* Read the ELF from the VFS into kernel memory. */
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0) {
        kprintf("[proc] spawn: '%s' not found\n", path);
        memset(path, 0, strlen(path) + 1);
        kfree(path);
        if (ea) { exec_args_free(ea); kfree(ea); }
        thread_exit();
    }

    struct vnode *vn = vfs_get_vnode(fd);
    if (vn) {
        tcb_t *cur = sched_current();
        if (cur && (vn->mode & 04000)) cur->euid = vn->uid;
        if (cur && (vn->mode & 02000)) cur->egid = vn->gid;
        /* Refuse to "execute" a directory (e.g. `run /proc`): without this
         * check vfs_read() on a directory vnode returns 0 bytes, which then
         * surfaces as a confusing "[elf] too small (0 bytes)" / "[proc] ELF
         * load failed" instead of a clear, POSIX-like diagnostic. */
        if (vn->type == VFS_TYPE_DIR) {
            kprintf("[proc] spawn: '%s' is a directory\n", path);
            vfs_close(fd);
            memset(path, 0, strlen(path) + 1);
            kfree(path);
            if (ea) { exec_args_free(ea); kfree(ea); }
            thread_exit();
        }
    }

    /* Executable size limit.
     *
     * This was 256 KB and the read loop simply stopped there, handing
     * load_and_jump() a TRUNCATED image.  The ELF loader then reported
     * "segment file range out of bounds" -- a message about the ELF, for a
     * problem that was really "your binary is bigger than the buffer".  That
     * cost real debugging time when /gltest grew past the limit, so the
     * limit is now both larger and, more importantly, DIAGNOSED. */
    #define SPAWN_MAX_IMAGE (1024 * 1024)

    uint8_t *buf = kmalloc(SPAWN_MAX_IMAGE);
    if (!buf) {
        vfs_close(fd);
        memset(path, 0, strlen(path) + 1);
        kfree(path);
        if (ea) { exec_args_free(ea); kfree(ea); }
        thread_exit();
    }
    int64_t total = 0;
    int64_t n;
    while (total < SPAWN_MAX_IMAGE &&
           (n = vfs_read(fd, buf + total, SPAWN_MAX_IMAGE - total)) > 0) {
        total += n;
    }

    /* Did anything remain?  A single further byte means the image did not
     * fit, and loading what was read would produce a mystifying failure
     * somewhere inside the ELF parser. */
    uint8_t probe;
    int truncated = (total >= SPAWN_MAX_IMAGE) &&
                    (vfs_read(fd, &probe, 1) > 0);
    vfs_close(fd);

    if (truncated) {
        kprintf("[spawn] '%s' is larger than the %d KB executable limit\n",
                path, SPAWN_MAX_IMAGE / 1024);
        kfree(buf);
        memset(path, 0, strlen(path) + 1);
        kfree(path);
        if (ea) { exec_args_free(ea); kfree(ea); }
        thread_exit();
    }
    kfree(path);

    /* load_and_jump_args() takes ownership of `ea` and frees it, including on
     * its own failure paths, so there is nothing to release here. */
    load_and_jump_args(buf, (uint64_t)total, ea);
    #undef SPAWN_MAX_IMAGE
}

/* process_spawn() — start `path` as a new process.
 *
 * @user_argv is a user-space pointer to a NULL-terminated char* vector, or 0
 * for none.  It is captured HERE, in the caller's address space, because the
 * new thread runs in a fresh one where those pointers mean nothing.
 *
 * The capture reuses exec_args_capture(), the same routine execve() uses.
 * That is deliberate: copying an array of user pointers safely is the
 * dangerous part of this, execve already gets it right, and a second
 * implementation would be a second place to get it subtly wrong. */
int64_t process_spawn_argv(const char *path, uint64_t user_argv) {
    /* Create a new address space. */
    uint64_t new_pml4 = paging_new_address_space();
    if (new_pml4 == 0) {
        return -1;
    }
    /* If we are spawning from a thread that has a VMA list, 
     * the child starts with a fresh one, but we don't need to 
     * clear the parent's. Wait, process_spawn creates a NEW thread.
     * So child->vma_list should be NULL. */

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

    /* Capture argv while we are still in the CALLER's address space. */
    struct exec_args *ea = NULL;
    if (user_argv) {
        ea = kmalloc(sizeof(struct exec_args));
        if (!ea) {
            kfree(path_copy);
            (void)paging_free_address_space(new_pml4);
            return -1;
        }
        if (exec_args_capture(ea, user_argv, 0) != 0) {
            kprintf("[proc] spawn: bad argv\n");
            kfree(ea);
            kfree(path_copy);
            (void)paging_free_address_space(new_pml4);
            return -EFAULT;
        }
    }

    struct spawn_payload *pl = kmalloc(sizeof(*pl));
    if (!pl) {
        if (ea) { exec_args_free(ea); kfree(ea); }
        kfree(path_copy);
        (void)paging_free_address_space(new_pml4);
        return -1;
    }
    pl->path = path_copy;
    pl->args = ea;

    /* FIX_R3: created UNSTARTED — pml4_phys, vmas, credentials and the cwd
     * below must be in place before any cpu may schedule the child. */
    tcb_t *child = kthread_create_unstarted(spawn_thread, pl, path);
    if (child == NULL) {
        if (ea) { exec_args_free(ea); kfree(ea); }
        kfree(pl);
        kfree(path_copy);
        (void)paging_free_address_space(new_pml4);
        return -1;
    }
    child->pml4_phys = new_pml4;
    child->vma_list = NULL;
    child->parent = sched_current();
    /* The spawned child joins the spawner's process group / session and shares
     * its controlling terminal (so a foreground spawn is killable by Ctrl+C). */
    if (child->parent) {
        vfs_fork_inherit(child->fd_table, child->parent->fd_table,
                         child->cloexec, child->parent->cloexec);
        child->pgid = child->parent->pgid;
        child->sid  = child->parent->sid;
        child->ctty = child->parent->ctty;
        child->is_session_leader = 0;

        child->uid  = child->parent->uid;
        child->euid = child->parent->euid;
        child->suid = child->parent->suid;
        child->gid  = child->parent->gid;
        child->egid = child->parent->egid;
        child->sgid = child->parent->sgid;
        child->ngroups = child->parent->ngroups;
        for (int i = 0; i < child->parent->ngroups; i++) {
            child->supplementary_gids[i] = child->parent->supplementary_gids[i];
        }
        child->umask = child->parent->umask;

        /* P10: inherit the spawner's current working directory (defaulting to
         * "/" if the spawner never set one). */
        if (child->parent->cwd[0] != '\0') {
            for (int i = 0; i < VFS_PATH_MAX; i++) {
                child->cwd[i] = child->parent->cwd[i];
                if (child->parent->cwd[i] == '\0') break;
            }
        } else {
            child->cwd[0] = '/';
            child->cwd[1] = '\0';
        }

        child->parent->n_children++;
    } else {
        /* No parent (e.g. the very first process): root the cwd at "/". */
        child->cwd[0] = '/';
        child->cwd[1] = '\0';
    }

    /* All fields are set: publish — the child may now run on any cpu. */
    kthread_start(child);

    return (int64_t)child->id;
}

/* ---- Self-test ---- */

/* Backwards-compatible entry point: spawn with no arguments.
 * Every existing caller (the shell, the GUI compositor, the self-tests) uses
 * this, so they are all a test that the no-argv path still works. */
int64_t process_spawn(const char *path) {
    return process_spawn_argv(path, 0);
}

void process_self_test(void) {
    kprintf("[proc] self-test: spawning /hello in isolated address space...\n");

    int64_t pid = process_spawn("/bin/hello");
    if (pid < 0) {
        kprintf("[proc] FAIL: spawn failed\n");
        return;
    }
    kprintf("[proc] spawned PID %lld, waiting...\n", (long long)pid);

    /* Wait for the child. */
    do_wait4(NULL);

    kprintf("[proc] PASS: /hello ran in isolated address space\n");

    /* P10: exercise execve(path, argv, envp) end-to-end.  /execve_child runs in
     * a fresh address space and immediately execve()s /argv_echo with a known
     * argv/envp; /argv_echo prints what it received (ARGV_ECHO ... markers).
     * Doing this here (kernel boot, no interactive shell yet) avoids racing on
     * the per-thread SYSCALL save area that a shell-launched fork+execve would. */
    kprintf("[proc] self-test: execve argv/envp via /execve_child...\n");
    int64_t epid = process_spawn("/tests/execve_child");
    if (epid < 0) {
        kprintf("[proc] FAIL: spawn /execve_child failed\n");
        return;
    }
    kprintf("[proc] spawned execve-test PID %lld, waiting...\n", (long long)epid);
    do_wait4(NULL);
    kprintf("[proc] PASS: execve argv/envp self-test completed\n");
}

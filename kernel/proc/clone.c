/* kernel/proc/clone.c — clone(2), arch_prctl(2), futex(2), tkill(2)  (P9)
 *
 * Real pthread support: do_clone() with CLONE_THREAD spawns a new task that
 * SHARES the caller's address space (same pml4_phys), runs on a caller-supplied
 * user stack, and (optionally) gets its own TLS base via CLONE_SETTLS.  The new
 * thread returns 0 from clone() in user mode, exactly like the main thread saw
 * a return value (the child PID) — mirroring fork()'s child trampoline.
 */

#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/signal.h"
#include "kernel/proc/usercopy.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/kprintf.h"
#include "kernel/sync/futex.h"
#include "kernel/arch/x86_64/paging.h"
#include <stdint.h>

/* Linux-compatible clone flags (subset used by the pthread runtime). */
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

/* arch_prctl codes. */
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_SET_GS 0x1001
#define ARCH_GET_GS 0x1004

/* User-mode return frame captured by the syscall entry stub. */
extern uint64_t syscall_saved_rcx;   /* user RIP   */
extern uint64_t syscall_saved_r11;   /* user RFLAGS */
extern uint64_t syscall_saved_rsp;   /* user RSP   */

extern void tss_set_rsp0(uint64_t rsp0);
extern void set_syscall_stack(uint64_t rsp0);
extern void fork_child_sysret(uint64_t rip, uint64_t rflags, uint64_t rsp);

/* First-run trampoline for a cloned thread: enter user mode at the clone
 * return site, on the new thread's own user stack, with RAX = 0. */
static void clone_thread_entry(void *arg) {
    (void)arg;
    tcb_t *self = sched_current();
    if (!self) thread_exit();

    /* Program the kernel/syscall stack for this thread. */
    if (self->kernel_stack) {
        uint64_t kstack = (uint64_t)self->kernel_stack + THREAD_STACK_SIZE;
        tss_set_rsp0(kstack);
        set_syscall_stack(kstack);
    }

    /* fork_child_sysret sets RAX=0 and SYSRETs to (rip, rflags, rsp). */
    fork_child_sysret(self->fork_user_rip, self->fork_user_rflags,
                      self->fork_user_rsp);
    thread_exit();   /* not reached */
}

int64_t do_clone(uint64_t flags, uint64_t stack, uint64_t ptid,
                 uint64_t ctid, uint64_t tls) {
    tcb_t *parent = sched_current();
    if (!parent) return -EINVAL;

    /* We only implement the thread-creation path (CLONE_VM|CLONE_THREAD).
     * fork()-style clone (no CLONE_VM) is handled by SYS_FORK already. */
    if (!(flags & CLONE_THREAD) || !(flags & CLONE_VM))
        return -ENOSYS;

    /* A new thread must run on its own user stack. */
    if (stack == 0)
        return -EINVAL;

    /* Capture the user-mode return frame for the new thread.  The caller's
     * saved_user_* fields hold the SYSCALL entry frame; fall back to the live
     * globals when not yet recorded. */
    uint64_t user_rip = parent->saved_user_rip ? parent->saved_user_rip
                                               : syscall_saved_rcx;
    uint64_t user_rflags = parent->saved_user_rip ? parent->saved_user_rflags
                                                  : syscall_saved_r11;

    /* Create the child with interrupts disabled so the scheduler can't run it
     * before its fields are initialised. */
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags));

    tcb_t *child = kthread_create(clone_thread_entry, NULL, "pthread");
    if (!child) {
        if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");
        return -ENOMEM;
    }

    /* Threads SHARE the parent's address space. */
    child->pml4_phys = parent->pml4_phys;
    child->parent    = parent;

    /* User return frame: resume at the clone call site, on the new stack. */
    child->fork_user_rip    = user_rip;
    child->fork_user_rflags = user_rflags;
    child->fork_user_rsp    = stack;

    /* Thread-group bookkeeping. */
    child->tgid       = parent->tgid ? parent->tgid : parent->id;
    child->is_pthread = 1;

    /* CLONE_FILES: share the parent's FD table (pointer copy + refcount). */
    vfs_fork_inherit(child->fd_table, parent->fd_table,
                     child->cloexec, parent->cloexec);

    /* Inherit credentials, pgid/sid, controlling tty, cwd, signal dispositions. */
    child->uid = parent->uid; child->euid = parent->euid; child->suid = parent->suid;
    child->gid = parent->gid; child->egid = parent->egid; child->sgid = parent->sgid;
    child->ngroups = parent->ngroups;
    for (int i = 0; i < parent->ngroups; i++)
        child->supplementary_gids[i] = parent->supplementary_gids[i];
    child->umask = parent->umask;
    child->pgid = parent->pgid;
    child->sid  = parent->sid;
    child->ctty = parent->ctty;
    child->is_session_leader = 0;
    for (int s = 0; s < NSIG; s++) child->sig_actions[s] = parent->sig_actions[s];
    child->sig_mask = parent->sig_mask;
    child->brk = parent->brk;
    child->mmap_next = parent->mmap_next;
    for (int i = 0; i < VFS_PATH_MAX && parent->cwd[i]; i++) child->cwd[i] = parent->cwd[i];

    /* CLONE_SETTLS: install the thread's TLS base (applied via FS.base on the
     * next context switch — see context.asm). */
    if (flags & CLONE_SETTLS)
        child->tls_base = tls;

    /* CLONE_CHILD_CLEARTID: on thread exit, zero *ctid and futex-wake it. */
    if (flags & CLONE_CHILD_CLEARTID)
        child->clear_tid_addr = ctid;

    parent->n_children++;

    if (rflags & 0x200ULL) __asm__ volatile ("sti" ::: "memory");

    /* CLONE_PARENT_SETTID / CLONE_CHILD_SETTID: publish the new TID. */
    if (flags & CLONE_PARENT_SETTID) {
        uint32_t tid = (uint32_t)child->id;
        (void)copy_to_user((void *)(uintptr_t)ptid, &tid, sizeof(tid));
    }
    if (flags & CLONE_CHILD_SETTID) {
        uint32_t tid = (uint32_t)child->id;
        (void)copy_to_user((void *)(uintptr_t)ctid, &tid, sizeof(tid));
    }

    return (int64_t)child->id;
}

int64_t do_arch_prctl(int code, uint64_t addr) {
    tcb_t *cur = sched_current();
    if (!cur) return -EINVAL;

    switch (code) {
    case ARCH_SET_FS:
        cur->tls_base = addr;
        __asm__ volatile ("wrfsbase %0" :: "r"(addr));
        return 0;
    case ARCH_GET_FS:
        if (copy_to_user((void *)(uintptr_t)addr, &cur->tls_base,
                         sizeof(cur->tls_base)) != 0)
            return -EFAULT;
        return 0;
    default:
        return -EINVAL;
    }
}

int64_t do_futex(uint64_t uaddr, int op, uint32_t val, uint64_t timeout,
                 uint32_t *uaddr2, uint32_t val3) {
    (void)timeout; (void)uaddr2; (void)val3;

    /* FUTEX_WAIT == 0, FUTEX_WAKE == 1 (low bits; ignore PRIVATE flag 128). */
    int cmd = op & 0x7f;
    switch (cmd) {
    case 0: /* FUTEX_WAIT */
        return futex_wait((uint32_t *)(uintptr_t)uaddr, val);
    case 1: /* FUTEX_WAKE */
        return futex_wake((uint32_t *)(uintptr_t)uaddr, (int)val);
    default:
        return -ENOSYS;
    }
}

int64_t do_tkill(int64_t tid, int sig) {
    if (sig < 0 || sig >= NSIG) return -EINVAL;
    tcb_t *t = thread_get_by_pid((uint64_t)tid);
    if (!t) return -ESRCH;
    if (sig != 0) signal_send(t, sig);
    return 0;
}

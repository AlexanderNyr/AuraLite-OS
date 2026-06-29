#ifndef AURALITE_PROC_THREAD_H
#define AURALITE_PROC_THREAD_H

#include <stdint.h>
#include <stddef.h>
#include "kernel/fs/vfs.h"
#include "kernel/proc/signal.h"
#include "kernel/time_types.h"

/*
 * Thread Control Block (also serves as the Process Control Block).
 *
 * The TCB's first field MUST be `rsp` (offset 0) so that context_switch
 * (context.asm) can save/load it with a single [rdi]/[rsi] access.
 *
 * Phase 15 extension: per-process address spaces. A user process has its own
 * PML4 (pml4_phys != 0); kernel threads share the kernel's address space
 * (pml4_phys == 0).
 */

#define THREAD_NAME_MAX    64
#define THREAD_STACK_SIZE  (16 * 1024)   /* 16 KiB usable per kernel thread */
#define THREAD_STACK_GUARD_PAGES 1
#define THREAD_STACK_PAGES       (THREAD_STACK_SIZE / 4096)
#define SCHED_QUANTUM      5              /* default tick slice (50ms @100Hz) */

/* Max processes (for the wait/child-tracking arrays). */
#define MAX_PROCS 64

enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DEAD,
};

struct tty;   /* kernel/tty/tty.h — controlling terminal */

typedef struct tcb {
    uint64_t  rsp;               /* offset 0: saved stack pointer            */
    void     *kernel_stack;      /* base of the usable stack                 */
    void     *kernel_stack_region; /* full mapped slot incl. guard pages      */
    int       kernel_stack_slot; /* guarded-stack allocator slot, or -1      */
    uint64_t  kernel_stack_phys[THREAD_STACK_PAGES]; /* backing frames       */
    uint64_t  id;                /* unique thread/process ID (= PID)         */
    char      name[THREAD_NAME_MAX];
    int       state;             /* enum thread_state                        */
    uint64_t  quantum;           /* remaining ticks before preemption        */
    void    (*entry)(void *);    /* thread function                          */
    void     *arg;               /* argument passed to entry                 */
    struct tcb *next;            /* run-queue linkage (singly-linked)        */
    /* ---- Process fields (per-process address spaces) ---- */
    uint64_t  pml4_phys;         /* this process's PML4 physical addr (0=kernel) */
    int       exit_code;         /* exit status for wait4                    */
    struct tcb *parent;          /* parent process (NULL for kernel threads) */
    /*
     * Zombie / wait4 bookkeeping.  When a child enters THREAD_DEAD it stays
     * on the global zombie list until its parent calls wait4(); only then is
     * `waited` flipped to 1 and the next thread_reap_zombies() pass actually
     * releases the TCB/stack and address space.  Orphaned children (whose
     * parent exited first) get `waited` set as part of the parent's exit
     * path so they are eligible for immediate reaping.
     */
    volatile int waited;
    struct ofd *fd_table[VFS_MAX_FDS]; /* per-process FD table: pointers to shared OFDs */
    uint8_t cloexec[VFS_MAX_FDS];      /* per-fd close-on-exec flags (FD_CLOEXEC == 1) */
    
    /* Program break (brk) / heap tracking. */
    uint64_t  brk;               /* Current user heap end */
    uint64_t  mmap_next;         /* Next anonymous mmap hint address */

    /* Saved user-mode return frame used by fork()'s child to re-enter user
     * space at the exact instruction that issued the SYSCALL.  Recorded by
     * do_fork() at the moment the syscall_saved_* globals are still valid;
     * read by fork_child_entry() once the scheduler activates this TCB. */
    uint64_t  fork_user_rip;
    uint64_t  fork_user_rflags;
    uint64_t  fork_user_rsp;
    /* Per-thread copy of the SYSCALL entry frame (RCX/R11).  Captured at the
     * very top of syscall_dispatch() so that another thread which runs while
     * we are blocked inside the syscall (e.g. via wait4 yields) can safely
     * overwrite the GLOBAL syscall_saved_* without losing OUR return
     * destination.  syscall_get_saved_return() reads these back at sysret. */
    uint64_t  saved_user_rip;
    uint64_t  saved_user_rflags;
    uint64_t  saved_user_rsp;
    /* Live user callee-saved (SysV-preserved) registers captured at the SYSCALL
     * boundary.  Snapshotted into the TCB at the top of syscall_dispatch() so a
     * context switch while blocked inside the syscall cannot lose them; used to
     * build a faithful signal frame when a signal is delivered at syscall exit. */
    uint64_t  saved_user_rbx;
    uint64_t  saved_user_rbp;
    uint64_t  saved_user_r12;
    uint64_t  saved_user_r13;
    uint64_t  saved_user_r14;
    uint64_t  saved_user_r15;

    /* ---- P4: signal state (see kernel/proc/signal.h) ---- */
    uint32_t  sig_pending;            /* bitmask: bit (signo-1) pending */
    uint32_t  sig_mask;               /* blocked signals */
    struct sigaction sig_actions[NSIG]; /* per-signal disposition (index by signo) */
    uint64_t  alarm_deadline;         /* PIT tick at which SIGALRM fires (0 = off) */
    int       sig_suspend_active;     /* 1 while in sigsuspend(): use sig_suspend_restore */
    uint32_t  sig_suspend_restore;    /* mask to record in the next signal frame */

    /* ---- P6: process groups / sessions (POSIX §4.8) ---- */
    int64_t   pgid;                   /* process group ID (leader: pgid == id) */
    int64_t   sid;                    /* session ID (leader: sid == id) */
    int       is_session_leader;
    struct tty *ctty;                 /* controlling terminal (NULL if none) */
    int       n_children;             /* live children (fork/spawn ++, reap --) */
    int       term_signal;            /* signal that killed this task (0 = exited) */

    /* ---- P7: POSIX user/group credentials ---- */
    uint32_t  uid,  euid,  suid;            /* real, effective, saved-set UID */
    uint32_t  gid,  egid,  sgid;            /* real, effective, saved-set GID */
    uint32_t  supplementary_gids[32];       /* supplementary groups */
    int       ngroups;                      /* count of supplementary_gids[] */
    uint16_t  umask;                        /* file creation mask, default 0022 */

    /* ---- P8: per-process interval timers ---- */
    struct itimer_state itimers[3]; /* [0]=ITIMER_REAL, [1]=ITIMER_VIRTUAL, [2]=ITIMER_PROF */
    uint64_t cpu_ticks;           /* ticks this process spent RUNNING */

    /* ---- P9: pthread / thread-group ---- */
    uint64_t  tgid;                /* thread group ID = PID of main thread */
    uint64_t  tls_base;            /* FS.base — WRFSBASE on context switch */
    int       detached;            /* 1 = pthread_detach() called */
    uint64_t  join_value;          /* pthread_exit() value */
    int       is_pthread;          /* 1 = userspace thread */
    uint64_t clear_tid_addr;      /* *ctid = 0 + futex_wake on exit */

    /* ---- P10: working directory ---- */
    char cwd[VFS_PATH_MAX];
} tcb_t;

/* Allocate/free a guarded kernel stack for an already-zeroed TCB. */
int  thread_alloc_kernel_stack(tcb_t *tcb);
void thread_free_kernel_stack(tcb_t *tcb);

/*
 * wait4(pid, *exit_code):
 *   pid <  0 : reap any one exited child of the current thread/process.
 *   pid >= 0 : reap that specific child (if it has exited or once it does).
 *   *exit_code receives the child's exit code if non-NULL.
 * Returns the reaped child's PID, or -1 if no matching child exists.
 */
int64_t do_wait4_pid(int64_t pid, int64_t *exit_code);

/* Find a zombie matching parent_pid + match_pid.  Internal helper for wait4. */
tcb_t *thread_find_zombie(uint64_t parent_pid, int64_t match_pid);

/*
 * Diagnostics: number of zombies queued / reaped since boot.  Used in
 * integration tests and the /proc-style status printers.
 */
uint64_t thread_zombies_queued_total(void);
uint64_t thread_zombies_reaped_total(void);

/*
 * Create a new kernel thread that will run `fn(arg)` when first scheduled.
 * The thread is added to the scheduler's ready queue.  Returns the TCB.
 */
tcb_t *kthread_create(void (*fn)(void *), void *arg, const char *name);

/*
 * Terminate the current thread.  Marks it THREAD_DEAD, wakes any waiting
 * parent, and switches to the next runnable thread.  Never returns.
 */
void thread_exit(void) __attribute__((noreturn));

/* Same as thread_exit() but records the given exit code on the TCB so a
 * subsequent wait4() can return it. */
void thread_exit_with_code(int code) __attribute__((noreturn));
/* Terminate the current task because of signal @signo (sets the wait-status
 * WIFSIGNALED encoding).  Exit code is recorded as 128+signo for legacy paths. */
void thread_exit_with_signal(int signo) __attribute__((noreturn));

/*
 * waitpid(pid, *status, options) — POSIX child wait.
 *   pid > 0  : that child;  pid == 0 : any child in caller's pgid;
 *   pid == -1: any child;   pid < -1 : any child in group |pid|.
 *   options  : WNOHANG (1) -> return 0 if no child ready; WUNTRACED (2).
 * @status (if non-NULL) receives the POSIX wait-status word.
 * Returns the reaped PID, 0 (WNOHANG, none ready), or -ECHILD / -EINVAL.
 */
int64_t do_waitpid(int64_t pid, int *status, int options);

/* Free THREAD_DEAD TCBs/stacks that have already switched off their own stack.
 * Safe to call from normal kernel-thread context; it never frees current. */
void thread_reap_zombies(void);

/* Register a TCB in the global all_threads array (called on creation/init). */
void thread_register_tcb(tcb_t *tcb);
/* Deregister a TCB from the global all_threads array (called on free). */
void thread_deregister_tcb(tcb_t *tcb);
/* Get a TCB by its PID. Returns NULL if not found. */
tcb_t *thread_get_by_pid(uint64_t pid);
/* Get all registered TCBs. Returns count. */
int thread_get_all(tcb_t *out_list[], int max);

#endif /* AURALITE_PROC_THREAD_H */

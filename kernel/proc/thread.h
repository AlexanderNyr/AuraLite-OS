#ifndef AURALITE_PROC_THREAD_H
#define AURALITE_PROC_THREAD_H

#include <stdint.h>
#include <stddef.h>
#include "kernel/fs/vfs.h"

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
#define THREAD_STACK_SIZE  (16 * 1024)   /* 16 KiB per kernel thread */
#define SCHED_QUANTUM      5              /* default tick slice (50ms @100Hz) */

/* Max processes (for the wait/child-tracking arrays). */
#define MAX_PROCS 64

enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DEAD,
};

typedef struct tcb {
    uint64_t  rsp;               /* offset 0: saved stack pointer            */
    void     *kernel_stack;      /* base of the allocated stack              */
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
    struct file fd_table[VFS_MAX_FDS]; /* per-process FD table (0/1/2 reserved) */
    uint8_t cloexec[VFS_MAX_FDS];      /* close-on-exec flags (FD_CLOEXEC == 1) */
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
} tcb_t;

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

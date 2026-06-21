#ifndef AURALITE_PROC_THREAD_H
#define AURALITE_PROC_THREAD_H

#include <stdint.h>
#include <stddef.h>

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
    volatile int waited_on;      /* non-zero if a parent is blocked in wait4 */
} tcb_t;

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

#endif /* AURALITE_PROC_THREAD_H */

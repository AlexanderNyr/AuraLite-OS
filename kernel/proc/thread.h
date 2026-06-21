#ifndef AURALITE_PROC_THREAD_H
#define AURALITE_PROC_THREAD_H

#include <stdint.h>
#include <stddef.h>

/*
 * Thread Control Block and kernel-thread creation.
 *
 * The TCB's first field MUST be `rsp` (offset 0) so that context_switch
 * (context.asm) can save/load it with a single [rdi]/[rsi] access.
 */

#define THREAD_NAME_MAX    64
#define THREAD_STACK_SIZE  (16 * 1024)   /* 16 KiB per kernel thread */
#define SCHED_QUANTUM      5              /* default tick slice (50ms @100Hz) */

enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DEAD,
};

typedef struct tcb {
    uint64_t  rsp;               /* offset 0: saved stack pointer            */
    void     *kernel_stack;      /* base of the allocated stack              */
    uint64_t  id;                /* unique thread ID                         */
    char      name[THREAD_NAME_MAX];
    int       state;             /* enum thread_state                        */
    uint64_t  quantum;           /* remaining ticks before preemption        */
    void    (*entry)(void *);    /* thread function                          */
    void     *arg;               /* argument passed to entry                 */
    struct tcb *next;            /* run-queue linkage (singly-linked)        */
} tcb_t;

/*
 * Create a new kernel thread that will run `fn(arg)` when first scheduled.
 * The thread is added to the scheduler's ready queue.  Returns the TCB.
 */
tcb_t *kthread_create(void (*fn)(void *), void *arg, const char *name);

/*
 * Terminate the current thread.  Marks it THREAD_DEAD, removes it from the
 * run queue, and switches to the next runnable thread.  Never returns.
 * (The TCB + stack are currently leaked — reaping is a TODO.)
 */
void thread_exit(void) __attribute__((noreturn));

#endif /* AURALITE_PROC_THREAD_H */

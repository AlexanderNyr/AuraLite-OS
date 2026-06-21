#ifndef AURALITE_PROC_SCHEDULER_H
#define AURALITE_PROC_SCHEDULER_H

#include <stdint.h>
#include "kernel/proc/thread.h"

/*
 * Round-robin preemptive scheduler for kernel threads.
 *
 * A singly-linked ready queue holds THREAD_READY threads.  The timer IRQ
 * (Phase 6) calls sched_tick() which decrements the current thread's quantum
 * and preempts when it reaches zero.  sched_yield() allows cooperative
 * scheduling.
 *
 * Concurrency model (single CPU): scheduler state is manipulated with
 * interrupts disabled (cli).  This is sufficient because there is only one
 * execution context; SMP (Phase 12) will add per-CPU run queues and real
 * spinlocks.  NOT SMP SAFE.
 */

/* Initialise the scheduler: create the kmain and idle threads. */
void sched_init(void);

/* Add a thread to the tail of the ready queue. */
void sched_add_thread(tcb_t *tcb);

/*
 * Pick the next runnable thread and switch to it.  The current thread is
 * re-added to the queue if it is still READY.  Must be called with interrupts
 * disabled.
 */
void schedule(void);

/* Cooperative yield: disable IRQs, call schedule(), restore IRQ state. */
void sched_yield(void);

/*
 * Timer-driven preemption hook (called from the PIT IRQ handler).  Decrements
 * the current thread's quantum and calls schedule() when it expires.  No-op
 * until the scheduler is initialised.
 */
void sched_tick(void);

/* Return the currently-running thread's TCB. */
tcb_t *sched_current(void);

/* Gate self-test: two threads print interleaved messages, demonstrating both
 * cooperative (yield) and preemptive (timer-driven) context switching. */
void scheduler_self_test(void);

#endif /* AURALITE_PROC_SCHEDULER_H */

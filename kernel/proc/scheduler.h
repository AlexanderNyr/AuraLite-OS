#ifndef AURALITE_PROC_SCHEDULER_H
#define AURALITE_PROC_SCHEDULER_H

#include <stdint.h>
#include "kernel/proc/thread.h"

/*
 * Round-robin preemptive scheduler for kernel threads, SMP edition.
 *
 * Ready queues: one singly-linked ready queue PER CPU (struct cpu_local,
 * protected by the per-CPU rq_lock spinlock).  The timer IRQ (PIT on the
 * BSP, the calibrated Local APIC timer on each AP -- see lapic.c/smp.c)
 * calls sched_tick() which decrements the current thread's quantum and
 * preempts when it reaches zero.  sched_yield() allows cooperative
 * scheduling; a CPU whose own queue is empty steals from another CPU's
 * queue (sched_steal_work in scheduler_rq.c).
 *
 * Concurrency model (SMP step 3.2): per-CPU queue selection happens under
 * the per-CPU rq_lock with interrupts saved; cross-CPU enqueue/steal takes
 * the TARGET cpu's rq_lock.  Global scheduler-adjacent state (thread
 * registry, zombie list, kernel heap, page tables) is covered by its own
 * locks in thread.c/kheap.c/paging.c.
 */

/* Initialise the scheduler: create the kmain and idle threads. */
void sched_init(void);
int  sched_is_ready(void);

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
 * Block the current thread for real: state -> THREAD_BLOCKED, then
 * schedule() with interrupts off (same discipline as kernel_nanosleep),
 * restoring the interrupt flag afterwards.  Wakers (signal_send, wait
 * queues, signal_tick deadline) flip the thread back to READY and enqueue
 * it.  Extracted so host unit tests of kernel/fs/select.c can stub it
 * (cli/sti are privileged and would fault in ring 3).
 */
void kernel_block_current(void);

/*
 * Timer-driven preemption hook (called from the PIT IRQ handler).  Decrements
 * the current thread's quantum and calls schedule() when it expires.  No-op
 * until the scheduler is initialised.
 */
void sched_tick(void);

/* Return the currently-running thread's TCB. */
tcb_t *sched_current(void);

/* Enter the idle loop for an AP. */
void sched_idle(void);

/*
 * CPU usage accounting, summed over all online CPUs.  Each cpu charges its
 * own per-cpu slot from its own timer tick (Per-CPU LAPIC-calibrated timer
 * on APs, PIT on the BSP -- scheduler.c), these getters return the
 * system-wide totals.  The values are cumulative ticks and only ever
 * increase, exactly like Linux's /proc/stat jiffie counters: callers that
 * want a percentage should sample both values twice, a short interval
 * apart, and compute busy-delta / total-delta themselves (see
 * userspace/apps/gui-sysmon/gsysmon.c for a worked example). This avoids baking
 * an arbitrary sampling window into the kernel.
 */
uint64_t sched_get_total_ticks(void);
uint64_t sched_get_idle_ticks(void);
/* Busy percent over the last ~1 s, times 100 (1234 => 12.34%). */
uint32_t sched_get_busy_pct_x100(void);

/* Gate self-test: two threads print interleaved messages, demonstrating both
 * cooperative (yield) and preemptive (timer-driven) context switching. */
void scheduler_self_test(void);

#endif /* AURALITE_PROC_SCHEDULER_H */

/* kernel/arch/aarch64/thread_a64.h -- kernel threads + round-robin
 * (ARM64_PLAN A4).  thread_rv.h's sibling, fourth edition. */

#ifndef AURALITE_ARCH_AARCH64_THREAD_A64_H
#define AURALITE_ARCH_AARCH64_THREAD_A64_H

#include <stdint.h>

#define THREAD_A64_MAX     8
#define THREAD_A64_KSTACK  (16 * 1024)

#define THREAD_A64_STATE_FREE  0
#define THREAD_A64_STATE_READY 1
#define THREAD_A64_STATE_DONE  2

typedef void (*thread_a64_fn)(void *arg);

void        sched_a64_init(void);
int         thread_a64_create(const char *name, thread_a64_fn fn, void *arg);
void        sched_a64_yield(void);
void        thread_a64_exit(void) __attribute__((noreturn));
void        thread_a64_reap(void);
int         thread_a64_current_tid(void);
int         thread_a64_state(int tid);
const char *thread_a64_name(int tid);

/* Timer-trap hooks: tick marks, dispatch-tail preempts (the post-EOI
 * placement, GIC-flavoured -- sched_a64_maybe_preempt is called from
 * a64_trap AFTER gic_dispatch returns, i.e. after EOIR completed the
 * timer INTID and the TVAL re-arm un-asserted the line). */
void sched_a64_tick(void);
void sched_a64_maybe_preempt(void);

#endif /* AURALITE_ARCH_AARCH64_THREAD_A64_H */

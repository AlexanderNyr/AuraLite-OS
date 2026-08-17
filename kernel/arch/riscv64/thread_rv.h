/* kernel/arch/riscv64/thread_rv.h -- kernel threads + round-robin
 * (RISCV_PLAN V4).  thread32.h's LP64 sibling. */

#ifndef AURALITE_ARCH_RISCV64_THREAD_RV_H
#define AURALITE_ARCH_RISCV64_THREAD_RV_H

#include <stdint.h>

#define THREAD_RV_MAX     8
#define THREAD_RV_KSTACK  (16 * 1024)

#define THREAD_RV_STATE_FREE  0
#define THREAD_RV_STATE_READY 1
#define THREAD_RV_STATE_DONE  2

typedef void (*thread_rv_fn)(void *arg);

void        sched_rv_init(void);
int         thread_rv_create(const char *name, thread_rv_fn fn, void *arg);
void        sched_rv_yield(void);
void        thread_rv_exit(void) __attribute__((noreturn));
void        thread_rv_reap(void);
int         thread_rv_current_tid(void);
int         thread_rv_state(int tid);
const char *thread_rv_name(int tid);

/* Timer-trap hooks: tick marks, dispatch-tail preempts (the post-EOI
 * placement, PLIC-flavoured -- see the sched_rv_maybe_preempt call
 * site in trap.c, AFTER sbi_set_timer re-arms). */
void sched_rv_tick(void);
void sched_rv_maybe_preempt(void);

#endif /* AURALITE_ARCH_RISCV64_THREAD_RV_H */

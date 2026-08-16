/* kernel/arch/i386/thread32.h -- kernel threads + round-robin scheduler
 * (I386_PLAN I4).
 *
 * Bring-up scope, BSP-only by decision D5: a static TCB table, 16 KiB
 * kmalloc32'd kernel stacks, preemption from the PIT tick, and the
 * same boot self-test contract as the x86_64 scheduler ([sched] PASS).
 * Wait queues and precise join/reap semantics port from the shared
 * kernel in I5; the hlt-poll wait below is the bring-up stand-in and
 * says so.
 */

#ifndef AURALITE_ARCH_I386_THREAD32_H
#define AURALITE_ARCH_I386_THREAD32_H

#include <stdint.h>

#define THREAD32_MAX        8
#define THREAD32_KSTACK     (16u * 1024u)

#define THREAD32_STATE_FREE  0
#define THREAD32_STATE_READY 1
#define THREAD32_STATE_DONE  2

typedef void (*thread32_fn)(void *arg);

/* Adopt the boot context as thread 0 and start the PIT-driven
 * preemption path (irq32_dispatch calls sched32_maybe_preempt). */
void sched32_init(void);

/* -1 on no slot / no stack; tid otherwise. */
int  thread32_create(const char *name, thread32_fn fn, void *arg);

void thread32_exit(void) __attribute__((noreturn));
void sched32_yield(void);

/* Called by the PIT handler every tick (sets need_resched). */
void sched32_tick(void);
/* Called by irq32_dispatch after EOI; switches if need_resched. */
void sched32_maybe_preempt(void);

/* THREAD32_STATE_*; -1 on a bad tid. */
int  thread32_state(int tid);
int  thread32_current_tid(void);
const char *thread32_name(int tid);

/* I7: dedicated Ring 3 trap stack for the current thread (0 = disarm,
 * falling back to kstack_top).  See the measured lesson in thread32.c. */
void thread32_set_esp0(uint32_t esp0);
uint32_t thread32_current_esp0(void);

#endif /* AURALITE_ARCH_I386_THREAD32_H */

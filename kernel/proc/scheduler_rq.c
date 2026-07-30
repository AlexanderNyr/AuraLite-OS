#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/arch/x86_64/smp.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/kprintf.h"
#include <stdint.h>

/* External access to all CPU local structures for load balancing. */
extern struct cpu_local bsp_cpu_local;
extern struct cpu_local ap_cpu_locals[32];

static inline struct cpu_local* get_cpu_by_id(uint32_t id) {
    return (id == 0) ? &bsp_cpu_local : &ap_cpu_locals[id];
}

/* Helper to find the least loaded CPU.
 *
 * Scoped to smp_get_schedulable_cpu_count(), which since SMP step 3.2 is
 * every online CPU: each AP is online with its own run queue AND its own
 * timer tick driving schedule(), so threads placed on an AP queue actually
 * execute there (the step-2/3.1 concern that a thread could strand on an
 * AP queue forever no longer applies). */
static struct cpu_local* find_least_loaded_cpu(void) {
    uint32_t cpu_count = smp_get_schedulable_cpu_count();
    int best_id = 0;
    uint32_t min_len = get_cpu_local()->rq_len;

    for (uint32_t i = 1; i < cpu_count; i++) {
        struct cpu_local *cpu = get_cpu_by_id(i);
        if (cpu->rq_len < min_len) {
            min_len = cpu->rq_len;
            best_id = i;
        }
    }
    return get_cpu_by_id(best_id);
}

/* NOTE: callers must already hold the thread's queue-membership claim
 * (tcb.on_queue via __sync_lock_test_and_set) so that a thread can never
 * be enqueued twice concurrently -- see thread.h/scheduler.c (SMP 3.2). */
void sched_add_thread(tcb_t *tcb) {
    if (!tcb) return;
    struct cpu_local *target = find_least_loaded_cpu();
    uint64_t flags = spinlock_acquire_irqsave(&target->rq_lock);
    tcb->next = NULL;
    if (target->rq_tail) {
        target->rq_tail->next = tcb;
    } else {
        target->rq_head = tcb;
    }
    target->rq_tail = tcb;
    target->rq_len++;
    spinlock_release_irqrestore(&target->rq_lock, flags);
}

tcb_t *sched_steal_work(void) {
    struct cpu_local *me = get_cpu_local();
    /* Same scoping as find_least_loaded_cpu(): any online CPU may hold work
     * in its queue, so a starving CPU round-robins across all of them. */
    uint32_t cpu_count = smp_get_schedulable_cpu_count();
    int my_id = (int)me->cpu_id;

    for (uint32_t i = 1; i < cpu_count; i++) {
        int victim_id = (my_id + i) % cpu_count;
        struct cpu_local *victim = get_cpu_by_id(victim_id);
        if (victim->rq_len == 0) continue;

        uint64_t flags = spinlock_acquire_irqsave(&victim->rq_lock);
        tcb_t *stolen = victim->rq_head;
        if (stolen) {
            victim->rq_head = stolen->next;
            if (!victim->rq_head) victim->rq_tail = NULL;
            victim->rq_len--;
            stolen->next = NULL;
            /* Dequeuing releases the queue-membership claim (tcb.on_queue). */
            __sync_lock_release(&stolen->on_queue);
            me->steal_count++;
        }
        spinlock_release_irqrestore(&victim->rq_lock, flags);
        if (stolen) return stolen;
    }
    return NULL;
}

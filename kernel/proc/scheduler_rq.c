#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/kprintf.h"
#include <stdint.h>

/* External access to all CPU local structures for load balancing. */
extern struct cpu_local bsp_cpu_local;
extern struct cpu_local ap_cpu_locals[32];

static inline struct cpu_local* get_cpu_by_id(uint32_t id) {
    return (id == 0) ? &bsp_cpu_local : &ap_cpu_locals[id];
}

/* Helper to find the least loaded CPU. */
static struct cpu_local* find_least_loaded_cpu(void) {
    /* We assume max 4 CPUs for this logic, but use the detected cpu_count. */
    extern int cpu_count; 
    int best_id = 0;
    uint32_t min_len = get_cpu_local()->rq_len;

    for (int i = 1; i < cpu_count; i++) {
        struct cpu_local *cpu = get_cpu_by_id(i);
        if (cpu->rq_len < min_len) {
            min_len = cpu->rq_len;
            best_id = i;
        }
    }
    return get_cpu_by_id(best_id);
}

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

static tcb_t *sched_steal_work(void) {
    struct cpu_local *me = get_cpu_local();
    extern int cpu_count;
    int my_id = (int)me->cpu_id;

    for (int i = 1; i < cpu_count; i++) {
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
            me->steal_count++;
        }
        spinlock_release_irqrestore(&victim->rq_lock, flags);
        if (stolen) return stolen;
    }
    return NULL;
}

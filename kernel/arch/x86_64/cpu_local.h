#ifndef AURALITE_ARCH_X86_64_CPU_LOCAL_H
#define AURALITE_ARCH_X86_64_CPU_LOCAL_H

#include <stdint.h>
#include "kernel/lib/spinlock.h"

struct tcb;

struct cpu_local {
    struct cpu_local *self;       /* offset 0: GS:0 self-pointer */
    uint64_t          cpu_id;
    struct tcb       *current;
    struct tcb       *idle;

    /* Per-CPU run queue */
    spinlock_t        rq_lock;
    struct tcb       *rq_head;
    struct tcb       *rq_tail;
    uint32_t          rq_len;
    uint64_t          steal_count;
};

void cpu_local_init(uint64_t cpu_id);

extern int cpu_local_ready;
extern struct cpu_local bsp_cpu_local;
extern struct cpu_local ap_cpu_locals[32];

static inline struct cpu_local *get_cpu_local(void) {
    struct cpu_local *p;
    __asm__ volatile ("mov %%gs:0, %0" : "=r"(p));
    return p;
}

#endif /* AURALITE_ARCH_X86_64_CPU_LOCAL_H */
#ifndef AURALITE_ARCH_X86_64_CPU_LOCAL_H
#define AURALITE_ARCH_X86_64_CPU_LOCAL_H

#include <stdint.h>

struct tcb;

struct cpu_local {
    struct cpu_local *self;       /* offset 0: GS:0 self-pointer */
    uint64_t          cpu_id;
    struct tcb       *current;
    struct tcb       *idle;
};

void cpu_local_init(uint64_t cpu_id);

static inline struct cpu_local *get_cpu_local(void) {
    struct cpu_local *p;
    __asm__ volatile ("mov %%gs:0, %0" : "=r"(p));
    return p;
}

#endif /* AURALITE_ARCH_X86_64_CPU_LOCAL_H */
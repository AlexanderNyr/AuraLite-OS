/* kernel/arch/x86_64/cpu_local.c — CPU-local data structure management (H8) */

#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/lib/string.h"
#include "kernel/lib/spinlock.h"
#include <stdint.h>

#define MSR_GS_BASE 0xC0000101
/* W32A-3: the swapgs shadow.  Parked at 0 on every CPU (BSP and APs):
 * no thread has run yet, so no user GS exists to shadow. */
#define MSR_KERNEL_GS_BASE 0xC0000102

int cpu_local_ready = 0;
struct cpu_local bsp_cpu_local;
struct cpu_local ap_cpu_locals[32];

static inline void wrmsr_gs(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

void cpu_local_init(uint64_t cpu_id) {
    struct cpu_local *c = (cpu_id == 0) ? &bsp_cpu_local : &ap_cpu_locals[cpu_id];
    memset(c, 0, sizeof(*c));
    c->self = c;
    c->cpu_id = cpu_id;
    spinlock_init(&c->rq_lock);
    wrmsr_gs(MSR_GS_BASE, (uint64_t)(uintptr_t)c);
    wrmsr_gs(MSR_KERNEL_GS_BASE, 0);
    if (cpu_id == 0) cpu_local_ready = 1;
}

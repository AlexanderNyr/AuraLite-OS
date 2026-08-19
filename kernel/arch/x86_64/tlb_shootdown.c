/* kernel/arch/x86_64/tlb_shootdown.c — TLB Shootdown IPI handler */

#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/lapic.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/perfstat.h"

void ipi_tlb_shootdown_handler(void) {
    /* Full TLB flush: reload CR3.
     * OPT_PLAN.md Fact 4 / phase O5: no address travels with this IPI, so
     * the handler cannot invlpg — every shootdown costs this CPU its whole
     * TLB.  The counter is O5's before/after evidence. */
    perfstat_add(PERF_TLB_SHOOTDOWNS_FULL, 1);
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
    lapic_eoi();
}

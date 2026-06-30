/* kernel/arch/x86_64/tlb_shootdown.c — TLB Shootdown IPI handler */

#include "kernel/arch/x86_64/cpu.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/lapic.h"
#include "kernel/lib/kprintf.h"

void ipi_tlb_shootdown_handler(void) {
    /* Full TLB flush: reload CR3. */
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
    lapic_eoi();
}

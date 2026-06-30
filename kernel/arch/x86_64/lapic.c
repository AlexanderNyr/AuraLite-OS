/* kernel/arch/x86_64/lapic.c — LAPIC management (H8) */

#include "kernel/arch/x86_64/lapic.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/limine_requests.h"
#include "limine/limine.h"
#include "kernel/lib/kprintf.h"
#include <stdint.h>

#define LAPIC_ID         0x0020
#define LAPIC_EOI        0x00B0
#define LAPIC_SIVR       0x00F0
#define LAPIC_TIMER      0x0320
#define LAPIC_TIMER_INIT 0x0380
#define LAPIC_TIMER_CUR  0x0390
#define LAPIC_TIMER_DIV  0x03E0

#define LAPIC_SIVR_ENABLE 0x100

static int lapic_mapped = 0;

void lapic_enable(void) {
    uint64_t hhdm = limine_get_hhdm_offset();
    if (!hhdm) return;
    uint64_t apic_base_msr;
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(0x1B));
    apic_base_msr = ((uint64_t)high << 32) | low;
    uint64_t lapic_phys = apic_base_msr & 0xFFFFF000ULL;
    if (!lapic_phys) lapic_phys = 0xFEE00000ULL;
    uint64_t virt = hhdm + lapic_phys;
    if (!lapic_mapped) {
        paging_map(virt, lapic_phys, PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_NO_EXEC);
        lapic_mapped = 1;
    }
    volatile uint32_t *lapic = (volatile uint32_t *)(uintptr_t)virt;
    
    /* Set Spurious Interrupt Vector Register to enable APIC (bit 8) + spurious vector 0xFF */
    lapic[LAPIC_SIVR / 4] = LAPIC_SIVR_ENABLE | 0xFF;
}

void lapic_eoi(void) {
    uint64_t hhdm = limine_get_hhdm_offset();
    if (!hhdm || !lapic_mapped) return;
    uint64_t apic_base_msr;
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(0x1B));
    apic_base_msr = ((uint64_t)high << 32) | low;
    uint64_t lapic_phys = apic_base_msr & 0xFFFFF000ULL;
    if (!lapic_phys) lapic_phys = 0xFEE00000ULL;
    volatile uint32_t *lapic = (volatile uint32_t *)(uintptr_t)(hhdm + lapic_phys);
    lapic[LAPIC_EOI / 4] = 0;
}

void lapic_timer_start(uint32_t hz) {
    uint64_t hhdm = limine_get_hhdm_offset();
    if (!hhdm || hz == 0 || !lapic_mapped) return;
    uint64_t apic_base_msr;
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(0x1B));
    apic_base_msr = ((uint64_t)high << 32) | low;
    uint64_t lapic_phys = apic_base_msr & 0xFFFFF000ULL;
    if (!lapic_phys) lapic_phys = 0xFEE00000ULL;
    volatile uint32_t *lapic = (volatile uint32_t *)(uintptr_t)(hhdm + lapic_phys);

    /* Divide configuration = 16 */
    lapic[LAPIC_TIMER_DIV / 4] = 0x03;
    
    /* Timer mode = Periodic (bit 17: 0x20000), vector = 32 (IRQ 0/timer vector) */
    lapic[LAPIC_TIMER / 4] = 0x20000 | 32;
    
    /* Assume a base APIC bus frequency of ~100 MHz for timer init count */
    uint32_t ticks = 100000000 / 16 / hz;
    lapic[LAPIC_TIMER_INIT / 4] = ticks;
}

void lapic_send_ipi_all_excluding_self(uint8_t vector) {
    uint64_t hhdm = limine_get_hhdm_offset();
    if (!hhdm) return;
    uint64_t apic_base_msr;
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(0x1B));
    apic_base_msr = ((uint64_t)high << 32) | low;
    uint64_t lapic_phys = apic_base_msr & 0xFFFFF000ULL;
    if (!lapic_phys) lapic_phys = 0xFEE00000ULL;
    volatile uint32_t *lapic = (volatile uint32_t *)(uintptr_t)(hhdm + lapic_phys);

    /* ICR High: Destination shorthand = All excluding self (3) */
    lapic[0x310 / 4] = 0; 
    /* ICR Low: Delivery Mode = Fixed (0), Vector = vector */
    lapic[0x300 / 4] = (3u << 18) | (0u << 14) | vector;

    /* Wait for delivery */
    while (lapic[0x300 / 4] & (1u << 12)) {}
}

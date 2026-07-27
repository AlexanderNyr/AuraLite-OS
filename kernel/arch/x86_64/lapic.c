/* kernel/arch/x86_64/lapic.c — LAPIC management (H8) */

#include "kernel/arch/x86_64/lapic.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/boot_info.h"
#include "kernel/lib/kprintf.h"
#include <stdint.h>

#define LAPIC_ID         0x0020
#define LAPIC_EOI        0x00B0
#define LAPIC_SIVR       0x00F0
#define LAPIC_LVT_LINT0  0x0350
#define LAPIC_LVT_LINT1  0x0360
#define LAPIC_TIMER      0x0320
#define LAPIC_TIMER_INIT 0x0380
#define LAPIC_TIMER_CUR  0x0390
#define LAPIC_TIMER_DIV  0x03E0

#define LAPIC_SIVR_ENABLE 0x100

/* LVT delivery-mode field (bits 10:8): ExtINT accepts the 8259's vector on
 * the INTA bus cycle; NMI is fixed non-maskable delivery. Bit 16 is the
 * per-entry Mask bit -- LINT0/LINT1 reset to Mask=1 on every real CPU, so
 * an OS that never explicitly clears it leaves the "virtual wire" path
 * permanently disconnected even after the 8259 and IMCR are configured
 * correctly. */
#define LAPIC_LVT_DM_EXTINT (7u << 8)
#define LAPIC_LVT_DM_NMI    (4u << 8)
#define LAPIC_LVT_MASKED    (1u << 16)

static int lapic_mapped = 0;

void lapic_enable(void) {
    uint64_t hhdm = boot_get_hhdm_offset();
    if (!hhdm) return;
    uint64_t apic_base_msr;
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(0x1B));
    apic_base_msr = ((uint64_t)high << 32) | low;
    uint64_t lapic_phys = apic_base_msr & 0xFFFFF000ULL;
    if (!lapic_phys) lapic_phys = 0xFEE00000ULL;
    uint64_t virt = hhdm + lapic_phys;
    if (!lapic_mapped) {
        paging_map(virt, lapic_phys, PAGE_FLAGS_MMIO);
        lapic_mapped = 1;
    }
    volatile uint32_t *lapic = (volatile uint32_t *)(uintptr_t)virt;
    
    /* Set Spurious Interrupt Vector Register to enable APIC (bit 8) + spurious vector 0xFF */
    lapic[LAPIC_SIVR / 4] = LAPIC_SIVR_ENABLE | 0xFF;

    /*
     * Legacy PIC "virtual wire" routing: only the Bootstrap Processor's
     * LINT0/LINT1 pins are wired to the 8259 cascade output / NMI on
     * MP-compliant systems, so this only needs to run once, on the BSP.
     *
     * On real hardware LINT0/LINT1 reset to masked, and many UEFI
     * implementations additionally leave IMCR routed to the (unused) I/O
     * APIC. Without explicitly unmasking LINT0 as ExtINT here, IRQ0 (PIT),
     * IRQ1 (keyboard) and IRQ12 (mouse) are configured and unmasked by the
     * 8259 driver but their interrupts never reach the CPU core: the timer
     * tick counter (and everything derived from it -- the GUI clock, the
     * scheduler, nanosleep/alarm) stays frozen at zero, and keyboard/mouse
     * input never arrives. QEMU boots with LINT0 already usable as ExtINT,
     * which is why this only shows up on physical machines. See
     * kernel/arch/x86_64/irq.c (IMCR) for the matching 8259-side fix. */
    struct cpu_local *me = get_cpu_local();
    if (!me || me->cpu_id == 0) {
        lapic[LAPIC_LVT_LINT0 / 4] = LAPIC_LVT_DM_EXTINT;   /* unmasked ExtINT */
        lapic[LAPIC_LVT_LINT1 / 4] = LAPIC_LVT_DM_NMI;      /* unmasked NMI    */
    } else {
        /* APs must never accept the shared 8259 ExtINT line. */
        lapic[LAPIC_LVT_LINT0 / 4] = LAPIC_LVT_MASKED;
        lapic[LAPIC_LVT_LINT1 / 4] = LAPIC_LVT_DM_NMI;
    }
}

void lapic_eoi(void) {
    uint64_t hhdm = boot_get_hhdm_offset();
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
    uint64_t hhdm = boot_get_hhdm_offset();
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
    uint64_t hhdm = boot_get_hhdm_offset();
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

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

/* Defined further below (shared by the timer code here and the IPI
 * helpers): returns the LAPIC's HHDM-mapped MMIO base, or NULL. */
static volatile uint32_t *lapic_mmio(void);

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

void lapic_mask_lint0(void) {
    volatile uint32_t *lapic = lapic_mmio();
    if (!lapic || !lapic_mapped) return;
    /* Set only the Mask bit in the LINT0 LVT entry: delivery mode and trigger
     * become irrelevant once masked.  This disconnects the 8259 ExtINT "virtual
     * wire" on the BSP once the I/O APIC is delivering IRQs directly. */
    lapic[LAPIC_LVT_LINT0 / 4] = LAPIC_LVT_MASKED;
}

/* ---- Local APIC timer (per-CPU scheduler tick source, SMP step 3.2) ----
 *
 * The legacy PIT reaches only the BSP (IRQ0 via LINT0/ExtINT -- nothing
 * routes it to APs), so each AP needs its own periodic tick to run
 * sched_tick() locally.  Every LAPIC has such a timer, driven off the APIC
 * bus clock -- whose frequency is NOT architecturally known (it varies per
 * machine and per emulator), which is why the old lapic_timer_start() with
 * its hardcoded "assume 100 MHz" guess could never be used.  Instead the
 * frequency is MEASURED once on the BSP against the PIT-based wall clock
 * (lapic_timer_calibrate_begin/end, called from smp_init() with a known
 * busy-wait between them), and each AP then programs its own timer from
 * that measurement (ap_entry() -> lapic_timer_start_periodic()). */

/* Divide Configuration Register value 0x3 == divide the bus clock by 16
 * (Intel SDM Vol.3 s.10.5.4, DCR encoding 0011b = /16).  With the measured
 * bus frequency kept in BUS-HZ units below, all programming maths happens
 * on the divided counter. */
#define LAPIC_TIMER_DCR_DIV16 0x3

/* Measured APIC bus frequency in Hz (counter ticks per second BEFORE the
 * /16 divider is applied), or 0 until lapic_timer_calibrate_end() ran. */
static uint32_t lapic_bus_hz = 0;

void lapic_timer_calibrate_begin(void) {
    volatile uint32_t *lapic = lapic_mmio();
    if (!lapic || !lapic_mapped) return;
    lapic[LAPIC_TIMER_DIV / 4] = LAPIC_TIMER_DCR_DIV16;
    /* Mask the LVT timer entry (one-shot mode, no interrupt delivery): we
     * only poll the current-count register, no IRQ must fire. */
    lapic[LAPIC_TIMER / 4] = LAPIC_LVT_MASKED;
    /* Largest possible start value for maximum measurement range. */
    lapic[LAPIC_TIMER_INIT / 4] = 0xFFFFFFFFu;
}

void lapic_timer_calibrate_end(uint32_t elapsed_us) {
    volatile uint32_t *lapic = lapic_mmio();
    if (!lapic || !lapic_mapped || elapsed_us == 0) return;
    uint32_t elapsed_ticks = 0xFFFFFFFFu - lapic[LAPIC_TIMER_CUR / 4];
    /* Keep the timer off until someone starts it for real. */
    lapic[LAPIC_TIMER / 4] = LAPIC_LVT_MASKED;
    lapic[LAPIC_TIMER_INIT / 4] = 0;

    /* elapsed_ticks counts BUS_HZ / 16 ticks over elapsed_us microseconds:
     *   bus_hz = elapsed_ticks * 16 * (1e6 / elapsed_us).  64-bit maths;
     * the multiplicative form is exact for the smp_udelay(20000) caller
     * (factor 800).  Reject implausible results (< 8 MHz) as a failed
     * measurement: lapic_timer_start_periodic() then refuses to run, which
     * keeps the system in its old safe BSP-only-tick mode. */
    uint64_t hz = (uint64_t)elapsed_ticks * 16ULL * 1000000ULL / elapsed_us;
    if (hz < 8000000ULL || hz > 0xFFFFFFFFULL) {
        kprintf("[smp] LAPIC timer calibration failed (%llu Hz); "
                "APs will NOT take scheduler ticks\n", (unsigned long long)hz);
        lapic_bus_hz = 0;
        return;
    }
    lapic_bus_hz = (uint32_t)hz;
    kprintf("[smp] LAPIC bus frequency: %u Hz (%u MHz)\n",
            lapic_bus_hz, lapic_bus_hz / 1000000u);
}

uint32_t lapic_timer_get_bus_hz(void) {
    return lapic_bus_hz;
}

void lapic_timer_start_periodic(uint32_t hz) {
    volatile uint32_t *lapic = lapic_mmio();
    if (!lapic || !lapic_mapped || hz == 0 || lapic_bus_hz == 0) return;
    lapic[LAPIC_TIMER_DIV / 4] = LAPIC_TIMER_DCR_DIV16;
    /* Periodic mode (bit 17), unmasked, vector 32 -- the same vector the
     * legacy PIT uses on the BSP, so irq_dispatch() routes AP ticks into the
     * same timer handler; that handler splits BSP wall-clock duties from
     * per-CPU scheduling by cpu_id (see drivers/timer/pit.c).  AP ticks get
     * their LAPIC EOI from irq_dispatch(). */
    lapic[LAPIC_TIMER / 4] = 0x20000u | 32u;
    uint32_t ticks = lapic_bus_hz / 16u / hz;
    if (ticks == 0) ticks = 1;
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

/* Shared helper: return the LAPIC's HHDM-mapped MMIO base, or NULL if the
 * HHDM isn't ready.  Every ICR-writing function below needs this, and
 * lapic_enable() has already mapped the page by the time any of them can
 * possibly be called. */
static volatile uint32_t *lapic_mmio(void) {
    uint64_t hhdm = boot_get_hhdm_offset();
    if (!hhdm) return 0;
    uint64_t apic_base_msr;
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(0x1B));
    apic_base_msr = ((uint64_t)high << 32) | low;
    uint64_t lapic_phys = apic_base_msr & 0xFFFFF000ULL;
    if (!lapic_phys) lapic_phys = 0xFEE00000ULL;
    return (volatile uint32_t *)(uintptr_t)(hhdm + lapic_phys);
}

uint32_t lapic_read_id(void) {
    volatile uint32_t *lapic = lapic_mmio();
    if (!lapic) return 0;
    /* xAPIC LAPIC ID register (offset 0x20): the ID occupies bits 31:24. */
    return lapic[LAPIC_ID / 4] >> 24;
}

/* Write the ICR (Interrupt Command Register, a 64-bit register split across
 * two 32-bit MMIO words) targeting a specific destination APIC ID, then wait
 * for the LAPIC to report the send completed (Delivery Status, ICR-low bit
 * 12) before returning.  This ordering -- high word (destination) before low
 * word (the write that actually triggers the send) -- is mandated by the
 * Intel SDM Vol.3 s.10.6.1. */
static void lapic_send_icr(uint32_t apic_id, uint32_t icr_low_bits) {
    volatile uint32_t *lapic = lapic_mmio();
    if (!lapic) return;
    lapic[0x310 / 4] = apic_id << 24;   /* ICR High: destination APIC ID */
    lapic[0x300 / 4] = icr_low_bits;    /* ICR Low: triggers the send */
    while (lapic[0x300 / 4] & (1u << 12)) { /* wait for Delivery Status */ }
}

void lapic_send_init_ipi(uint32_t apic_id) {
    /* Delivery Mode = INIT (101b, bits 10:8), Level = Assert (bit 14),
     * Trigger Mode = Level (bit 15).  Vector is ignored/must be 0 for INIT. */
    lapic_send_icr(apic_id, (5u << 8) | (1u << 14) | (1u << 15));
}

void lapic_send_init_deassert(uint32_t apic_id) {
    /* INIT, Level = Deassert (bit 14 clear), Trigger Mode = Level. */
    lapic_send_icr(apic_id, (5u << 8) | (1u << 15));
}

void lapic_send_sipi(uint32_t apic_id, uint8_t vector) {
    /* Delivery Mode = Startup (110b, bits 10:8); vector = target real-mode
     * start address >> 12. */
    lapic_send_icr(apic_id, (6u << 8) | vector);
}

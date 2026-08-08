/* kernel/arch/x86_64/ioapic.c -- I/O APIC driver (MATURITY_PLAN.md phase M2).
 *
 * Routes the 16 legacy ISA IRQs through the I/O APIC instead of the 8259 PIC,
 * so the BSP stops receiving them over the PIC/"virtual-wire" (LINT0 ExtINT)
 * path and the 8259 can be fully masked.  This is the prerequisite for SMP-
 * safe, level-aware interrupt delivery and for leaving PIC mode behind on
 * real hardware.
 *
 * This is the QEMU-primary increment (plan decision D5): the I/O APIC is
 * assumed at the PC-standard base 0xFEC00000, and the one legacy remap that
 * matters -- ISA IRQ0 (the PIT) is wired to GSI 2 -- is hard-coded.  Real
 * machines publish the IOAPIC base and any further Interrupt Source Overrides
 * in the ACPI MADT; parsing that in the bootloader and feeding it here is the
 * documented real-hardware follow-up.
 *
 * Safety: if no I/O APIC is present (the version register reads as
 * all-zero/all-one), this returns non-zero WITHOUT touching the PIC, so the
 * established PIC virtual-wire path keeps working unchanged. */

#include <stdint.h>
#include "kernel/arch/x86_64/ioapic.h"
#include "kernel/arch/x86_64/lapic.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/boot_info.h"
#include "kernel/lib/kprintf.h"

/* I/O APIC register interface: an 8-bit IOREGSEL index register at +0x00
 * selects which 32-bit register the IOWIN window at +0x10 reads/writes. */
#define IOAPIC_PHYS       0xFEC00000ULL
#define IOAPIC_REGSEL     0x00
#define IOAPIC_WIN        0x10

#define IOAPIC_ID         0x00   /* register index: ID            */
#define IOAPIC_VER        0x01   /* register index: version       */
#define IOAPIC_REDTBL     0x10   /* first redirection-table reg   */

/* Becomes 1 once the BSP has switched to I/O-APIC delivery; irq_dispatch()
 * checks it to skip the (now-meaningless) 8259 EOI. */
volatile int apic_irq_mode = 0;

static volatile uint32_t *ioapic_win;   /* HHDM-mapped register window */
static int                ioapic_count; /* number of redirection entries */

static inline uint32_t io_read(uint8_t reg) {
    ioapic_win[IOAPIC_REGSEL / 4] = reg;
    return ioapic_win[IOAPIC_WIN / 4];
}

static inline void io_write(uint8_t reg, uint32_t val) {
    ioapic_win[IOAPIC_REGSEL / 4] = reg;
    ioapic_win[IOAPIC_WIN / 4] = val;
}

/* A redirection-table entry is a 64-bit value split across two 32-bit
 * registers: low half at 0x10+2*gsi, high half at 0x11+2*gsi.  Write the high
 * half (destination) first so that, the instant the low half unmasks and arms
 * the vector, the destination field already holds the BSP APIC ID -- no
 * transient window can deliver to APIC ID 0 by accident. */
static void io_set_redir(int gsi, uint64_t entry) {
    io_write((uint8_t)(IOAPIC_REDTBL + 2 * gsi + 1), (uint32_t)(entry >> 32));
    io_write((uint8_t)(IOAPIC_REDTBL + 2 * gsi),     (uint32_t)(entry));
}

/* Build a redirection entry for an edge-triggered, active-high, fixed-delivery
 * ISA interrupt aimed (physical destination mode) at the given APIC ID. */
static uint64_t io_make_isa_entry(int vector, uint32_t dest_apic_id) {
    return ((uint64_t)(vector & 0xFF))            /* [7:0]   vector             */
         | (0ULL << 8)                            /* [10:8]  fixed delivery     */
         | (0ULL << 11)                           /* [11]    physical dest mode */
         | (0ULL << 13)                           /* [13]    active high        */
         | (0ULL << 15)                           /* [15]    edge triggered     */
         | (0ULL << 16)                           /* [16]    unmasked           */
         | (((uint64_t)(dest_apic_id & 0xFF)) << 56); /* [63:56] dest APIC ID */
}

int ioapic_init(void) {
    uint64_t hhdm = boot_get_hhdm_offset();
    if (!hhdm) {
        kprintf("[ioapic] no HHDM offset; staying on PIC\n");
        return -1;
    }

    /* Map the I/O APIC MMIO window into the HHDM region. */
    uint64_t virt = hhdm + IOAPIC_PHYS;
    paging_map(virt, IOAPIC_PHYS, PAGE_FLAGS_MMIO);
    ioapic_win = (volatile uint32_t *)(uintptr_t)virt;

    /* Probe: the version register's low byte is the IOAPIC version (0x10/0x11
     * on the 82093AA and QEMU, 0x20/0x21 on newer parts) and bits [23:16]
     * hold max-redirection-entry, so entry count = that + 1.  An absent IOAPIC
     * reads back all-zero or all-one; either way this guard rejects it and
     * leaves the PIC path intact. */
    uint32_t ver   = io_read(IOAPIC_VER);
    uint32_t vmax  = (ver >> 16) & 0xFF;
    uint32_t vlow  = ver & 0xFF;
    ioapic_count   = (int)vmax + 1;
    if (vlow == 0x00 || vlow == 0xFF || ioapic_count <= 0 || ioapic_count > 64) {
        kprintf("[ioapic] no I/O APIC at 0x%llx (ver=0x%x); staying on PIC\n",
                (unsigned long long)IOAPIC_PHYS, ver);
        return -1;
    }

    /* Step 1: mask every redirection entry first so nothing can fire while we
     * reprogram the legacy routing. */
    for (int i = 0; i < ioapic_count; i++) {
        io_set_redir(i, 1ULL << 16);              /* masked, everything else 0 */
    }

    /* Step 2: program the 16 legacy ISA IRQs.  ISA IRQ0 (the PIT) is wired to
     * GSI 2 on every PC-compatible system (the single mandatory ACPI Interrupt
     * Source Override); every other ISA IRQ is identity-mapped to its GSI.
     * Vectors stay at 32 + irq -- exactly the PIC remap offsets -- so every
     * already-registered handler keeps firing on the same vector.  Destination
     * is the BSP APIC ID in physical mode.
     *
     * IRQ2 must be SKIPPED: it is the 8259 cascade line (no real device sits
     * on it), and in the identity map IRQ2 would land on GSI2 -- the very pin
     * the PIT override occupies.  Programming it would overwrite the PIT's
     * redirection entry with IRQ2's vector, sending PIT ticks to vector 34 and
     * freezing the timer (and the scheduler) after zero ticks. */
    uint32_t bsp = lapic_read_id();
    for (int irq = 0; irq < 16; irq++) {
        if (irq == 2) continue;                   /* cascade; GSI2 is the PIT  */
        int gsi = (irq == 0) ? 2 : irq;           /* PIT: IRQ0 -> GSI2         */
        if (gsi >= ioapic_count) continue;
        io_set_redir(gsi, io_make_isa_entry(32 + irq, bsp));
    }

    /* Step 3: switch the BSP off the PIC.  Mask the 8259 completely and flip
     * the IMCR to APIC delivery so the (now silent) 8259 is fully decoupled,
     * then mask LINT0 -- it carried the 8259's ExtINT vectors and the IOAPIC
     * delivers directly now.  LINT1 (NMI) is untouched. */
    pic_disable_for_apic();
    lapic_mask_lint0();

    apic_irq_mode = 1;
    kprintf("[ioapic] I/O APIC @0x%llx ver 0x%x, %d redirection entries; "
            "BSP on APIC IRQs (PIT@GSI2, kbd@GSI1)\n",
            (unsigned long long)IOAPIC_PHYS, vlow, ioapic_count);
    return 0;
}

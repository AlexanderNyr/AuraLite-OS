#ifndef AURALITE_ARCH_X86_64_IOAPIC_H
#define AURALITE_ARCH_X86_64_IOAPIC_H

#include <stdint.h>

/*
 * ioapic.h -- I/O APIC driver (MATURITY_PLAN.md phase M2).
 *
 * Brings up the I/O APIC and routes the legacy ISA IRQs (PIT, keyboard, mouse,
 * IDE, ...) through it instead of the 8259 PIC, so the BSP stops relying on the
 * PIC/"virtual wire" (LINT0 ExtINT) path and interrupts are delivered directly
 * to the Local APIC.
 *
 * Scope of this increment (per the plan's D5, QEMU is the primary target): the
 * I/O APIC is assumed at the PC-standard base 0xFEC00000 with the single
 * standard Interrupt Source Override (ISA IRQ0 / PIT -> GSI 2).  Real hardware
 * needs the IOAPIC base address and the ISO list read from the ACPI MADT in the
 * bootloader -- that parsing is the documented real-hardware follow-up.
 */

/* Bring up the I/O APIC, route the 16 legacy ISA IRQs to vectors 32..47, mask
 * the 8259 PIC and LINT0, and switch the BSP to APIC interrupt mode.  Returns 0
 * on success; on failure (no IOAPIC at the standard base) leaves the existing
 * PIC virtual-wire path untouched and returns non-zero. */
int  ioapic_init(void);

/* 1 once ioapic_init() succeeded and the BSP receives IRQs via the I/O APIC
 * (irq_dispatch then EOI's the Local APIC only, never the 8259). */
extern volatile int apic_irq_mode;

/* RESIDUE2 T2: point one redirection entry at a chosen APIC ID at runtime
 * (edge/high/fixed, vector as given, unmasked).  Used by the RES-16 wake
 * selftest to aim a device IRQ at a hlt-ed AP and restore the pin after.
 * Returns 0 on success, -1 if the I/O APIC is not up or the GSI is out of
 * range. */
int ioapic_route_gsi(int gsi, int vector, uint32_t dest_apic_id);

#endif /* AURALITE_ARCH_X86_64_IOAPIC_H */

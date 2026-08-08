#ifndef AURALITE_ARCH_X86_64_LAPIC_H
#define AURALITE_ARCH_X86_64_LAPIC_H

#include <stdint.h>

#define IPI_TLB_SHOOTDOWN_VECTOR 0xF0

void lapic_enable(void);
void lapic_eoi(void);
/* Mask LINT0 on the running CPU (write the LVT entry with the Mask bit).  Used
 * by ioapic_init() once the I/O APIC is delivering legacy IRQs directly: the
 * BSP's LINT0 no longer needs to carry the 8259's ExtINT vectors. */
void lapic_mask_lint0(void);
void lapic_send_ipi_all_excluding_self(uint8_t vector);

/* Per-CPU Local APIC timer (SMP step 3.2).  The APIC bus frequency is not
 * architecturally fixed, so smp_init() measures it once on the BSP: call
 * lapic_timer_calibrate_begin(), wait a known wall-clock interval (the
 * PIT-based smp_udelay), then lapic_timer_calibrate_end(elapsed_us).
 * ap_entry() then arms every AP's own periodic tick from that measurement.
 * lapic_timer_start_periodic() is a safe no-op if calibration failed. */
void lapic_timer_calibrate_begin(void);
void lapic_timer_calibrate_end(uint32_t elapsed_us);
uint32_t lapic_timer_get_bus_hz(void);
void lapic_timer_start_periodic(uint32_t hz);

/* This CPU's own Local APIC ID, read from the LAPIC ID register (more
 * reliable than trusting bootloader-supplied values). */
uint32_t lapic_read_id(void);

/* INIT-SIPI-SIPI AP startup sequence (Intel MP spec s.B.4): assert INIT,
 * deassert it, then deliver one or two Startup IPIs whose vector is the
 * 4 KiB page number the target AP begins executing from in real mode. */
void lapic_send_init_ipi(uint32_t apic_id);
void lapic_send_init_deassert(uint32_t apic_id);
void lapic_send_sipi(uint32_t apic_id, uint8_t vector);

#endif /* AURALITE_ARCH_X86_64_LAPIC_H */

#ifndef AURALITE_ARCH_X86_64_LAPIC_H
#define AURALITE_ARCH_X86_64_LAPIC_H

#include <stdint.h>

#define IPI_TLB_SHOOTDOWN_VECTOR 0xF0

void lapic_enable(void);
void lapic_timer_start(uint32_t hz);
void lapic_eoi(void);
void lapic_send_ipi_all_excluding_self(uint8_t vector);

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

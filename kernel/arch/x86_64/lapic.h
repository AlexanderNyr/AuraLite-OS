#ifndef AURALITE_ARCH_X86_64_LAPIC_H
#define AURALITE_ARCH_X86_64_LAPIC_H

#include <stdint.h>

#define IPI_TLB_SHOOTDOWN_VECTOR 0xF0

void lapic_enable(void);
void lapic_timer_start(uint32_t hz);
void lapic_eoi(void);
void lapic_send_ipi_all_excluding_self(uint8_t vector);

#endif /* AURALITE_ARCH_X86_64_LAPIC_H */
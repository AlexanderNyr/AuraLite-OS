#ifndef AURALITE_ARCH_X86_64_LAPIC_H
#define AURALITE_ARCH_X86_64_LAPIC_H

#include <stdint.h>

void lapic_enable(void);
void lapic_timer_start(uint32_t hz);
void lapic_eoi(void);

#endif /* AURALITE_ARCH_X86_64_LAPIC_H */
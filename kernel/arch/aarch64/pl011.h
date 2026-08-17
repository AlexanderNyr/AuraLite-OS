/* kernel/arch/aarch64/pl011.h -- day-0 PL011 TX console (ARM64_PLAN A0). */

#ifndef AURALITE_ARCH_AARCH64_PL011_H
#define AURALITE_ARCH_AARCH64_PL011_H

#include <stdint.h>

void pl011_putc(char c);
void pl011_puts(const char *s);
void pl011_puthex64(uint64_t v);
void pl011_putdec64(uint64_t v);

#endif /* AURALITE_ARCH_AARCH64_PL011_H */

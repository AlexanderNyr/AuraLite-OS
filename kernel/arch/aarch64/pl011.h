/* kernel/arch/aarch64/pl011.h -- the PL011 console (ARM64_PLAN A0 TX;
 * A7: ring TX [AMEND-3] + IRQ RX). */

#ifndef AURALITE_ARCH_AARCH64_PL011_H
#define AURALITE_ARCH_AARCH64_PL011_H

#include <stdint.h>

void pl011_putc(char c);
void pl011_puts(const char *s);
void pl011_puthex64(uint64_t v);
void pl011_putdec64(uint64_t v);

/* A7 [AMEND-3]: switch TX from the A0 synchronous poll to the O3
 * ring core (uart_ring.h).  Call once the GIC serves interrupts;
 * before this, every byte is a synchronous FR.TXFF poll (early boot
 * and panic prints owe nothing to interrupts). */
void pl011_tx_ring_enable(void);

/* Drain the TX ring synchronously (the pre-poweroff flush -- a
 * PSCI SYSTEM_OFF with bytes still ringed would eat the tail of the
 * boot log, and the smoke asserts live in that tail). */
void pl011_tx_flush(void);

/* A7: arm IRQ-driven RX (IMSC.RXIM + receive timeout, INTID from the
 * DTB via A1's normalisation -- SPI 1 -> 33 on the virt board). */
void pl011_rx_init(uint32_t intid);

/* One byte or -1.  Ring-fed once pl011_rx_init has run; the A5c
 * polled read before that. */
int  pl011_try_getc(void);

/* Bytes that arrived through the GIC path (the receipt counter the
 * smoke greps -- a poll-fed session would leave it 0). */
uint64_t pl011_rx_count(void);

#endif /* AURALITE_ARCH_AARCH64_PL011_H */

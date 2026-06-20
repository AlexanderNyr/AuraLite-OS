#ifndef NOVOS_DRIVERS_UART_UART_H
#define NOVOS_DRIVERS_UART_UART_H

#include <stdint.h>

/*
 * 16550-compatible serial UART driver for COM1 (0x3F8).
 * Provides the most reliable early console: it works regardless of video mode
 * and is captured by QEMU with `-serial stdio`.
 */

#define UART_COM1 0x3F8

/* UART register offsets from the base port. */
#define UART_THR  0   /* Transmit Holding Register (write) / Data (read) */
#define UART_IER  1   /* Interrupt Enable Register */
#define UART_FCR  2   /* FIFO Control Register */
#define UART_LCR  3   /* Line Control Register */
#define UART_MCR  4   /* Modem Control Register */
#define UART_LSR  5   /* Line Status Register */
#define UART_DL_LO 0  /* Divisor Latch Low  (when DLAB=1) */
#define UART_DL_HI 1  /* Divisor Latch High (when DLAB=1) */

#define UART_LSR_THRE 0x20   /* Transmit Holding Register Empty */

/* Baud rate = 115200 / divisor. 1 => 115200 baud (the QEMU default). */
#define UART_BAUD_DIVISOR 1

void uart_init(void);
void uart_putchar(char c);
void uart_puts(const char *s);

#endif /* NOVOS_DRIVERS_UART_UART_H */

/* uart.c — 16550 UART (COM1) serial driver. */

#include "kernel/arch/x86_64/portio.h"
#include "drivers/uart/uart.h"

void uart_init(void) {
    const uint16_t base = UART_COM1;

    outb(base + UART_IER, 0x00);           /* disable all interrupts */
    outb(base + UART_LCR, 0x80);           /* enable DLAB (set baud divisor) */
    outb(base + UART_DL_LO, UART_BAUD_DIVISOR & 0xFF);
    outb(base + UART_DL_HI, (UART_BAUD_DIVISOR >> 8) & 0xFF);
    outb(base + UART_LCR, 0x03);           /* 8 bits, no parity, 1 stop; DLAB off */
    outb(base + UART_FCR, 0xC7);           /* enable + clear FIFO, 14-byte threshold */
    outb(base + UART_MCR, 0x0B);           /* RTS/DSR set, OUT2 (IRQs routed) */
}

void uart_putchar(char c) {
    const uint16_t base = UART_COM1;
    /* Busy-wait until the transmit holding register can accept a byte. */
    while ((inb(base + UART_LSR) & UART_LSR_THRE) == 0) {
        /* spin */
    }
    outb(base + UART_THR, (uint8_t)c);
}

void uart_puts(const char *s) {
    while (*s) {
        uart_putchar(*s++);
    }
}

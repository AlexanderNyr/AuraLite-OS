/* kernel/arch/aarch64/pl011.c -- day-0 console: PL011 TX (ARM64_PLAN A0).
 *
 * TX-only, zero initialisation: QEMU's reset state has the UART
 * enabled (measured during fact-finding -- the EL1 stub's banner cost
 * one str per byte).  The one concession to assert-not-assume is the
 * TXFF poll: QEMU's FIFO drains instantly into the chardev, but the
 * flag register is architecture, not QEMU, and polling it costs one
 * load.  RX, IRQs, baud -- A7's problem (interrupt-driven, IMSC/ICR).
 *
 * The base address is the virt board's (plan Fact 3, dumped DTB:
 * pl011@9000000).  A1 replaces this constant with the DTB-discovered
 * reg -- the same hardcode-then-discover shape the riscv64 UART used.
 */

#include <stdint.h>

#include "kernel/arch/aarch64/pl011.h"

#define PL011_BASE 0x09000000UL

#define PL011_DR   0x00u
#define PL011_FR   0x18u
#define PL011_FR_TXFF (1u << 5)

static volatile uint32_t *reg32(uint32_t off)
{
    return (volatile uint32_t *)(PL011_BASE + off);
}

void pl011_putc(char c)
{
    while (*reg32(PL011_FR) & PL011_FR_TXFF)
        ;
    *reg32(PL011_DR) = (uint32_t)(unsigned char)c;
}

void pl011_puts(const char *s)
{
    while (*s)
        pl011_putc(*s++);
}

void pl011_puthex64(uint64_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    pl011_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        pl011_putc(digits[(v >> shift) & 0xF]);
}

void pl011_putdec64(uint64_t v)
{
    char buf[21];
    int  i = 20;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    pl011_puts(&buf[i]);
}

/* kernel/arch/i386/kprintf32.c -- COM1 + minimal printf for the i386
 * bring-up (I386_PLAN I2).  See kprintf32.h for why this is not the
 * shared kernel/lib/kprintf.c yet.
 */

#include <stdarg.h>
#include <stdint.h>

#include "kernel/arch/i386/portio.h"
#include "kernel/arch/i386/kprintf32.h"
#include "kernel/arch/i386/vga32.h"

#define COM1 0x3F8

void uart32_init(void)
{
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);   /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8n1 */
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

/* I7: UART receive side, polled.  IRQ4-driven RX can follow when
 * something needs it; the shell's read path polls from the idle
 * loop's hlt cadence, which at human typing speed loses nothing. */
int uart32_has_byte(void)
{
    return inb(COM1 + 5) & 0x01;
}

uint8_t uart32_read_byte(void)
{
    return inb(COM1 + 0);
}

void kputc32(char c)
{
    if (c == '\n')
        kputc32('\r');
    while (!(inb(COM1 + 5) & 0x20)) { }
    outb(COM1 + 0, (uint8_t)c);
    /* Fan out to the VGA text console once it exists (I7) -- same
     * two-sink shape as the 64-bit kprintf.  VGA handles its own \n,
     * so skip the \r we synthesised for the serial line. */
    if (c != '\r' && vga32_active())
        vga32_putc(c);
}

void kputs32(const char *s)
{
    while (*s)
        kputc32(*s++);
}

static void put_udec(uint32_t v)
{
    char buf[10];
    int i = 0;
    do {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (i--)
        kputc32(buf[i]);
}

static void put_hex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4)
        kputc32(hex[(v >> shift) & 0xF]);
}

static void put_hex8(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    kputc32(hex[(v >> 4) & 0xF]);
    kputc32(hex[v & 0xF]);
}

void kprintf32(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            kputc32(*fmt);
            continue;
        }
        fmt++;
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            kputs32(s ? s : "(null)");
            break;
        }
        case 'c':
            kputc32((char)va_arg(ap, int));
            break;
        case 'd': {
            int32_t v = va_arg(ap, int32_t);
            if (v < 0) {
                kputc32('-');
                v = -v;
            }
            put_udec((uint32_t)v);
            break;
        }
        case 'u':
            put_udec(va_arg(ap, uint32_t));
            break;
        case 'x':
            put_hex(va_arg(ap, uint32_t));
            break;
        case 'b':   /* two hex digits -- MAC bytes etc.  (The %x-only
                     * first cut printed MACs as 00000052:...; a MAC
                     * that needs 51 columns is technically correct
                     * and practically unreadable.) */
            put_hex8(va_arg(ap, uint32_t) & 0xFF);
            break;
        case 'p':
            kputs32("0x");
            put_hex((uint32_t)va_arg(ap, void *));
            break;
        case '%':
            kputc32('%');
            break;
        default:
            kputc32('%');
            kputc32(*fmt);
            break;
        }
    }

    va_end(ap);
}

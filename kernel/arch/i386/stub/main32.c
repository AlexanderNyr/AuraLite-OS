/* kernel/arch/i386/stub/main32.c -- i386 boot stub (I386_PLAN I1).
 *
 * This is deliberately NOT the ported kernel; it is the 32-bit sibling of
 * BL3's "stage2 alive" banner, and it exists for the same reason that
 * banner did: prove the *chain* before growing the *payload*.  Everything
 * it validates -- ELF32 staging, the protected-mode entry, boot_info_t in
 * ESI, the magic check, COM1 by raw port I/O -- is exactly the substrate
 * the real i386 kernel (phase I2) stands on.
 *
 * It shares boot/shared/boot_info.h verbatim: the struct is all
 * fixed-width fields, so the same header compiles to the same layout at
 * both pointer widths.  That is not luck -- gen_boot_offsets.c would
 * catch a drift -- but it is worth asserting at runtime once, which the
 * magic check below does.
 *
 * Freestanding, no libc, no kernel headers beyond the shared hand-off
 * contract.  Everything is static because nothing outside this file may
 * link against a stub that will be deleted in phase I2.
 */

#include <stdint.h>

#include "boot/shared/boot_info.h"

/* ---- COM1 by raw port I/O (mirrors drivers/uart/uart.c, 32-bit asm) ---- */

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void uart_init(void)
{
    outb(COM1 + 1, 0x00);   /* disable interrupts             */
    outb(COM1 + 3, 0x80);   /* DLAB on                        */
    outb(COM1 + 0, 0x01);   /* divisor 1 -> 115200 baud       */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8n1, DLAB off                  */
    outb(COM1 + 2, 0xC7);   /* FIFO on, clear, 14-byte trigger */
    outb(COM1 + 4, 0x0B);   /* DTR | RTS | OUT2               */
}

static void uart_putc(char c)
{
    while (!(inb(COM1 + 5) & 0x20)) { /* THR empty */ }
    outb(COM1 + 0, (uint8_t)c);
}

static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

static void uart_puthex32(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        uart_putc(hex[(v >> shift) & 0xF]);
}

/* ---- Entry -------------------------------------------------------------- */

void kmain32(uint32_t boot_info_phys)
{
    uart_init();

    uart_puts("\n[kernel32] AuraLite i386 stub alive\n");
    uart_puts("[kernel32] protected mode, paging off, booted via BIOS\n");

    /* Paging is off and the segments are flat, so a physical address IS a
     * usable pointer -- the one moment in the i386 bring-up where that
     * sentence is true, which is why the check lives here. */
    const boot_info_t *bi = (const boot_info_t *)boot_info_phys;

    uart_puts("[kernel32] boot_info at ");
    uart_puthex32(boot_info_phys);
    uart_puts("\n");

    if (bi->magic == BOOT_MAGIC) {
        uart_puts("[kernel32] boot_info handoff (ESI) magic OK\n");
        uart_puts("[kernel32] mmap entries: ");
        uart_puthex32(bi->mmap_count);
        uart_puts("\n");
        uart_puts("[kernel32] initrd size: ");
        uart_puthex32((uint32_t)bi->initrd_size);
        uart_puts(" bytes\n");
    } else {
        /* Print both halves: the low dword arriving as high (or vice
         * versa) would point at an offsets bug, not a corruption. */
        uart_puts("[kernel32] boot_info magic BAD: hi=");
        uart_puthex32((uint32_t)(bi->magic >> 32));
        uart_puts(" lo=");
        uart_puthex32((uint32_t)bi->magic);
        uart_puts("\n");
    }

    uart_puts("[kernel32] I1 stub complete; halting (I2 grows this into kmain)\n");

    for (;;)
        __asm__ volatile("hlt");
}

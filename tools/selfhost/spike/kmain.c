/*
 * tools/selfhost/spike/kmain.c -- SELFHOST_PLAN.md SH5a spike kernel main.
 *
 * The SH5 measured question (Fact 2): does TinyCC's x86_64 codegen link at
 * the higher-half address 0xFFFFFFFF80100000?  This file is the smallest
 * kernel that answers it end-to-end: it is compiled by tcc, linked with
 * aulink against kernel.ld, packed into the dual-boot ISO in place of the
 * real kernel, and booted in QEMU -- where it prints a receipt to COM1 and
 * halts.
 *
 * Deliberately dependency-free: no kernel headers, no libc.  It exercises
 * the three codegen features that decide the spike:
 *   - function calls            -> R_X86_64_PLT32 (handled as PC32 by aulink)
 *   - global data reads         -> R_X86_64_PC32 (RIP-relative; representable
 *                                  at the higher half, unlike gcc/clang's
 *                                  32-bit absolute small-model relocations)
 *   - string literals           -> tcc emits `.data.ro` (kernel.ld maps it
 *                                  into the rodata PHDR -- see kernel.ld)
 * plus inline asm for port I/O.
 */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned long long u64;

#define COM1 0x3F8

static inline void outb(u16 port, u8 v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline u8 inb(u16 port) {
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void uart_putc(char c) {
    while (!(inb(COM1 + 5) & 0x20)) /* THR empty */ ;
    outb(COM1, (u8)c);
}
static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}
static void uart_hex(u64 v) {
    static const char d[] = "0123456789abcdef";
    char buf[17];
    buf[16] = 0;
    for (int i = 15; i >= 0; i--) { buf[i] = d[v & 0xF]; v >>= 4; }
    uart_puts(buf);
}

/* A writable global forces .data + a read-through-RIP-relocation; the value
 * is checked against the expected constant on the serial line. */
static volatile u64 spike_marker = 0x5A15A5E1ULL;

void kmain(void *boot_info) {
    (void)boot_info;
    spike_marker += 1;
    uart_puts("\r\n[selfhost] SH5a spike: tcc+aulink kernel booted at the higher half\r\n");
    uart_puts("[selfhost] spike marker = 0x");
    uart_hex(spike_marker);
    uart_puts(" (expected 0x5a15a5e2)\r\n");
    for (;;) __asm__ volatile("hlt");
}

#ifndef NOVOS_ARCH_X86_64_PORTIO_H
#define NOVOS_ARCH_X86_64_PORTIO_H

#include <stdint.h>

/*
 * Port-mapped I/O primitives for x86. These issue the `in`/`out` instructions
 * directly; clang treats them as side-effecting and will not reorder them out.
 */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) {
    /* Jump to port 0x80; a harmless write that costs ~1us, used to throttle
       slow PIO devices after commands. */
    outb(0x80, 0x00);
}

#endif /* NOVOS_ARCH_X86_64_PORTIO_H */

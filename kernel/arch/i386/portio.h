/* kernel/arch/i386/portio.h -- port I/O for the i386 kernel (I386_PLAN I2).
 *
 * Sibling of kernel/arch/x86_64/portio.h.  The instructions are identical
 * at both widths; this copy exists so the i386 tree never includes an
 * x86_64 header by accident (the include path IS the arch boundary until
 * the arch.h forwarding header adoption finishes in I6).
 */

#ifndef AURALITE_ARCH_I386_PORTIO_H
#define AURALITE_ARCH_I386_PORTIO_H

#include <stdint.h>

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

static inline void io_wait(void)
{
    /* Port 0x80 is the POST diagnostic port; a write burns ~1 us and has
     * no side effect.  Same trick irq.c uses on x86_64. */
    outb(0x80, 0);
}

#endif /* AURALITE_ARCH_I386_PORTIO_H */

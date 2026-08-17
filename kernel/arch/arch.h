/* kernel/arch/arch.h -- the arch-forwarding header (I386_PLAN I6;
 * promised in I2, delivered here where its negative control lives).
 *
 * Portable code includes THIS, never kernel/arch/<arch>/... directly.
 * The selector is the compiler's own target macro -- the one thing
 * that cannot drift from what is actually being built:
 *
 *     x86_64 kernel:  clang --target=x86_64-elf  -> __x86_64__
 *     i386 kernel:    clang --target=i686-elf    -> __i386__
 *
 * Adoption is incremental and ratchet-guarded: every direct
 * `#include "kernel/arch/x86_64/..."` in portable code is counted by
 * tools/check_width_sweep.py (ratchet 2), and migrating a file here
 * lowers the count in the same commit.  The negative control for
 * every migration batch: the x86_64 kernel's .text must be
 * byte-identical before and after (checked by
 * tests/unit/test_width_sweep.sh) -- a forwarding header that
 * changes generated code is not forwarding.
 *
 * Only headers with a same-contract sibling on both sides are
 * forwarded.  Headers that are genuinely arch-specific (lapic.h,
 * smp.h, syscall.h with its MSR layout) stay direct includes and are
 * part of ratchet 2's residue until their subsystems port.
 */

#ifndef AURALITE_ARCH_ARCH_H
#define AURALITE_ARCH_ARCH_H

/* Port I/O (the I6 block).  x86-only by nature: riscv64 has no port
 * address space -- device access there is MMIO through the HHDM, and
 * a portable file that needs inb/outb is an x86 driver by definition.
 *
 * The riscv branch declares the functions UNAVAILABLE rather than
 * stubbing them or #erroring the whole header: including arch.h for
 * its irqflags block must stay legal on riscv, but the first USE of
 * a port function is a hard compile error naming the V7 route.  A
 * port write that silently does nothing is how the xHCI-shaped bugs
 * of USB_PLAN lore are born (RISCV_PLAN V6, the fence task). */
#if defined(__x86_64__)
#  include "kernel/arch/x86_64/portio.h"
#elif defined(__i386__)
#  include "kernel/arch/i386/portio.h"
#elif defined(__riscv)
#  include <stdint.h>
#  define AURALITE_NO_PORTIO_MSG \
    "port I/O does not exist on riscv64; this driver is x86-only -- " \
    "virtio-mmio (RISCV_PLAN V7) is the riscv device route"
static inline uint8_t  inb(uint16_t port)
    __attribute__((unavailable(AURALITE_NO_PORTIO_MSG)));
static inline void     outb(uint16_t port, uint8_t val)
    __attribute__((unavailable(AURALITE_NO_PORTIO_MSG)));
static inline uint16_t inw(uint16_t port)
    __attribute__((unavailable(AURALITE_NO_PORTIO_MSG)));
static inline void     outw(uint16_t port, uint16_t val)
    __attribute__((unavailable(AURALITE_NO_PORTIO_MSG)));
static inline uint32_t inl(uint16_t port)
    __attribute__((unavailable(AURALITE_NO_PORTIO_MSG)));
static inline void     outl(uint16_t port, uint32_t val)
    __attribute__((unavailable(AURALITE_NO_PORTIO_MSG)));
#else
#  error "arch.h: no port layer for this target"
#endif

/* Interrupt masking + spin-wait (the V6 block, D6): arch_irq_save /
 * arch_irq_restore / arch_wait_for_interrupt / arch_cpu_relax, one
 * contract, three backends.  Every cli/sti/hlt/pause in portable code
 * migrates onto these -- counted by check_width_sweep.py's ratchet 4,
 * lowered batch by batch. */
#if defined(__x86_64__)
#  include "kernel/arch/x86_64/irqflags.h"
#elif defined(__i386__)
#  include "kernel/arch/i386/irqflags.h"
#elif defined(__riscv)
#  include "kernel/arch/riscv64/irqflags.h"
#endif

#endif /* AURALITE_ARCH_ARCH_H */

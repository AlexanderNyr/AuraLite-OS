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

#if defined(__x86_64__)
#  include "kernel/arch/x86_64/portio.h"
#elif defined(__i386__)
#  include "kernel/arch/i386/portio.h"
#else
#  error "arch.h: no port layer for this target"
#endif

#endif /* AURALITE_ARCH_ARCH_H */

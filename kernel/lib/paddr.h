#ifndef AURALITE_LIB_PADDR_H
#define AURALITE_LIB_PADDR_H

/*
 * paddr_t -- a physical address (I386_PLAN I6, decision D6).
 *
 * Physical addresses are 64-bit on BOTH architectures:
 *
 *   - Device descriptor formats (e1000 rings, AHCI command tables,
 *     xHCI TRBs) carry 64-bit addresses on the wire regardless of the
 *     CPU's pointer width.
 *   - E820 reports regions above 4 GiB even to a 32-bit kernel; an
 *     entry that does not fit in uint32_t must be *skipped*, never
 *     truncated into aliasing low memory (pmm32.c does exactly this).
 *
 * Virtual addresses are uintptr_t and NOTHING else.  The two rules
 * together make the width sweep mechanical: every `(uint64_t)ptr`
 * cast in portable code is either a virtual address (delete the cast,
 * use uintptr_t) or a physical one (spell it paddr_t).  The cast
 * counter in tools/check_width_sweep.py enforces that the total only
 * ever goes down.
 *
 * On x86_64 this is a pure alias of the uint64_t the code already
 * used -- adopting it cannot change a single byte of generated code,
 * which is the property the I6 negative control checks.
 */

#include <stdint.h>

typedef uint64_t paddr_t;

#define PADDR_FMT_HI(p)  ((uint32_t)((p) >> 32))
#define PADDR_FMT_LO(p)  ((uint32_t)(p))

#endif /* AURALITE_LIB_PADDR_H */

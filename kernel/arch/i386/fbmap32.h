/* kernel/arch/i386/fbmap32.h -- map the loader's linear framebuffer into
 * the 32-bit kernel's address space (RESIDUE3 C3 / RES-11).
 *
 * Why this exists at all: the i386 direct map covers phys [0, 896 MiB) at
 * 0xC0000000, and the VBE linear framebuffer QEMU hands out sits at
 * 0xFD000000 (1024x768x32 with the default vgamem) -- far past that
 * horizon.  p2v_32() cannot reach it, so the framebuffer console needs a
 * window of its own.
 *
 * Window choice, measured (net32.c's e1000 window is the one that bit):
 *   [0xC0000000, 0xF8000000)  direct map of phys [0, 896 MiB)
 *   [0xF8000000, 0xFC000000)  kernel heap (64 MiB)
 *   0xFC000000                the vmm self-test probe page (one 4 KiB
 *                             page, mapped and unmapped per test)
 *   [0xFD000000, 0xFD020000)  net32.c's e1000 BAR0 window (32 x 4 KiB)
 *   [0xFE000000, 0xFF800000)  THIS window: 24 MiB of device memory, enough
 *                             for 1920x1080x32 (8 MiB) with room to spare.
 *
 * The first draft of this file used 0xFD000000 -- the same address net32.c
 * had already claimed for the NIC -- and the collision was silent for
 * exactly one boot: paging32_map REFUSES an already-mapped PTE, fb_init won
 * the race, and net32.c ignored the refusal, so the e1000 driver went on to
 * read its registers out of framebuffer pixels (garbage MAC, DHCP never
 * answered) while the screen looked perfect.  Two lessons are encoded here:
 * the window is chosen with every other window written down, and net32.c
 * now checks the return value instead of trusting the mapping.
 *
 * The mapping is PAGE32_FLAG_WRITE | PAGE32_FLAG_MMIO: uncached and
 * write-through, because those are the only two memory-type bits a
 * non-PAE PTE has.
 */

#ifndef AURALITE_ARCH_I386_FBMAP32_H
#define AURALITE_ARCH_I386_FBMAP32_H

#include <stdint.h>

#define FB32_WINDOW_BASE  0xFE000000u
#define FB32_WINDOW_SIZE  (24u * 1024u * 1024u)

/* Map [phys, phys+len) into the window and return its virtual address,
 * or NULL when the range does not fit or a page-table allocation fails.
 * Idempotent-safe for the same phys: a second call re-maps nothing and
 * returns the same address (paging32_map refuses an already-mapped PTE,
 * which this function treats as "somebody did it first"). */
void *fb32_map_fb(uint32_t phys, uint32_t len);

#endif /* AURALITE_ARCH_I386_FBMAP32_H */

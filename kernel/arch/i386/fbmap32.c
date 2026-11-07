/* kernel/arch/i386/fbmap32.c -- the framebuffer window (RES-11).
 * See fbmap32.h for the window layout and why it exists.
 */

#include <stdint.h>

#include "kernel/arch/i386/fbmap32.h"
#include "kernel/arch/i386/paging32.h"

#define FRAME_32 0x1000u

void *fb32_map_fb(uint32_t phys, uint32_t len)
{
    if (phys == 0 || len == 0 || len > FB32_WINDOW_SIZE)
        return 0;

    uint32_t start = phys & ~(FRAME_32 - 1u);
    uint32_t end   = (phys + len + FRAME_32 - 1u) & ~(FRAME_32 - 1u);

    for (uint32_t p = start; p < end; p += FRAME_32) {
        uint32_t virt = FB32_WINDOW_BASE + (p - start);
        /* Already mapped by an earlier call: that is success, not an
         * error (paging32_map refuses to overwrite a live PTE). */
        if (paging32_map(virt, p, PAGE32_FLAG_WRITE | PAGE32_FLAG_MMIO) != 0) {
            uint32_t existing = 0;
            if (!paging32_probe(virt, &existing) || existing != p)
                return 0;
        }
    }

    return (void *)(FB32_WINDOW_BASE + (phys - start));
}

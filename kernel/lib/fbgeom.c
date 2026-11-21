/* fbgeom.c — the framebuffer geometry seam (GPU_ACCEL_PLAN.md H1).
 * See fbgeom.h for why this exists instead of an #include.
 *
 * Static storage, plain stores: gfx_init() runs before the heap, and
 * /proc/fb may be read from any context, so the writer publishes three
 * aligned integers and the reader treats a torn read as a stale read —
 * a geometry that is one mode-change old is still a true statement
 * about a frame that was presented.
 */
#include <stdint.h>
#include "kernel/lib/fbgeom.h"

static uint32_t geom_width;
static uint32_t geom_height;
static uint32_t geom_pitch;

void gfx_fbgeom_publish(uint32_t width, uint32_t height, uint32_t pitch_bytes) {
    geom_width = width;
    geom_height = height;
    geom_pitch = pitch_bytes;
}

void gfx_fbgeom_get(uint32_t *width, uint32_t *height, uint32_t *pitch_bytes) {
    if (width) *width = geom_width;
    if (height) *height = geom_height;
    if (pitch_bytes) *pitch_bytes = geom_pitch;
}

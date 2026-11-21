/* fbgeom.h — the framebuffer geometry seam (GPU_ACCEL_PLAN.md H1).
 *
 * WHY THIS FILE EXISTS
 *
 * H1 needs /proc/fb so that a user program can size its own copy bench
 * against the geometry the kernel is actually presenting at.  But
 * kernel/fs may not include a driver header — that is PARITY P1/R6's
 * rule, checked by check_parity_claims.py, and it is a good rule: the
 * driver includes the seam, never the other way round.
 *
 * So the driver REGISTERS the geometry here and fs READS it here.  Same
 * shape as kernel/time.h's ktime seam, which paid the pit.h coupling out
 * of procfs for exactly the same reason.  The registration is a plain
 * store of three integers: no allocation, no locking, callable from
 * gfx_init before the heap exists.
 *
 * NOT a second source of truth: graphics.c calls gfx_fbgeom_publish()
 * from the one place that already owns fb_width/fb_height/fb_pitch.
 */
#ifndef AURALITE_LIB_FBGEOM_H
#define AURALITE_LIB_FBGEOM_H

#include <stdint.h>

/* Published by the framebuffer driver once the mode is known.  Zero
 * width means "no framebuffer" — the VGA-text lane on a virtio-gpu with
 * no linear mode, which is a real configuration and must read as such
 * rather than as a 1x1 screen. */
void     gfx_fbgeom_publish(uint32_t width, uint32_t height, uint32_t pitch_bytes);
void     gfx_fbgeom_get(uint32_t *width, uint32_t *height, uint32_t *pitch_bytes);

#endif /* AURALITE_LIB_FBGEOM_H */

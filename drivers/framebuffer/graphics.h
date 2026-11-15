#ifndef AURALITE_DRIVERS_FRAMEBUFFER_GRAPHICS_H
#define AURALITE_DRIVERS_FRAMEBUFFER_GRAPHICS_H

#include <stdint.h>

/*
 * 2D graphics library for the AuraLite OS framebuffer.
 *
 * Provides pixel plotting, filled rectangles, lines, and double-buffering
 * (render to an off-screen back buffer, then flip to avoid tearing). The
 * library is built on top of the Phase 1 framebuffer console.
 */

/* Initialise the graphics layer (allocates a back buffer). */
void gfx_init(void);

/* Colour type (32-bit ARGB/RGB, matching the framebuffer layout). */
typedef uint32_t color_t;

/* Common colours. */
#define GFX_BLACK   0x00000000
#define GFX_WHITE   0x00FFFFFF
#define GFX_RED     0x00FF0000
#define GFX_GREEN   0x0000FF00
#define GFX_BLUE    0x000000FF
#define GFX_YELLOW  0x00FFFF00
#define GFX_CYAN    0x0000FFFF
#define GFX_MAGENTA 0x00FF00FF
#define GFX_GRAY    0x00808080
#define GFX_DARKBLUE 0x00000028

/* Draw a single pixel to the back buffer. */
void gfx_putpixel(uint32_t x, uint32_t y, color_t color);

/* Fill a rectangle (x, y, w, h) with `color` on the back buffer. */
void gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, color_t color);

/* Draw a rectangle outline (1px border). */
void gfx_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, color_t color);

/* Draw a line from (x0,y0) to (x1,y1) using Bresenham's algorithm. */
void gfx_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, color_t color);

/* Draw a string at pixel position (x, y) in `color`. */
void gfx_draw_string(uint32_t x, uint32_t y, const char *s, color_t color);

/* Compatibility helpers used by the boot splash / GUI code. */
void gfx_draw_text(uint32_t x, uint32_t y, const char *s, color_t color);
void gfx_draw_text_centered(uint32_t y, const char *s, color_t color);
uint32_t gfx_text_width(const char *s);
color_t gfx_blend(color_t a, color_t b, uint8_t t);
void gfx_gradient_v(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    color_t top, color_t bottom);
void gfx_draw_circle(uint32_t cx, uint32_t cy, uint32_t r, color_t color);
void gfx_fill_circle(uint32_t cx, uint32_t cy, uint32_t r, color_t color);

/* Copy a rectangle of packed back-buffer pixels.  The software cursor
 * saves the scene, stamps itself, flips, then restores: cursor pixels
 * must not remain in the back buffer or a later dirty clip that misses
 * them reprints a dotted trail.  These ignore the compositor clip. */
void gfx_back_get_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t *dst);
void gfx_back_put_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, const uint32_t *src);

/* Flip the back buffer to the front (copy to the visible framebuffer). */
void gfx_flip(void);

/*
 * Flip only a sub-rectangle of the back buffer to the front.
 * Used by the dirty-rect compositor for partial redraws.
 * Clips to screen bounds.
 */
void gfx_flip_rect(int32_t x, int32_t y, uint32_t w, uint32_t h);

/* Clear the back buffer to `color`. */
void gfx_clear(color_t color);

/* OPT_PLAN.md O4: compositor clip rectangle + composited-pixel counter.
 * The clip bounds every primitive's back-buffer writes; the counter is
 * drained once per frame by the compositor into perfstat. */
void     gfx_clip_set(int32_t x, int32_t y, uint32_t w, uint32_t h);
void     gfx_clip_clear(void);
uint64_t gfx_take_px_counter(void);

/* Get framebuffer dimensions. */
uint32_t gfx_get_width(void);
uint32_t gfx_get_height(void);

/* Fatal stop: further gfx_flip / gfx_flip_rect become no-ops so an
 * AP compositor cannot overwrite the blue screen. */
void gfx_bsod_seize(void);

/* WR-3 + SIMD: blit a packed 32-bit rectangle into the back buffer,
 * honouring both the compositor clip and a caller-supplied skip-mask.
 *
 * src/src_stride are in PIXELS (stride is the source row pitch).
 * skip_mask, if non-NULL, has one bit per source pixel: 1 = skip (the
 * WR-3 hole).  The mask row is `(w+7)/8` bytes, MSB-first, and
 * advances by `mask_stride` bytes per source row.
 *
 * Uses SSE2 when the CPU has it, scalar fallback otherwise.
 * Returns the number of source pixels actually stored (post-clip,
 * excluding skipped).
 */
uint64_t gfx_blit_rgb888(int32_t x, int32_t y, uint32_t w, uint32_t h,
                         const uint32_t *src, uint32_t src_stride,
                         const uint8_t *skip_mask, uint32_t mask_stride);

/* ---------------------------------------------------------------------
 * MSDF / anti-aliasing blending primitives.
 *
 * gfx_blend_pixel is the scalar entry point used for anti-aliased edges
 * and single dots.  gfx_blend_pixels4 and gfx_blend_span operate on
 * horizontal runs and use SSE2 when the CPU supports it, falling back to
 * scalar blending otherwise.  All three honour the compositor clip.
 * ------------------------------------------------------------------- */

/* Blend one pixel with the given alpha (0 = skip, 255 = write-through). */
void gfx_blend_pixel(int32_t x, int32_t y, uint32_t color, uint8_t alpha);

/* Blend four adjacent horizontal pixels with per-pixel alphas.
 * This is the fast path for MSDF glyph rows. */
void gfx_blend_pixels4(int32_t x, int32_t y, uint32_t color,
                       const uint8_t alpha[4]);

/* Blend a whole horizontal run, one alpha per pixel.  Reduces per-pixel
 * FFI overhead for the Rust glyph renderer. */
void gfx_blend_span(int32_t x, int32_t y, uint32_t count,
                    uint32_t color, const uint8_t *alphas);

/* ---------------------------------------------------------------------
 * Modern UI effects: translucent fills, per-pixel-alpha blits, blur.
 * ------------------------------------------------------------------- */

/* Fill a rectangle with `color` at the given alpha.  SIMD-accelerated
 * on SSE2 hosts.  Used for Mica/acrylic panels, modal dimming,
 * and hover highlights. */
void gfx_fill_rect_alpha(int32_t x, int32_t y, uint32_t w, uint32_t h,
                         uint32_t color, uint8_t alpha);

/* Blit a 0xAARRGGBB source rectangle (stride in pixels).  Pixels with
 * source alpha 0 are skipped; 255 is copied verbatim; otherwise blended. */
void gfx_blit_alpha(int32_t dst_x, int32_t dst_y, uint32_t w, uint32_t h,
                    const uint32_t *src, uint32_t stride);

/* Two-pass box blur over a back-buffer region.  Used for frosted-glass
 * (Acrylic) task bars and Start menus.  Allocates a temporary buffer;
 * becomes a no-op on allocation failure. */
void gfx_box_blur_rect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                       uint32_t radius);

/* ---------------------------------------------------------------------
 * Clip query, AA rounded rect, four-corner gradient.
 * ------------------------------------------------------------------- */

/* Report the current compositor clip rectangle.  Any output pointer
 * may be NULL. */
void gfx_get_clip(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h);

/* Rounded rectangle with anti-aliased edges via 4x4 supersampling of
 * the four corner arcs.  The straight edges use the fast opaque fill. */
void gfx_fill_round_rect_aa(int32_t x, int32_t y, uint32_t w, uint32_t h,
                            uint32_t r, uint32_t color);

/* Four-corner bilinear gradient.  Interpolation happens in RGB space and
 * the result is written straight into the back buffer. */
void gfx_gradient_rect_4(int32_t x, int32_t y, uint32_t w, uint32_t h,
                         uint32_t c_tl, uint32_t c_tr,
                         uint32_t c_bl, uint32_t c_br);

#endif /* AURALITE_DRIVERS_FRAMEBUFFER_GRAPHICS_H */

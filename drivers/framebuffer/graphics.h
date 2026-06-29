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

/* Get framebuffer dimensions. */
uint32_t gfx_get_width(void);
uint32_t gfx_get_height(void);

#endif /* AURALITE_DRIVERS_FRAMEBUFFER_GRAPHICS_H */

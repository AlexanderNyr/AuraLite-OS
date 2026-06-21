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

/* Draw a centred string at pixel position (x, y) in `color`. */
void gfx_draw_string(uint32_t x, uint32_t y, const char *s, color_t color);

/* Flip the back buffer to the front (copy to the visible framebuffer). */
void gfx_flip(void);

/* Clear the back buffer to `color`. */
void gfx_clear(color_t color);

/* Get framebuffer dimensions. */
uint32_t gfx_get_width(void);
uint32_t gfx_get_height(void);

#endif /* AURALITE_DRIVERS_FRAMEBUFFER_GRAPHICS_H */

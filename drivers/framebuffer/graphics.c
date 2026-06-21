/* graphics.c — 2D graphics library with double-buffering.
 *
 * Renders to an off-screen back buffer allocated via kmalloc, then copies
 * (flips) to the visible Limine framebuffer. This avoids tearing/flicker
 * during multi-element redraws. Pixel plotting, filled rectangles, lines
 * (Bresenham), and bitmap-font text are provided.
 */

#include <stdint.h>
#include "drivers/framebuffer/graphics.h"
#include "drivers/framebuffer/font.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/string.h"
#include "limine/limine.h"
#include "kernel/limine_requests.h"

static uint32_t *front_fb = NULL;   /* the visible Limine framebuffer */
static uint32_t *back_fb  = NULL;   /* off-screen render target       */
static uint32_t  fb_width  = 0;
static uint32_t  fb_height = 0;
static uint32_t  fb_pitch  = 0;     /* bytes per scanline              */
static uint8_t   r_shift, g_shift, b_shift;

static color_t make_color(uint32_t rgb) {
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8)  & 0xFF;
    uint8_t b =  rgb        & 0xFF;
    return ((uint32_t)r << r_shift) | ((uint32_t)g << g_shift) | ((uint32_t)b << b_shift);
}

void gfx_init(void) {
    extern struct limine_framebuffer *limine_get_framebuffer(void);
    struct limine_framebuffer *fb = limine_get_framebuffer();
    if (fb == NULL || fb->bpp != 32) {
        return;
    }

    front_fb  = (uint32_t *)fb->address;
    fb_width  = (uint32_t)fb->width;
    fb_height = (uint32_t)fb->height;
    fb_pitch  = (uint32_t)fb->pitch;
    r_shift   = fb->red_mask_shift;
    g_shift   = fb->green_mask_shift;
    b_shift   = fb->blue_mask_shift;

    /* Allocate the back buffer (same size as the framebuffer). */
    uint64_t buf_bytes = (uint64_t)fb_pitch * fb_height;
    back_fb = kmalloc(buf_bytes);
    if (back_fb) {
        memset(back_fb, 0, buf_bytes);
    }
}

void gfx_putpixel(uint32_t x, uint32_t y, color_t color) {
    if (!back_fb || x >= fb_width || y >= fb_height) {
        return;
    }
    /* Pre-compute the packed colour for our mask layout. */
    color_t packed = make_color(color);
    uint32_t pitch32 = fb_pitch / 4;
    back_fb[y * pitch32 + x] = packed;
}

void gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, color_t color) {
    color_t packed = make_color(color);
    uint32_t pitch32 = fb_pitch / 4;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t py = y + row;
        if (py >= fb_height) break;
        for (uint32_t col = 0; col < w; col++) {
            uint32_t px = x + col;
            if (px >= fb_width) break;
            back_fb[py * pitch32 + px] = packed;
        }
    }
}

void gfx_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, color_t color) {
    /* Top and bottom edges. */
    for (uint32_t i = 0; i < w; i++) {
        gfx_putpixel(x + i, y, color);
        gfx_putpixel(x + i, y + h - 1, color);
    }
    /* Left and right edges. */
    for (uint32_t i = 0; i < h; i++) {
        gfx_putpixel(x, y + i, color);
        gfx_putpixel(x + w - 1, y + i, color);
    }
}

void gfx_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, color_t color) {
    int dx = (x1 > x0) ? (int)(x1 - x0) : (int)(x0 - x1);
    int dy = (y1 > y0) ? (int)(y1 - y0) : (int)(y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        gfx_putpixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void gfx_draw_string(uint32_t x, uint32_t y, const char *s, color_t color) {
    uint32_t cx = x;
    for (; *s; s++) {
        if (*s == '\n') {
            cx = x;
            y += 8;
            continue;
        }
        const char *glyph = font8x8_basic[(unsigned char)*s & 0x7F];
        for (int gy = 0; gy < 8; gy++) {
            for (int gx = 0; gx < 8; gx++) {
                if (glyph[gy] & (1 << gx)) {
                    gfx_putpixel(cx + gx, y + gy, color);
                }
            }
        }
        cx += 8;
    }
}

void gfx_flip(void) {
    if (!back_fb || !front_fb) {
        return;
    }
    uint32_t pitch32 = fb_pitch / 4;
    for (uint32_t y = 0; y < fb_height; y++) {
        memcpy(front_fb + y * pitch32, back_fb + y * pitch32,
               (size_t)fb_pitch);
    }
}

void gfx_clear(color_t color) {
    if (!back_fb) return;
    color_t packed = make_color(color);
    uint32_t pitch32 = fb_pitch / 4;
    for (uint32_t y = 0; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            back_fb[y * pitch32 + x] = packed;
        }
    }
}

uint32_t gfx_get_width(void)  { return fb_width; }
uint32_t gfx_get_height(void) { return fb_height; }

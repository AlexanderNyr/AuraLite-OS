/* fb.c — linear framebuffer console with an 8x8 bitmap font. */

#include <stdint.h>
#include <stddef.h>
#include "drivers/framebuffer/fb.h"
#include "drivers/framebuffer/font.h"
#include "limine/limine.h"
#include "kernel/limine_requests.h"

#define FONT_WIDTH   8
#define FONT_HEIGHT  8

static uint32_t *fb_addr     = NULL;
static uint64_t  fb_width    = 0;
static uint64_t  fb_height   = 0;
static uint64_t  fb_pitch    = 0;   /* bytes per scanline */

static uint32_t fb_fg;              /* foreground colour (text) */
static uint32_t fb_bg;              /* background colour */

static int fb_cursor_col;
static int fb_cursor_row;
static int fb_cols;                 /* characters per line  */
static int fb_rows;                 /* lines on the screen  */

static uint8_t fb_r_shift, fb_g_shift, fb_b_shift;

/* Pack an RGB triplet into a framebuffer pixel using Limine's mask shifts. */
static uint32_t make_color(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << fb_r_shift)
         | ((uint32_t)g << fb_g_shift)
         | ((uint32_t)b << fb_b_shift);
}

static inline uint32_t *pixel_addr(uint32_t x, uint32_t y) {
    /* pitch is in bytes; we index a uint32_t (4-byte) array. */
    return fb_addr + y * (fb_pitch / 4) + x;
}

static void fb_putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x < fb_width && y < fb_height) {
        *pixel_addr(x, y) = color;
    }
}

static void fb_draw_char(unsigned char c, uint32_t origin_x, uint32_t origin_y) {
    const char *glyph = font8x8_basic[c & 0x7F];
    for (int gy = 0; gy < FONT_HEIGHT; gy++) {
        uint8_t bits = (uint8_t)glyph[gy];
        for (int gx = 0; gx < FONT_WIDTH; gx++) {
            fb_putpixel(origin_x + (uint32_t)gx, origin_y + (uint32_t)gy,
                        (bits & (1 << gx)) ? fb_fg : fb_bg);
        }
    }
}

static void fb_scroll(void) {
    /* Shift the whole framebuffer up by one glyph row, then blank the last row. */
    uint32_t pitch32 = (uint32_t)(fb_pitch / 4);

    for (uint32_t y = FONT_HEIGHT; y < fb_height; y++) {
        uint32_t *src = fb_addr + y * pitch32;
        uint32_t *dst = fb_addr + (y - FONT_HEIGHT) * pitch32;
        for (uint32_t x = 0; x < fb_width; x++) {
            dst[x] = src[x];
        }
    }
    for (uint32_t y = fb_height - FONT_HEIGHT; y < fb_height; y++) {
        uint32_t *row = fb_addr + y * pitch32;
        for (uint32_t x = 0; x < fb_width; x++) {
            row[x] = fb_bg;
        }
    }
}

void fb_init(void) {
    struct limine_framebuffer *fb = limine_get_framebuffer();
    if (fb == NULL || fb->bpp != 32 || fb->memory_model != LIMINE_FRAMEBUFFER_RGB) {
        fb_addr = NULL;   /* no usable framebuffer; fb_putchar will be a no-op */
        return;
    }

    fb_addr   = (uint32_t *)fb->address;
    fb_width  = fb->width;
    fb_height = fb->height;
    fb_pitch  = fb->pitch;

    fb_r_shift = fb->red_mask_shift;
    fb_g_shift = fb->green_mask_shift;
    fb_b_shift = fb->blue_mask_shift;

    fb_fg = make_color(220, 220, 220);   /* light grey text  */
    fb_bg = make_color(16,  16,  28);    /* near-black blue  */

    fb_cols = (int)(fb_width  / FONT_WIDTH);
    fb_rows = (int)(fb_height / FONT_HEIGHT);
    fb_cursor_col = 0;
    fb_cursor_row = 0;

    fb_clear();
}

void fb_clear(void) {
    if (!fb_addr) {
        return;
    }
    uint32_t pitch32 = (uint32_t)(fb_pitch / 4);
    for (uint32_t y = 0; y < fb_height; y++) {
        uint32_t *row = fb_addr + y * pitch32;
        for (uint32_t x = 0; x < fb_width; x++) {
            row[x] = fb_bg;
        }
    }
    fb_cursor_col = 0;
    fb_cursor_row = 0;
}

void fb_putchar(char c) {
    if (!fb_addr) {
        return;
    }
    unsigned char uc = (unsigned char)c;

    switch (uc) {
    case '\n':
        fb_cursor_col = 0;
        fb_cursor_row++;
        break;
    case '\r':
        fb_cursor_col = 0;
        break;
    case '\t':
        fb_cursor_col = (fb_cursor_col + 4) & ~3;
        if (fb_cursor_col >= fb_cols) {
            fb_cursor_col = 0;
            fb_cursor_row++;
        }
        break;
    default:
        if (fb_cursor_col >= fb_cols) {
            fb_cursor_col = 0;
            fb_cursor_row++;
        }
        fb_draw_char(uc,
                     (uint32_t)fb_cursor_col * FONT_WIDTH,
                     (uint32_t)fb_cursor_row * FONT_HEIGHT);
        fb_cursor_col++;
        break;
    }

    if (fb_cursor_row >= fb_rows) {
        fb_scroll();
        fb_cursor_row = fb_rows - 1;
    }
}

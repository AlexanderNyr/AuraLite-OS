/* fb.c — linear framebuffer console with a real PSF1/PSF2 font. */

#include <stdint.h>
#include <stddef.h>
#include "drivers/framebuffer/fb.h"
#include "drivers/framebuffer/psf.h"
#include "kernel/boot_info.h"

static uint32_t *fb_addr = NULL;
static uint64_t fb_width = 0, fb_height = 0, fb_pitch = 0;
static uint32_t fb_fg, fb_bg;
static int fb_cursor_col, fb_cursor_row, fb_cols, fb_rows;
static uint8_t fb_r_shift, fb_g_shift, fb_b_shift;
static uint32_t font_width, font_height;
static int fb_console_on = 1;   /* turned off when the GUI compositor owns the screen */

static uint32_t make_color(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << fb_r_shift) | ((uint32_t)g << fb_g_shift) | ((uint32_t)b << fb_b_shift);
}

static inline uint32_t *pixel_addr(uint32_t x, uint32_t y) {
    return fb_addr + y * (fb_pitch / 4) + x;
}

static void fb_putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x < fb_width && y < fb_height) *pixel_addr(x, y) = color;
}

static void fb_draw_char(unsigned char c, uint32_t ox, uint32_t oy) {
    const struct psf_font *f = psf_get_font();
    if (!f) return;
    uint32_t idx = c;
    if (idx >= f->num_glyphs) idx = 0;
    const uint8_t *glyph = f->data + (uint64_t)idx * f->bytes_per_glyph;
    for (uint32_t row = 0; row < f->height; row++) {
        const uint8_t *row_bytes = glyph + row * f->bytes_per_row;
        for (uint32_t col = 0; col < f->width; col++) {
            uint8_t byte = row_bytes[col / 8];
            uint8_t bit_pos = 7 - (col % 8);
            uint32_t pix = (byte >> bit_pos) & 1 ? fb_fg : fb_bg;
            fb_putpixel(ox + col, oy + row, pix);
        }
    }
}

static void fb_scroll(void) {
    uint32_t pitch32 = (uint32_t)(fb_pitch / 4);
    for (uint32_t y = font_height; y < fb_height; y++) {
        uint32_t *src = fb_addr + y * pitch32;
        uint32_t *dst = fb_addr + (y - font_height) * pitch32;
        for (uint32_t x = 0; x < fb_width; x++) dst[x] = src[x];
    }
    for (uint32_t y = fb_height - font_height; y < fb_height; y++) {
        uint32_t *row = fb_addr + y * pitch32;
        for (uint32_t x = 0; x < fb_width; x++) row[x] = fb_bg;
    }
}

void fb_init(void) {
    /* Initialise the PSF font subsystem unconditionally, before any early
     * return below. The font is used not just by this text console but by
     * the whole GUI text-rendering path (gui_draw_text() in kernel/gui/gui.c,
     * gfx_draw_string() in graphics.c): those draw into a window's private
     * backing buffer, which exists and is drawable even when there is no
     * usable linear framebuffer to actually display it on (e.g. a legacy
     * BIOS boot with no VBE mode set). Gating psf_init() behind the
     * framebuffer check here used to be harmless when the font was a
     * hardcoded 8x8 C array with no init step, but now that it is a real
     * parsed PSF2 image, skipping psf_init() left psf_get_font() returning
     * NULL and every GUI text draw silently failing on such boots. */
    psf_init();

    boot_fb_t *fb = boot_get_framebuffer();
    if (!fb || fb->bpp != 32) { fb_addr = NULL; return; }
    const struct psf_font *f = psf_get_font();
    if (!f) { fb_addr = NULL; return; }
    font_width = f->width; font_height = f->height;
    /* boot_fb_t.phys_base is a raw physical address; add the HHDM
     * offset so the framebuffer is accessible in kernel virtual space. */
    fb_addr = (uint32_t *)(uintptr_t)(boot_get_hhdm_offset() + fb->phys_base);
    fb_width = fb->width; fb_height = fb->height; fb_pitch = fb->pitch;
    fb_r_shift = fb->red_shift;
    fb_g_shift = fb->green_shift;
    fb_b_shift = fb->blue_shift;
    fb_fg = make_color(220, 220, 220);
    fb_bg = make_color(16, 16, 28);
    fb_cols = fb_width / font_width;
    fb_rows = fb_height / font_height;
    fb_cursor_col = fb_cursor_row = 0;
    fb_clear();
}

void fb_clear(void) {
    if (!fb_addr) return;
    uint32_t pitch32 = fb_pitch / 4;
    for (uint32_t y = 0; y < fb_height; y++) {
        uint32_t *row = fb_addr + y * pitch32;
        for (uint32_t x = 0; x < fb_width; x++) row[x] = fb_bg;
    }
    fb_cursor_col = fb_cursor_row = 0;
}

void fb_set_console_enabled(int on) { fb_console_on = on ? 1 : 0; }
int  fb_console_enabled(void)       { return fb_console_on; }

void fb_putchar(char c) {
    if (!fb_addr || !fb_console_on) return;
    unsigned char uc = c;
    switch (uc) {
    case '\n': fb_cursor_col = 0; fb_cursor_row++; break;
    case '\r': fb_cursor_col = 0; break;
    case '\t':
        fb_cursor_col = (fb_cursor_col + 4) & ~3;
        if (fb_cursor_col >= fb_cols) { fb_cursor_col = 0; fb_cursor_row++; }
        break;
    default:
        if (fb_cursor_col >= fb_cols) { fb_cursor_col = 0; fb_cursor_row++; }
        fb_draw_char(uc, fb_cursor_col * font_width, fb_cursor_row * font_height);
        fb_cursor_col++;
        break;
    }
    if (fb_cursor_row >= fb_rows) { fb_scroll(); fb_cursor_row = fb_rows - 1; }
}

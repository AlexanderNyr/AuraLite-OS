/* psf.c — PSF font rendering.
 *
 * Parses the embedded PSF1 8x16 font and renders glyphs to the framebuffer
 * via the graphics library. Replaces the old 8x8 font for much sharper text.
 */

#include <stdint.h>
#include "drivers/framebuffer/psf.h"
#include "drivers/framebuffer/graphics.h"
#include "drivers/framebuffer/psf_font.h"

static struct psf_font font;

void psf_init(void) {
    /* The embedded font is PSF1 8x16 (pre-extracted glyph data).
     * No header parsing needed — the constants are in psf_font.h. */
    font.width         = PSF_FONT_WIDTH;
    font.height        = PSF_FONT_HEIGHT;
    font.bytes_per_row = (PSF_FONT_WIDTH + 7) / 8;  /* 1 byte for 8-wide */
    font.num_glyphs    = PSF_FONT_GLYPHS;
    font.data          = psf_font;
}

const struct psf_font *psf_get_font(void) {
    return &font;
}

void psf_draw_glyph(uint32_t x, uint32_t y, char c,
                    uint32_t fg_packed, uint32_t bg_packed) {
    uint32_t glyph_index = (uint32_t)(unsigned char)c;
    if (glyph_index >= font.num_glyphs) {
        glyph_index = 0;  /* render as space/null for out-of-range chars */
    }

    const uint8_t *glyph = font.data +
        glyph_index * font.bytes_per_row * font.height;

    for (uint32_t row = 0; row < font.height; row++) {
        for (uint32_t col = 0; col < font.width; col++) {
            /* For each row, the byte at glyph[row * bytes_per_row + col/8]
             * holds 8 pixels. Bit (col % 8) selects the pixel, but PSF uses
             * MSB-first within each byte: bit 7 is the leftmost pixel. */
            uint8_t byte = glyph[row * font.bytes_per_row + col / font.bytes_per_row];
            uint8_t bit_pos = 7 - (col % 8);  /* MSB = leftmost */
            uint32_t pixel = (byte >> bit_pos) & 1 ? fg_packed : bg_packed;
            gfx_putpixel(x + col, y + row, pixel);
        }
    }
}

void psf_draw_string(uint32_t x, uint32_t y, const char *s,
                     uint32_t fg_packed, uint32_t bg_packed) {
    uint32_t cx = x;
    uint32_t cy = y;

    for (; *s; s++) {
        if (*s == '\n') {
            cx = x;
            cy += font.height;
            continue;
        }
        psf_draw_glyph(cx, cy, *s, fg_packed, bg_packed);
        cx += font.width;
    }
}

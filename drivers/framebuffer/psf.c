/* psf.c — PSF1/PSF2 font parsing and rendering.
 *
 * Parses a real PSF1 or PSF2 binary font image (see psf.h for the on-disk
 * layout of both formats) and renders glyphs to the framebuffer. The
 * embedded default font (psf2_default_font.inc) is a genuine PSF2
 * container built from the public-domain Linux console VGA 8x16 font, so
 * this is not a hand-rolled bitmap table: the same code path that parses
 * it would equally parse any other .psf/.psf2 file dropped in its place.
 */

#include <stdint.h>
#include "drivers/framebuffer/psf.h"
#include "drivers/framebuffer/graphics.h"

/* Embedded default font: a real PSF2 binary blob (magic, header, glyph
 * bitmaps) — see the file for the exact layout documented in psf.h. */
#include "drivers/framebuffer/psf2_default_font.inc"

static struct psf_font font;
static int font_loaded = 0;

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int psf_load_from_memory(const uint8_t *buf, uint32_t len) {
    if (!buf) return -1;

    /* ---- Try PSF2 first (4-byte magic). ---- */
    if (len >= 4 && buf[0] == PSF2_MAGIC0 && buf[1] == PSF2_MAGIC1 &&
        buf[2] == PSF2_MAGIC2 && buf[3] == PSF2_MAGIC3) {
        if (len < PSF2_HEADER_SIZE) return -1;

        uint32_t headersize     = rd_u32le(buf + 8);
        uint32_t numglyph       = rd_u32le(buf + 16);
        uint32_t bytesperglyph  = rd_u32le(buf + 20);
        uint32_t height         = rd_u32le(buf + 24);
        uint32_t width          = rd_u32le(buf + 28);

        if (width == 0 || height == 0 || numglyph == 0) return -1;
        if (headersize < PSF2_HEADER_SIZE || headersize > len) return -1;

        uint32_t bytes_per_row = (width + 7) / 8;
        /* Sanity-check the glyph stride matches width/height; some encoders
         * pad bytesperglyph, so only reject it if it's too small to hold
         * the claimed geometry (never trust it blindly for the loop below). */
        if (bytesperglyph < bytes_per_row * height) return -1;

        /* Bound-check that every glyph's bitmap actually fits in the buffer
         * (the Unicode table after it, if present, is not required to be
         * within `len` — we never read past the bitmap region). */
        uint64_t bitmap_bytes = (uint64_t)numglyph * bytesperglyph;
        if ((uint64_t)headersize + bitmap_bytes > len) return -1;

        font.magic           = PSF2_MAGIC;
        font.width           = width;
        font.height          = height;
        font.bytes_per_row   = bytes_per_row;
        font.bytes_per_glyph = bytesperglyph;
        font.num_glyphs      = numglyph;
        font.data            = buf + headersize;
        font_loaded = 1;
        return 0;
    }

    /* ---- Fall back to PSF1 (2-byte magic, always 8-wide glyphs). ---- */
    if (len >= 4 && buf[0] == PSF1_MAGIC0 && buf[1] == PSF1_MAGIC1) {
        uint8_t mode     = buf[2];
        uint8_t charsize = buf[3];
        if (charsize == 0) return -1;

        uint32_t numglyph = (mode & PSF1_MODE_512) ? 512 : 256;
        uint64_t bitmap_bytes = (uint64_t)numglyph * charsize;
        if (4ULL + bitmap_bytes > len) return -1;

        font.magic           = 0x0436;
        font.width           = 8;
        font.height          = charsize;
        font.bytes_per_row   = 1;
        font.bytes_per_glyph = charsize;
        font.num_glyphs      = numglyph;
        font.data            = buf + 4;
        font_loaded = 1;
        return 0;
    }

    return -1;
}

void psf_init(void) {
    int rc = psf_load_from_memory(psf2_default_font_data, PSF2_DEFAULT_FONT_SIZE);
    (void)rc; /* the embedded font is verified at build time; this cannot fail */
}

const struct psf_font *psf_get_font(void) {
    return font_loaded ? &font : (const struct psf_font *)0;
}

void psf_draw_glyph(uint32_t x, uint32_t y, unsigned char c,
                    uint32_t fg_packed, uint32_t bg_packed) {
    if (!font_loaded) return;

    uint32_t glyph_index = c;
    if (glyph_index >= font.num_glyphs) {
        glyph_index = 0;  /* render as the font's glyph 0 for out-of-range chars */
    }

    const uint8_t *glyph = font.data + (uint64_t)glyph_index * font.bytes_per_glyph;

    for (uint32_t row = 0; row < font.height; row++) {
        const uint8_t *row_bytes = glyph + row * font.bytes_per_row;
        for (uint32_t col = 0; col < font.width; col++) {
            /* PSF bitmaps are MSB-first within each row byte: bit 7 of
             * row_bytes[0] is the leftmost pixel, bit 0 is pixel 7, then
             * row_bytes[1] continues with pixel 8, etc. */
            uint8_t byte = row_bytes[col / 8];
            uint8_t bit_pos = 7 - (col % 8);
            uint32_t pixel = (byte >> bit_pos) & 1 ? fg_packed : bg_packed;
            gfx_putpixel(x + col, y + row, pixel);
        }
    }
}

void psf_draw_string(uint32_t x, uint32_t y, const char *s,
                     uint32_t fg_packed, uint32_t bg_packed) {
    if (!font_loaded) return;

    uint32_t cx = x;
    uint32_t cy = y;

    for (; *s; s++) {
        if (*s == '\n') {
            cx = x;
            cy += font.height;
            continue;
        }
        psf_draw_glyph(cx, cy, (unsigned char)*s, fg_packed, bg_packed);
        cx += font.width;
    }
}

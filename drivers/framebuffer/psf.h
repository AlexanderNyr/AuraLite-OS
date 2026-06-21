#ifndef AURALITE_DRIVERS_FRAMEBUFFER_PSF_H
#define AURALITE_DRIVERS_FRAMEBUFFER_PSF_H

#include <stdint.h>

/*
 * PSF (PC Screen Font) font support.
 *
 * Supports both PSF1 (8xN, up to 512 glyphs) and PSF2 (variable width/height).
 * The default font is lat0-16.psf (PSF1, 8x16, 256 glyphs) embedded as a C
 * array. This provides much sharper text than the previous 8x8 font.
 *
 * Glyph rendering: each glyph is a bitmap where each byte represents one row
 * of pixels. For an 8-wide font, each row is one byte; bit 0 (LSB) is the
 * leftmost pixel. For wider fonts, rows span multiple bytes.
 */

/* PSF1 magic (0x36 0x04) */
#define PSF1_MAGIC 0x0436

/* PSF2 magic (0x72 0xb5 0x4a 0x86) */
#define PSF2_MAGIC 0x864ab572u

/* Font descriptor — filled by psf_init(). */
struct psf_font {
    uint32_t width;        /* glyph width in pixels */
    uint32_t height;       /* glyph height in pixels (= rows) */
    uint32_t bytes_per_row;/* bytes per row of pixels */
    uint32_t num_glyphs;   /* total glyphs in the font */
    const uint8_t *data;   /* pointer to glyph data */
};

/* Initialise the PSF font subsystem with the embedded default font. */
void psf_init(void);

/* Get the active font descriptor. */
const struct psf_font *psf_get_font(void);

/*
 * Render a single glyph at pixel position (x, y) with the given foreground
 * and background colours. The colours are pre-packed framebuffer pixels
 * (not RGB triplets).
 */
void psf_draw_glyph(uint32_t x, uint32_t y, char c,
                    uint32_t fg_packed, uint32_t bg_packed);

/*
 * Render a string at pixel position (x, y). Each character advances by
 * font width. Newlines advance Y by font height and reset X.
 */
void psf_draw_string(uint32_t x, uint32_t y, const char *s,
                     uint32_t fg_packed, uint32_t bg_packed);

#endif /* AURALITE_DRIVERS_FRAMEBUFFER_PSF_H */

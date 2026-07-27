#ifndef AURALITE_DRIVERS_FRAMEBUFFER_PSF_H
#define AURALITE_DRIVERS_FRAMEBUFFER_PSF_H

#include <stdint.h>

/*
 * PSF (PC Screen Font) support — real PSF1 and PSF2 binary parsing.
 *
 * PSF2 is the modern console font container used by Linux's `setfont`/`kbd`
 * tooling (see /usr/share/consolefonts for real-world examples on any
 * Debian system). It has a proper versioned header:
 *
 *   offset 0   : 4-byte magic 0x72 0xb5 0x4a 0x86 ("\x72\xb5\x4a\x86")
 *   offset 4   : uint32 version        (0 for the only defined version)
 *   offset 8   : uint32 headersize     (bytes before the glyph bitmap data)
 *   offset 12  : uint32 flags          (bit 0: a Unicode translation table
 *                                        follows the glyph bitmaps)
 *   offset 16  : uint32 numglyph       (glyph count)
 *   offset 20  : uint32 bytesperglyph  (bitmap bytes per glyph)
 *   offset 24  : uint32 height         (glyph height in pixels)
 *   offset 28  : uint32 width          (glyph width in pixels)
 *   offset headersize .. : numglyph * bytesperglyph bytes of 1-bpp glyph
 *                          bitmaps (MSB-first per row, rows padded to a
 *                          whole byte), optionally followed by a UTF-8
 *                          Unicode translation table (ignored by this
 *                          driver — glyph index == byte value, which is
 *                          how every code-page-based PSF font is built).
 *
 * PSF1 (the older, simpler format previously hardcoded by this driver) is
 * still recognised for compatibility with legacy embedded blobs:
 *
 *   offset 0 : 2-byte magic 0x36 0x04
 *   offset 2 : uint8 mode   (bit 0 set => 512 glyphs, else 256)
 *   offset 3 : uint8 charsize (glyph height; width is always 8)
 *   offset 4 .. : numglyph * charsize bytes of glyph bitmap data.
 *
 * psf_load_from_memory() auto-detects either format from its magic bytes,
 * so any real .psf/.psf2 font file (embedded as a C array, or loaded from
 * a filesystem at some point in the future) can be dropped in without
 * touching the parser.
 */

/* PSF1 magic (2 bytes: 0x36 0x04) */
#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04
#define PSF1_MODE_512 0x01

/* PSF2 magic (4 bytes, stored little-endian as a 32-bit word: 0x864ab572) */
#define PSF2_MAGIC0 0x72
#define PSF2_MAGIC1 0xb5
#define PSF2_MAGIC2 0x4a
#define PSF2_MAGIC3 0x86
#define PSF2_MAGIC  0x864ab572u
#define PSF2_HEADER_SIZE 32
#define PSF2_HAS_UNICODE_TABLE 0x00000001u

/* Font descriptor — filled by psf_init()/psf_load_from_memory(). */
struct psf_font {
    uint32_t magic;           /* PSF1_MAGIC or PSF2_MAGIC (which format this came from) */
    uint32_t width;           /* glyph width in pixels */
    uint32_t height;          /* glyph height in pixels (= rows) */
    uint32_t bytes_per_row;   /* ceil(width / 8): bytes spanned by one glyph row */
    uint32_t bytes_per_glyph; /* bytes_per_row * height: total bitmap size per glyph */
    uint32_t num_glyphs;      /* total glyphs in the font */
    const uint8_t *data;      /* pointer to the first glyph's bitmap data */
};

/*
 * Parse a PSF1 or PSF2 font image already resident in memory (e.g. an
 * embedded C array or an initrd file loaded into RAM) and make it the
 * active font. `len` must cover at least the header plus every glyph's
 * bitmap (the Unicode table, if any, is not required to be present).
 *
 * Returns 0 on success, -1 if `buf` doesn't look like a well-formed PSF1/
 * PSF2 image (bad magic, truncated data, or a zero width/height/glyph
 * count). On failure the previously active font (if any) is left intact.
 */
int psf_load_from_memory(const uint8_t *buf, uint32_t len);

/* Initialise the PSF font subsystem with the embedded default font (a
 * genuine PSF2 container, VGA 8x16, 256 glyphs — see psf2_default_font.inc). */
void psf_init(void);

/* Get the active font descriptor. Never returns NULL once psf_init() (or a
 * successful psf_load_from_memory()) has run. */
const struct psf_font *psf_get_font(void);

/*
 * Render a single glyph at pixel position (x, y) with the given foreground
 * and background colours. Colours are passed straight through to
 * gfx_putpixel(), which packs them for the active framebuffer layout, so
 * callers should pass plain 0xRRGGBB values here (matching graphics.h's
 * color_t), not already hardware-packed pixels.
 */
void psf_draw_glyph(uint32_t x, uint32_t y, unsigned char c,
                    uint32_t fg_packed, uint32_t bg_packed);

/*
 * Render a string at pixel position (x, y). Each character advances by
 * font width. Newlines advance Y by font height and reset X.
 */
void psf_draw_string(uint32_t x, uint32_t y, const char *s,
                     uint32_t fg_packed, uint32_t bg_packed);

#endif /* AURALITE_DRIVERS_FRAMEBUFFER_PSF_H */

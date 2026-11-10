/* userspace/office/libaoglyph/aoglyph.h — bitmap text for the office suite.
 *
 * OFFICE_PLAN.md OF1 exists because the machine could render and accept exactly
 * one kind of text: 8x16 CP437 glyphs, monospaced, ASCII plus the DOS line-art
 * half of the table, typed through a `char[128]` keymap.  A word processor that
 * cannot set «Привет, мир — 2026» in proportional type with a proper em dash is
 * not a word processor for the people this project is for.
 *
 * So this library does four things, and each one is a paragraph of the plan:
 *
 *   1. LOADERS — BDF 2.1 and PSF2, both parsed from a byte range in memory.
 *      PSF2 because that is what the kernel already ships and what a user can
 *      drop in from a Linux box; BDF because a bitmap face has to be
 *      reviewable and editable as text, not as an opaque blob.  Both loaders
 *      are total functions over their input: a truncated or malformed font
 *      produces an error code, never a read past the end.
 *
 *   2. COVERAGE — codepoint to glyph through a sorted table with binary
 *      search and a '?' fallback.  An unmapped codepoint is a drawn '?', not
 *      a crash and not a silent gap that shifts the rest of the line.
 *
 *   3. STYLE SYNTHESIS — bold by horizontal smear, italic by row shear.  These
 *      are synthesis, not faces: the plan says to document them, and they are
 *      documented here because a synthesised italic is a lie a user should be
 *      able to see through.
 *
 *   4. LAYOUT PRIMITIVES — advance widths, measurement, and greedy line
 *      breaking over space/hyphen/em-dash.  Hyphenation is out of scope (§8 of
 *      the plan): this wraps at opportunities that exist in the text, it does
 *      not invent them.
 *
 * The library is pure: no syscalls, no globals, no libc allocation.  Storage
 * comes from a caller-provided arena (the same ao_arena_t a document uses), so
 * a font dies with the thing that loaded it and the whole file compiles and
 * runs against the host for tests/unit/test_ao_glyph.c.
 */

#ifndef AURALITE_OFFICE_AOGLYPH_H
#define AURALITE_OFFICE_AOGLYPH_H

#include <stddef.h>
#include <stdint.h>

#include "aodoc.h"      /* ao_arena_t, ao_utf8_decode */

/* A face with more glyphs than this is refused rather than truncated: half a
 * font is a document that renders differently depending on which characters it
 * happens to contain.  512 covers Latin-1 + Cyrillic + punctuation with room
 * for a symbol set; the VGA PSF2 has 256. */
#define AO_GLYPH_MAX 512

/* Style bits (AO_STYLE_*, combinable) come from aodoc.h: the document model
 * and the text layer must agree on what a bit means, so they share one
 * definition rather than two that happen to match. */

/* Loader results.  Every loader leaves the font EMPTY on failure, so a caller
 * that ignores the code still cannot draw from a half-parsed face. */
#define AO_FONT_OK            0
#define AO_FONT_E_ARG        -1   /* NULL font/storage/data */
#define AO_FONT_E_TRUNCATED  -2   /* data ends inside a header or a bitmap */
#define AO_FONT_E_FORMAT     -3   /* not this format, or a field is nonsense */
#define AO_FONT_E_CAPACITY   -4   /* more than AO_GLYPH_MAX glyphs */
#define AO_FONT_E_STORAGE    -5   /* the caller's arena is too small */

/* One glyph, in BDF's own geometry.
 *
 * `rows` points at bit-packed rows: `h` rows of `(w + 7) / 8` bytes each, most
 * significant bit leftmost.  The bits live either inside the caller's arena
 * (BDF, decoded from hex) or inside the font image the caller loaded (PSF2,
 * already packed) — hence `const`.
 *
 * `yoff` is the BDF convention and the one thing worth stating twice: rows
 * count UP from the baseline, so a glyph with a descender (р, у, д) has a
 * NEGATIVE yoff and a letter sitting on the baseline has yoff 0.  Getting this
 * backwards is the classic off-by-one that puts '!' two rows under the line.
 */
typedef struct {
    uint32_t      cp;        /* codepoint this glyph draws */
    const uint8_t *rows;     /* bit-packed, (w+7)/8 bytes per row */
    uint16_t      w, h;
    int16_t       xoff, yoff;
    uint16_t      adv;       /* pen advance in pixels (DWIDTH) */
} ao_glyph_t;

typedef struct {
    ao_arena_t   *arena;      /* storage for glyphs and decoded bitmaps */
    ao_glyph_t    g[AO_GLYPH_MAX];
    uint16_t      n;          /* glyphs loaded, sorted by cp */
    uint16_t      cell_w, cell_h;
    int16_t       ascent, descent;
    uint8_t       proportional;
    uint8_t       from_psf2;
    uint16_t      fallback;   /* index of '?' in g[], or 0xFFFF */
    const uint8_t *image;     /* PSF2 glyph block, if loaded from PSF2 */
    uint32_t      image_len;
    uint32_t      bytesperglyph;
    uint16_t      nglyph_raw; /* PSF2 glyph slots, for the index bound check */
} ao_font_t;

/* ---------------------------------------------------------------- loading */

/* Parse a BDF 2.1 font from `data` (not necessarily NUL-terminated).  Glyph
 * bitmaps are decoded into `arena`.  Returns AO_FONT_OK or an AO_FONT_E_*. */
int ao_font_load_bdf(ao_font_t *f, ao_arena_t *arena,
                     const char *data, size_t len);

/* Parse a PSF2 container.  `ascent` is how many rows sit above the baseline;
 * pass 0 for the VGA-like default (height - height/4), which is what an
 * fbsplash font assumed.  Unlike BDF, PSF2 glyphs are NOT copied: `rows`
 * points into `data`, which must outlive the font. */
int ao_font_load_psf2(ao_font_t *f, ao_arena_t *arena,
                      const uint8_t *data, size_t len, int ascent);

/* --------------------------------------------------------------- coverage */

/* The glyph for `cp`, or the '?' glyph, or — for a face without '?' — a blank
 * glyph with the cell advance.  Never NULL, so drawing code has no branch. */
const ao_glyph_t *ao_font_glyph(const ao_font_t *f, uint32_t cp);

/* Pen advance for `cp` (fallback applied). */
int ao_font_advance(const ao_font_t *f, uint32_t cp);

/* 1 when the face really has a glyph for `cp` (no fallback). */
int ao_font_has(const ao_font_t *f, uint32_t cp);

/* ----------------------------------------------------------------- drawing
 *
 * Destinations are packed 32-bit XRGB8888 — the same layout ag_blit() takes,
 * so an application rasterises into its own buffer and presents the band it
 * changed instead of asking the compositor for one syscall per pixel.
 *
 * `w`,`h` are the clip box of that buffer, `stride` its row pitch in PIXELS.
 * Everything is clipped exactly: a baseline above the top edge draws nothing
 * rather than corrupting the row before it.
 */

/* Draw one glyph with its pen at (x, baseline_y).  Returns the number of
 * DISTINCT pixels set inside the clip box (bold smear and italic shear overlap
 * each other, and a pixel is still one pixel), which is what makes the style
 * tests exact instead of approximate. */
int ao_glyph_draw(const ao_font_t *f, uint32_t cp, int style,
                  uint32_t *dst, int stride, int w, int h,
                  int x, int baseline_y, uint32_t color);

/* Draw a NUL-free UTF-8 run.  Returns pixels set; *width_out (optional) gets
 * the pen advance.  Invalid UTF-8 draws one '?' per damaged byte and moves on. */
int ao_text_draw(const ao_font_t *f, const char *s, size_t len, int style,
                 uint32_t *dst, int stride, int w, int h,
                 int x, int baseline_y, uint32_t color, int *width_out);

/* Width in pixels of the same run without drawing it. */
int ao_text_width(const ao_font_t *f, const char *s, size_t len);

/* ------------------------------------------------------------ line breaking
 *
 * Greedy wrap with a breakpoint table: space, no-break space is NOT a break,
 * hyphen and em dash break AFTER the character, and the closing punctuation of
 * a quoted word (« » ” ’ . ,) does not start a line.  No hyphenation (§8).
 *
 * ao_text_fit() returns the byte length of the longest prefix of s[0..len)
 * that fits in max_w and ends at a break opportunity (or, when even one word
 * does not fit, the longest prefix that fits at any position, so a long word
 * fills the line instead of looping forever).  It never returns 0 for a
 * non-empty input: a single glyph wider than the line still consumes it.
 */
int  ao_text_is_break(uint32_t cp);
size_t ao_text_fit(const ao_font_t *f, const char *s, size_t len, int max_w,
                   int *width_out);

/* ------------------------------------------------------------------- stats */

/* Used for the receipt the integration gate prints: what a face actually
 * carries, so "we ship Cyrillic" is a number, not a claim. */
int ao_font_covers(const ao_font_t *f, uint32_t lo, uint32_t hi);
size_t ao_font_used_bytes(const ao_font_t *f);

#endif /* AURALITE_OFFICE_AOGLYPH_H */

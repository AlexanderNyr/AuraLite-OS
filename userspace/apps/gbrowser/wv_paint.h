/*
 * wv_paint.h — painting for the AuraLite web view (WEBVIEW_PLAN W4).
 *
 * Display list → pixels in the caller's buffer.  Everything is clipped to
 * the buffer, and boxes entirely outside the viewport are skipped — that
 * is the difference between scrolling a long page smoothly and not.
 *
 * Glyphs come from the project's PSF2 VGA 8×16 font for ASCII 0x00–0x7F
 * (the same blob the kernel console uses) plus a windows-1251 overlay
 * for 0x80–0xFF so Cyrillic text is a real letter, not a CP437 dingbat.
 * Synthesised bold (plan D7) draws each glyph twice, one pixel apart —
 * the honest 1980s approach.
 *
 * Scrolling (plan §1, 0.068 ms for 40 px) is a memmove of the retained
 * buffer plus a repaint of the exposed band only, via wv_paint_scroll() +
 * wv_paint_band().
 *
 * The hash gate: wv_paint_hash() is an FNV-1a over the whole buffer; the
 * unit test pins a fixed input to a fixed reference value, so a change in
 * rendering is a deliberate act with an updated expectation, not a
 * judgement call.
 */

#ifndef AURALITE_WV_PAINT_H
#define AURALITE_WV_PAINT_H

#include <stdint.h>
#include "wv_layout.h"

#define WV_FONT_W 8
#define WV_FONT_H 16
#define WV_MAX_IMG 8

typedef struct {
    const uint32_t *px;
    int w, h;
} wv_img_slot;

typedef struct {
    uint32_t *page;         /* caller's pixel buffer (XRGB8888) */
    int32_t   w, h;         /* buffer size in pixels */
    const uint8_t *glyphs;  /* 256 glyphs × 16 bytes, MSB-first rows */
    const wv_img_slot *imgs;
    int nimg;
} wv_paint_t;

void wv_paint_init(wv_paint_t *P, uint32_t *page, int32_t w, int32_t h);
void wv_paint_set_images(wv_paint_t *P, const wv_img_slot *imgs, int n);

/* Fill a rectangle, clipped to the buffer.  (x,y) is the top-left. */
void wv_paint_rect(wv_paint_t *P, int32_t x, int32_t y,
                   int32_t rw, int32_t rh, uint32_t color);

/* One glyph at (x, y) — top-left.  Clipped. */
void wv_paint_glyph(wv_paint_t *P, int32_t x, int32_t y,
                    unsigned char ch, uint32_t fg);

/* A text run at (x, y).  bold = double-strike; underline = bar at the
 * glyph baseline.  Clipped. */
void wv_paint_text(wv_paint_t *P, int32_t x, int32_t y,
                   const char *s, uint32_t len,
                   uint32_t fg, int bold, int underline);

/* Paint a whole display list with a vertical scroll offset: a box or
 * word whose page y (item y − scroll_y) falls entirely outside the buffer
 * is skipped.  Does NOT clear the buffer — the caller fills the paper. */
void wv_paint_run(wv_paint_t *P, const wv_layout_t *L, int32_t scroll_y);

/* Scroll the retained buffer by `delta` pixels (positive = content moves
 * up, i.e. scroll_y increased).  The exposed band is NOT painted — call
 * wv_paint_band() for it.  Returns the band's (top, height) via pointers. */
void wv_paint_scroll(wv_paint_t *P, int32_t delta,
                     int32_t *band_top, int32_t *band_h);

/* Repaint only a horizontal band of the display list (for scrolling). */
void wv_paint_band(wv_paint_t *P, const wv_layout_t *L, int32_t scroll_y,
                   int32_t band_top, int32_t band_h);

/* FNV-1a 32-bit hash of the whole buffer (the W4 reference gate). */
uint32_t wv_paint_hash(const uint32_t *page, int32_t w, int32_t h);

#endif /* AURALITE_WV_PAINT_H */

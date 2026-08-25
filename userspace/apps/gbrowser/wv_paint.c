/*
 * wv_paint.c — painting for the AuraLite web view (WEBVIEW_PLAN W4).
 *
 * See wv_paint.h for the design.  Implementation notes:
 *   - All drawing is clip-aware: a rect is intersected with the buffer,
 *     a glyph skips whole rows/columns outside it, a display-list run
 *     culls items whose page y is entirely off-screen before touching a
 *     single pixel.
 *   - The font is the kernel console's PSF2 VGA 8×16 blob, included as
 *     plain data (no kernel dependencies — it is just bytes).
 *   - Bold is a double-strike one pixel to the right; underline is a
 *     one-pixel bar at the glyph baseline (y + 15).
 */

#include "wv_paint.h"

/* Overlap-safe byte copy (freestanding builds have no guaranteed
 * memmove). */
static void wv_memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
}

/* The kernel console's VGA 8x16 PSF2 font, embedded as data. */
#include "../../../drivers/framebuffer/psf2_default_font.inc"
/* windows-1251 high half (0x80–0xFF) so Cyrillic text is a real letter. */
#include "wv_cp1251_font.inc"

static uint8_t wv_font_glyphs[256 * 16];
static int wv_font_ready;

void wv_paint_init(wv_paint_t *P, uint32_t *page, int32_t w, int32_t h) {
    P->page = page;
    P->w = w;
    P->h = h;
    if (!wv_font_ready) {
        const uint8_t *vga = psf2_default_font_data + 32;
        for (size_t i = 0; i < 128u * 16u; i++)
            wv_font_glyphs[i] = vga[i];
        for (size_t i = 0; i < 128u * 16u; i++)
            wv_font_glyphs[128u * 16u + i] = wv_cp1251_font[i];
        wv_font_ready = 1;
    }
    P->glyphs = wv_font_glyphs;
    P->imgs = 0;
    P->nimg = 0;
}

void wv_paint_set_images(wv_paint_t *P, const wv_img_slot *imgs, int n) {
    P->imgs = imgs;
    P->nimg = n;
}

static void wv_paint_image(wv_paint_t *P, const wv_disp_t *it, int32_t py) {
    int32_t dw = (int32_t)it->w, dh = (int32_t)it->h;
    if (dw <= 0 || dh <= 0) return;
    const uint32_t *src = NULL;
    int sw = 0, sh = 0;
    if (it->img_id > 0 && P->imgs && (int)it->img_id <= P->nimg) {
        const wv_img_slot *s = &P->imgs[it->img_id - 1];
        if (s->px && s->w > 0 && s->h > 0) {
            src = s->px; sw = s->w; sh = s->h;
        }
    }
    if (!src) {
        uint32_t fill = it->bg ? it->bg : 0x00E0E0E0u;
        wv_paint_rect(P, it->x, py, dw, dh, fill);
        if (it->border)
            wv_paint_rect(P, it->x, py, dw, 1, it->border_color);
        return;
    }
    int32_t row, col;
    for (row = 0; row < dh; row++) {
        int32_t sy = py + row;
        if (sy < 0 || sy >= P->h) continue;
        int srcy = row * sh / dh;
        if (srcy >= sh) srcy = sh - 1;
        uint32_t *line = P->page + (size_t)sy * P->w;
        for (col = 0; col < dw; col++) {
            int32_t sx = it->x + col;
            if (sx < 0 || sx >= P->w) continue;
            int srcx = col * sw / dw;
            if (srcx >= sw) srcx = sw - 1;
            line[sx] = src[(size_t)srcy * (size_t)sw + (size_t)srcx] | 0x00000000u;
        }
    }
}

void wv_paint_rect(wv_paint_t *P, int32_t x, int32_t y,
                   int32_t rw, int32_t rh, uint32_t color) {
    if (!P->page || rw <= 0 || rh <= 0) return;
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int32_t x1 = x + rw; if (x1 > P->w) x1 = P->w;
    int32_t y1 = y + rh; if (y1 > P->h) y1 = P->h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int32_t yy = y0; yy < y1; yy++) {
        uint32_t *row = P->page + (size_t)yy * P->w;
        for (int32_t xx = x0; xx < x1; xx++) row[xx] = color;
    }
}

void wv_paint_glyph(wv_paint_t *P, int32_t x, int32_t y,
                    unsigned char ch, uint32_t fg) {
    if (!P->page || !P->glyphs) return;
    if (y + WV_FONT_H <= 0 || y >= P->h) return;      /* whole glyph off */
    if (x + WV_FONT_W <= 0 || x >= P->w) return;
    const uint8_t *g = P->glyphs + (size_t)ch * 16;
    for (int32_t row = 0; row < WV_FONT_H; row++) {
        int32_t py = y + row;
        if (py < 0 || py >= P->h) continue;
        uint8_t bits = g[row];
        uint32_t *line = P->page + (size_t)py * P->w;
        for (int32_t col = 0; col < WV_FONT_W; col++) {
            if (!(bits & (0x80u >> col))) continue;
            int32_t px = x + col;
            if (px < 0 || px >= P->w) continue;
            line[px] = fg;
        }
    }
}

void wv_paint_text(wv_paint_t *P, int32_t x, int32_t y,
                   const char *s, uint32_t len,
                   uint32_t fg, int bold, int underline) {
    if (!P->page || !s) return;
    int32_t cx = x;
    for (uint32_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        wv_paint_glyph(P, cx, y, ch, fg);
        if (bold)
            wv_paint_glyph(P, cx + 1, y, ch, fg);
        if (underline) {
            /* bar at the glyph baseline: y + 15 */
            int32_t by = y + 15;
            if (by >= 0 && by < P->h) {
                uint32_t *line = P->page + (size_t)by * P->w;
                for (int32_t k = 0; k < WV_FONT_W; k++) {
                    int32_t px = cx + k;
                    if (px >= 0 && px < P->w) line[px] = fg;
                }
            }
        }
        cx += WV_FONT_W;
    }
}

/* Border: four bars around a box, clipped to [y0, y1) vertically (used by
 * both the full run and the band repaint). */
static void wv_paint_border(wv_paint_t *P, int32_t x, int32_t y,
                            int32_t w, int32_t h, int32_t bw,
                            uint32_t color, int32_t y0, int32_t y1) {
    if (bw <= 0 || w <= 0 || h <= 0) return;
    if (y + h <= y0 || y >= y1) return;
    int32_t top = y < y0 ? y0 : y;
    int32_t bot = y + h > y1 ? y1 : y + h;
    if (top >= bot) return;
    /* top bar */
    wv_paint_rect(P, x, top, w, top < y + bw ? y + bw - top : 0, color);
    /* bottom bar */
    if (y + h - bw < bot)
        wv_paint_rect(P, x, y + h - bw, w, bot - (y + h - bw), color);
    /* left bar (excluding the corners already drawn) */
    int32_t ly0 = y + bw < top ? top : y + bw;
    int32_t ly1 = y + h - bw > bot ? bot : y + h - bw;
    if (ly0 < ly1)
        wv_paint_rect(P, x, ly0, bw, ly1 - ly0, color);
    /* right bar */
    if (ly0 < ly1)
        wv_paint_rect(P, x + w - bw, ly0, bw, ly1 - ly0, color);
}

void wv_paint_run(wv_paint_t *P, const wv_layout_t *L, int32_t scroll_y) {
    if (!P->page || !L || !L->items) return;
    for (size_t i = 0; i < L->item_count; i++) {
        const wv_disp_t *it = &L->items[i];
        int32_t py = it->y - scroll_y;
        if (it->type == WV_D_IMAGE) {
            if (py + (int32_t)it->h <= 0 || py >= P->h) continue;
            wv_paint_image(P, it, py);
        } else if (it->type == WV_D_BOX) {
            if (py + (int32_t)it->h <= 0 || py >= P->h) continue;
            if (it->bg)
                wv_paint_rect(P, it->x, py, (int32_t)it->w, (int32_t)it->h, it->bg);
            if (it->border)
                wv_paint_border(P, it->x, py, (int32_t)it->w, (int32_t)it->h,
                                it->border, it->border_color, 0, P->h);
        } else { /* WV_D_TEXT */
            if (py + WV_FONT_H <= 0 || py >= P->h) continue;   /* cull */
            const char *s = wv_layout_str(L, it->text_off);
            wv_paint_text(P, it->x, py, s, it->text_len, it->fg,
                          it->bold, it->underline);
        }
    }
}

void wv_paint_scroll(wv_paint_t *P, int32_t delta,
                     int32_t *band_top, int32_t *band_h) {
    *band_top = 0;
    *band_h = 0;
    if (!P->page || delta == 0) return;
    int32_t step = delta < 0 ? -delta : delta;
    if (step >= P->h) {                 /* scrolled past the whole page */
        *band_top = 0;
        *band_h = P->h;
        return;
    }
    size_t bytes_row = (size_t)P->w * sizeof(uint32_t);
    if (delta > 0) {
        /* content moves up: rows [step..h) <- rows [0..h-step) */
        wv_memmove(P->page, P->page + (size_t)step * P->w,
                (size_t)(P->h - step) * bytes_row);
        *band_top = P->h - step;        /* bottom band exposed */
        *band_h = step;
    } else {
        /* content moves down: rows [0..h-step) <- rows [0..h-step) ... */
        wv_memmove(P->page + (size_t)step * P->w, P->page,
                (size_t)(P->h - step) * bytes_row);
        *band_top = 0;                  /* top band exposed */
        *band_h = step;
    }
}

void wv_paint_band(wv_paint_t *P, const wv_layout_t *L, int32_t scroll_y,
                   int32_t band_top, int32_t band_h) {
    if (!P->page || !L || !L->items || band_h <= 0) return;
    if (band_top < 0) band_top = 0;
    if (band_top >= P->h) return;
    if (band_top + band_h > P->h) band_h = P->h - band_top;

    /* Items whose page rect intersects [band_top, band_top+band_h) */
    for (size_t i = 0; i < L->item_count; i++) {
        const wv_disp_t *it = &L->items[i];
        int32_t py = it->y - scroll_y;
        int32_t ih = (it->type == WV_D_BOX || it->type == WV_D_IMAGE)
                         ? (int32_t)it->h : WV_FONT_H;
        if (py + ih <= band_top || py >= band_top + band_h) continue;
        if (it->type == WV_D_IMAGE) {
            wv_paint_image(P, it, py);
        } else if (it->type == WV_D_BOX) {
            /* CLIP the box to the band: its part above the band was
             * already painted by the retained buffer, and repainting it
             * would erase content drawn on top of it.  (A box fully
             * inside the band is unaffected by the clip.) */
            int32_t y0 = py < band_top ? band_top : py;
            int32_t y1 = py + ih;
            if (y1 > band_top + band_h) y1 = band_top + band_h;
            if (y0 < y1) {
                if (it->bg)
                    wv_paint_rect(P, it->x, y0, (int32_t)it->w, y1 - y0, it->bg);
                if (it->border)
                    wv_paint_border(P, it->x, py, (int32_t)it->w, ih,
                                    it->border, it->border_color,
                                    band_top, band_top + band_h);
            }
        } else {
            /* Text is drawn whole (glyphs may straddle the band edge —
             * exactly as the full repaint draws them). */
            const char *s = wv_layout_str(L, it->text_off);
            wv_paint_text(P, it->x, py, s, it->text_len, it->fg,
                          it->bold, it->underline);
        }
    }
}

uint32_t wv_paint_hash(const uint32_t *page, int32_t w, int32_t h) {
    /* FNV-1a 32-bit */
    uint32_t hash = 0x811C9DC5u;
    size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = page[i];
        hash ^= (v >> 24) & 0xFFu; hash *= 0x01000193u;
        hash ^= (v >> 16) & 0xFFu; hash *= 0x01000193u;
        hash ^= (v >> 8) & 0xFFu;  hash *= 0x01000193u;
        hash ^= v & 0xFFu;         hash *= 0x01000193u;
    }
    return hash;
}

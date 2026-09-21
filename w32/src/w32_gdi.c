/* w32_gdi.c — the GDI32 personality (W32APP_PLAN.md W32A-7).
 *
 * The DC model, the object table, the memory-DC raster engine, blitting
 * with raster ops, region clipping, font metrics against the shipped
 * PSF2 font, the 8-bit palette path, icon decode, and the printing
 * calls as named refusals.
 *
 * Architecture, in one paragraph: a DC is either a window slot (the
 * A-5 model -- a small integer naming a live compositor window), a
 * memory DC (an object-table entry owning a selected bitmap), or the
 * metrics-only screen DC (GetDC(NULL)).  All raster primitives run
 * through one surface writer that applies the DC's clip region; on a
 * memory DC it stores pixels directly, on a window DC it renders into
 * an ARGB temp and hands it to the compositor with one ag_blit_alpha
 * call -- so a shape drawn into a memory DC and onto a window produces
 * byte-identical pixels BY CONSTRUCTION (the fixture hashes both).
 * Fonts are the shipped PSF2 VGA 8x16 face (the very blob the kernel
 * console uses -- included, not copied), so every metric is the real
 * geometry and the host unit test asserts them against the file.
 *
 * Documented limitations (D9 -- named, not hidden):
 *   * Only the ROPs the plan lists are implemented; anything else
 *     refuses with ERROR_CALL_NOT_IMPLEMENTED at the call site.
 *   * Non-SRCCOPY blits onto a window DC and window-source blits do
 *     per-pixel readback through ag_get_pixel: correct, deliberately
 *     slow, and the kernel keeps owning the pixels.
 *   * Printing (StartDoc/StartPage/EndPage/EndDoc) fails cleanly:
 *     AuraLite has no printers, and the refusal is named.
 *   * Bitmap-only fonts: CreateFont* records the LOGFONT (GetObject
 *     returns it verbatim) and all metrics report the shipped 8x16
 *     geometry; "scaling is nearest" means the one size that exists.
 *   * One drawing deviation from Win32 is documented here: brush
 *     hatch/pattern gaps paint the DC background only when the DC's
 *     bk mode is OPAQUE (Win32 ties it to the brush's own DIB pattern
 *     background; the DC-wide rule is the honest subset).
 */

#include "w32/gdi32.h"
#include "w32/w32_errno.h"
#include "w32/w32_module.h"
#include "w32/w32_manifest.h"
#include "w32/kernel32.h"

#include <stdint.h>
#include <stddef.h>

#include <string.h>

#ifndef AURALITE_W32_HOST_TEST
#include "auragui.h"
#include <stdio.h>
#else
/* The host suite has no compositor and no auragui.h; it supplies these.
 * The compositor calls this file makes are mirrored with the same
 * signatures libauragui declares (user32_win.c keeps the window-family
 * half of that mirror block).  The theme is NOT mirrored: ag_theme_get
 * takes a struct whose host mirror type belongs to user32_win.c, and
 * re-declaring it here would give the amalgamated test TU two
 * conflicting declarations.  Instead the suite supplies the one field
 * a raster engine reads -- the DPI -- through a hook; the guest path is
 * the real ag_theme_get call below, and the integration fixture
 * (gtheme --dpi + reboot) exercises it end to end. */
int  ag_blit_alpha(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h,
                   const uint32_t *argb, uint32_t stride);
int  ag_fill_rect(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h,
                  uint32_t color);
int  ag_get_pixel(int wid, int32_t x, int32_t y);
uint32_t w32_gdi_host_dpi(void);          /* host suite: the theme's dpi */
#endif

/* The personality's link into the other halves; declared extern rather
 * than dragging private headers in (same convention as w32_dlg.c). */
extern int       w32_win_ag_wid(int win_index);       /* user32_win.c */
extern W32_DWORD w32_colorref_to_ag(W32_DWORD cr);    /* user32.c */
extern W32_DWORD w32_ag_to_colorref(W32_DWORD ag);    /* user32.c */
extern int       w32_hdc_win_index(W32_HDC hdc);      /* user32.c */

/* BS_* constants (gdi32.h deliberately carries only what callers pass). */
#define GDI_BS_SOLID    0
#define GDI_BS_NULL     5
#define GDI_BS_HATCHED  2
#define GDI_BS_PATTERN  3

/* ===================================================================== *
 * Object table                                                          *
 * ===================================================================== */

#define GDI_HANDLE_BIAS 0x6D700000u   /* far above the 0x00BBGGRR raw-
                                       * colour range A-5 brushes used */
#define GDI_OBJ_MAX     512

enum {
    GOBJ_PEN = 1, GOBJ_BRUSH, GOBJ_FONT, GOBJ_BITMAP,
    GOBJ_RGN, GOBJ_PALETTE, GOBJ_ICON, GOBJ_MEMDC,
};

typedef struct {
    uint32_t style;             /* PS_* */
    int32_t  width;
    uint32_t color;             /* COLORREF */
} gdi_pen_t;

typedef struct {
    uint32_t style;             /* BS_* */
    uint32_t color;             /* COLORREF (solid/hatched) */
    int32_t  hatch;             /* HS_* */
    uint8_t  pat[8];            /* pattern brush: 8x8 1-bpp rows, MSB first */
} gdi_brush_t;

typedef struct {
    W32_LOGFONTW lf;            /* the request, verbatim */
} gdi_font_t;

#define GDI_CT_MAX 256
typedef struct {
    int32_t  w, h;
    int      bpp;               /* 8, 24, 32 */
    int      is_dib;            /* 1: rows are in DIB format/order */
    int      top_down;          /* DIB rows top-down (biHeight < 0) */
    uint32_t stride;            /* bytes per row */
    uint8_t *bits;              /* pool allocation (DIB sections share it
                                 * with the caller -- that is the contract) */
    size_t   alloc;
    int      ct_n;              /* color-table size (8bpp DIBs) */
    W32_RGBQUAD ct[GDI_CT_MAX];
} gdi_bitmap_t;

#define GDI_RGN_MAX 32
typedef struct {
    int      n;
    W32_RECT r[GDI_RGN_MAX];
} gdi_rgn_t;

typedef struct {
    int              n;
    int              realized;
    W32_PALETTEENTRY e[GDI_CT_MAX];
} gdi_pal_t;

typedef struct {
    int32_t   w, h;
    uint32_t *argb;             /* pool allocation, 0xAARRGGBB rows */
    size_t    alloc;
} gdi_icon_t;

/* Full DC state; window slots, memory DCs and the screen DC all use it. */
#define GDI_SAVE_MAX 8
typedef struct {
    uint32_t text_color, bk_color, bk_mode, rop2, text_align;
    int32_t  map_mode, worg_x, worg_y, borg_x, borg_y, cur_x, cur_y;
    uint32_t pen, brush, font, pal;
    int      clip_n;
    W32_RECT clip[GDI_RGN_MAX];
} gdi_save_t;

typedef struct gdi_dc {
    int      valid;
    int      kind;              /* 0 window, 1 memory, 2 screen */
    int      slot;              /* window index (kind 0) */
    uint32_t text_color, bk_color;      /* COLORREF */
    uint32_t bk_mode;                   /* TRANSPARENT/OPAQUE */
    uint32_t rop2;
    uint32_t text_align;
    int32_t  map_mode;
    int32_t  worg_x, worg_y, borg_x, borg_y;
    int32_t  cur_x, cur_y;
    uint32_t pen, brush, font, pal;     /* table handles; 0 = stock */
    uint32_t bmp;                       /* memory DCs: selected bitmap */
    int      clip_n;                    /* < 0 = unbounded */
    W32_RECT clip[GDI_RGN_MAX];
    int      sp;
    gdi_save_t save[GDI_SAVE_MAX];
} gdi_dc_t;

typedef struct {
    int       used;
    int       type;
    union {
        gdi_pen_t    pen;
        gdi_brush_t  brush;
        gdi_font_t   font;
        gdi_bitmap_t bmp;
        gdi_rgn_t    rgn;
        gdi_pal_t    pal;
        gdi_icon_t   icon;
        gdi_dc_t     dc;
    } u;
} gdi_obj_t;

static gdi_obj_t gobjs[GDI_OBJ_MAX];

static uint32_t h_from_idx(int i) { return GDI_HANDLE_BIAS + (uint32_t)i; }
static int idx_from_h(void *h) {
    uintptr_t v = (uintptr_t)h;
    if (v < GDI_HANDLE_BIAS) return -1;
    uintptr_t i = v - GDI_HANDLE_BIAS;
    if (i >= GDI_OBJ_MAX) return -1;
    return gobjs[i].used ? (int)i : -1;
}
static gdi_obj_t *obj_from_h(void *h, int type) {
    int i = idx_from_h(h);
    if (i < 0 || gobjs[i].type != type) return 0;
    return &gobjs[i];
}

static void *gdi_alloc_obj(int type, gdi_obj_t **out) {
    for (int i = 0; i < GDI_OBJ_MAX; i++) {
        if (!gobjs[i].used) {
            memset(&gobjs[i], 0, sizeof gobjs[i]);
            gobjs[i].used = 1;
            gobjs[i].type = type;
            if (out) *out = &gobjs[i];
            return (void *)(uintptr_t)h_from_idx(i);
        }
    }
    return 0;
}

static void *handle_of_obj(gdi_obj_t *o) {
    int idx = (int)(o - gobjs);
    return (void *)(uintptr_t)h_from_idx(idx);
}

/* ---- bitmap pixel pool -------------------------------------------------
 * A bump allocator with tail-trim free over a static arena.  Memory DCs,
 * DIB sections, window-raster temps and decoded icons draw from it;
 * DeleteObject frees.  The interior-free leak is bounded by the arena
 * and per-process (GDI objects in this OS are per-process anyway). */
#define GDI_POOL_SIZE (2u * 1024u * 1024u)
static uint8_t  gdi_pool[GDI_POOL_SIZE] __attribute__((aligned(16)));
static size_t   gdi_pool_used;

static void *pool_alloc(size_t n) {
    n = (n + 15u) & ~(size_t)15u;
    if (n == 0) n = 16;
    if (gdi_pool_used + n > GDI_POOL_SIZE) return 0;
    void *p = &gdi_pool[gdi_pool_used];
    gdi_pool_used += n;
    return p;
}
static void pool_free(void *p, size_t n) {
    if (!p) return;
    n = (n + 15u) & ~(size_t)15u;
    if ((uint8_t *)p + n == &gdi_pool[gdi_pool_used]) gdi_pool_used -= n;
}

/* ===================================================================== *
 * The shipped font (the same PSF2 blob the kernel console uses)         *
 * ===================================================================== */
#include "../../drivers/framebuffer/psf2_default_font.inc"

typedef struct {
    uint32_t w, h, ascent, descent, glyphs, bytes_per_glyph, bytes_per_row;
    const uint8_t *data;
} gdi_font_face_t;

static gdi_font_face_t face;
static int face_parsed;

static const gdi_font_face_t *gdi_face(void) {
    if (!face_parsed) {
        face_parsed = 1;
        const uint8_t *b = psf2_default_font_data;
        if (b[0] == 0x72 && b[1] == 0xB5 && b[2] == 0x4A && b[3] == 0x86) {
            uint32_t hdr = (uint32_t)b[8] | ((uint32_t)b[9] << 8) |
                           ((uint32_t)b[10] << 16) | ((uint32_t)b[11] << 24);
            uint32_t ng  = (uint32_t)b[16] | ((uint32_t)b[17] << 8) |
                           ((uint32_t)b[18] << 16) | ((uint32_t)b[19] << 24);
            uint32_t bpg = (uint32_t)b[20] | ((uint32_t)b[21] << 8) |
                           ((uint32_t)b[22] << 16) | ((uint32_t)b[23] << 24);
            uint32_t hh  = (uint32_t)b[24] | ((uint32_t)b[25] << 8) |
                           ((uint32_t)b[26] << 16) | ((uint32_t)b[27] << 24);
            uint32_t ww  = (uint32_t)b[28] | ((uint32_t)b[29] << 8) |
                           ((uint32_t)b[30] << 16) | ((uint32_t)b[31] << 24);
            if (hdr <= PSF2_DEFAULT_FONT_SIZE && ng && bpg && ww && hh &&
                (uint64_t)hdr + (uint64_t)ng * bpg <= PSF2_DEFAULT_FONT_SIZE) {
                face.w = ww; face.h = hh; face.glyphs = ng;
                face.bytes_per_glyph = bpg;
                face.bytes_per_row = (ww + 7) / 8;
                face.ascent = hh - 2; face.descent = 2;   /* VGA descender rows */
                face.data = b + hdr;
            }
        }
    }
    return face.data ? &face : 0;
}

/* The face names the enumeration reports: this IS the shipped set. */
static const uint16_t face_name_w[]   = {'V','G','A',' ','8','x','1','6',0};
static const uint16_t face_style_w[]  = {'R','e','g','u','l','a','r',0};

/* ===================================================================== *
 * DCs                                                                   *
 * ===================================================================== */

#define GDI_WIN_DC_MAX 32
static gdi_dc_t win_dcs[GDI_WIN_DC_MAX];
static gdi_dc_t screen_dc;

static void dc_defaults(gdi_dc_t *d, int kind, int slot) {
    memset(d, 0, sizeof *d);
    d->valid = 1;
    d->kind = kind;
    d->slot = slot;
    d->text_color = 0x00000000u;        /* black */
    d->bk_color   = 0x00FFFFFFu;        /* white */
    d->bk_mode    = W32_OPAQUE;
    d->rop2       = W32_R2_COPYPEN;
    d->text_align = W32_TA_LEFT | W32_TA_TOP;
    d->map_mode   = W32_MM_TEXT;
    d->clip_n     = -1;                 /* unbounded */
}

void w32_gdi_reset_win_dc(int win_index) {
    if (win_index >= 0 && win_index < GDI_WIN_DC_MAX)
        dc_defaults(&win_dcs[win_index], 0, win_index);
}

#define GDI_SCREEN_SENTINEL (GDI_HANDLE_BIAS + (uint32_t)GDI_OBJ_MAX)

W32_HDC w32_gdi_screen_dc(void) {
    if (!screen_dc.valid) dc_defaults(&screen_dc, 2, -1);
    return (W32_HDC)(uintptr_t)GDI_SCREEN_SENTINEL;
}

static gdi_dc_t *dc_from_h(W32_HDC hdc) {
    if ((uintptr_t)hdc == GDI_SCREEN_SENTINEL) return &screen_dc;
    int wi = w32_hdc_win_index(hdc);
    if (wi >= 0) {
        gdi_dc_t *d = &win_dcs[wi];
        if (!d->valid) dc_defaults(d, 0, wi);
        return d;
    }
    gdi_obj_t *o = obj_from_h(hdc, GOBJ_MEMDC);
    return o ? &o->u.dc : 0;
}

static gdi_bitmap_t *dc_bmp(gdi_dc_t *d) {
    if (!d || d->kind != 1 || !d->bmp) return 0;
    gdi_obj_t *o = obj_from_h((void *)(uintptr_t)d->bmp, GOBJ_BITMAP);
    return o ? &o->u.bmp : 0;
}

static int clip_has(gdi_dc_t *d, int32_t x, int32_t y) {
    if (!d || d->clip_n < 0) return 1;
    for (int i = 0; i < d->clip_n; i++)
        if (x >= d->clip[i].left && x < d->clip[i].right &&
            y >= d->clip[i].top && y < d->clip[i].bottom) return 1;
    return 0;
}
static int clip_intersects(gdi_dc_t *d, const W32_RECT *r) {
    if (!d || d->clip_n < 0) return 1;
    for (int i = 0; i < d->clip_n; i++)
        if (r->left < d->clip[i].right && r->right > d->clip[i].left &&
            r->top < d->clip[i].bottom && r->bottom > d->clip[i].top) return 1;
    return 0;
}

/* ---- stocks ----------------------------------------------------------- */
static gdi_brush_t stock_brushes[6];    /* ids 0..5: WHITE..NULL */
static gdi_pen_t   stock_pens[3];       /* WHITE_PEN, BLACK_PEN, NULL_PEN */
static gdi_font_t  stock_font;
static int stocks_ready;

static void ensure_stocks(void) {
    if (stocks_ready) return;
    stocks_ready = 1;
    stock_brushes[0].style = GDI_BS_SOLID; stock_brushes[0].color = 0x00FFFFFFu;
    stock_brushes[1].style = GDI_BS_SOLID; stock_brushes[1].color = 0x00C0C0C0u;
    stock_brushes[2].style = GDI_BS_SOLID; stock_brushes[2].color = 0x00808080u;
    stock_brushes[3].style = GDI_BS_SOLID; stock_brushes[3].color = 0x00404040u;
    stock_brushes[4].style = GDI_BS_SOLID; stock_brushes[4].color = 0x00000000u;
    stock_brushes[5].style = GDI_BS_NULL;
    stock_pens[0].style = 0; stock_pens[0].width = 1; stock_pens[0].color = 0x00FFFFFFu;
    stock_pens[1].style = 0; stock_pens[1].width = 1; stock_pens[1].color = 0x00000000u;
    stock_pens[2].style = W32_PS_NULL;
    memset(&stock_font, 0, sizeof stock_font);
    stock_font.lf.lfHeight = 16;
    stock_font.lf.lfWeight = 400;
    stock_font.lf.lfCharSet = W32_OEM_CHARSET;
    stock_font.lf.lfPitchAndFamily = 0x01;      /* FIXED_PITCH */
    for (int i = 0; i < 8 && face_name_w[i]; i++)
        stock_font.lf.lfFaceName[i] = face_name_w[i];
}

static gdi_brush_t *dc_brush(gdi_dc_t *d) {
    ensure_stocks();
    if (d->brush) {
        gdi_obj_t *o = obj_from_h((void *)(uintptr_t)d->brush, GOBJ_BRUSH);
        if (o) return &o->u.brush;
    }
    return &stock_brushes[W32_BLACK_BRUSH];
}
static gdi_pen_t *dc_pen(gdi_dc_t *d) {
    ensure_stocks();
    if (d->pen) {
        gdi_obj_t *o = obj_from_h((void *)(uintptr_t)d->pen, GOBJ_PEN);
        if (o) return &o->u.pen;
    }
    return &stock_pens[1];
}
static gdi_font_t *dc_font(gdi_dc_t *d) {
    ensure_stocks();
    if (d->font) {
        gdi_obj_t *o = obj_from_h((void *)(uintptr_t)d->font, GOBJ_FONT);
        if (o) return &o->u.font;
    }
    return &stock_font;
}

/* ===================================================================== *
 * Surface writer: the one raster path for both DC kinds                 *
 * ===================================================================== *
 * is_mem: store into the bitmap (bounds + clip apply).
 * window: store ARGB into the temp (alpha 255 forced); the temp starts
 *         fully transparent and ag_blit_alpha composites it, so gaps
 *         show the window through exactly like a memory DC shows the
 *         bitmap through.                                               */
typedef struct {
    gdi_dc_t     *dc;
    int           is_mem;
    gdi_bitmap_t *bmp;
    uint32_t     *argb;
    int32_t       w, h;
    int32_t       ox, oy;
    int           win_wid;
} surf_t;

static void surf_put(surf_t *s, int32_t x, int32_t y, uint32_t ag_color) {
    if (!clip_has(s->dc, x, y)) return;
    if (s->is_mem) {
        gdi_bitmap_t *b = s->bmp;
        if (x < 0 || y < 0 || x >= b->w || y >= b->h) return;
        if (b->bpp == 32 && !b->is_dib) {
            ((uint32_t *)(void *)b->bits)[y * (int32_t)(b->stride / 4) + x] = ag_color;
        } else {
            uint32_t rrow = (uint32_t)(b->top_down ? y : (b->h - 1 - y));
            uint8_t *row = b->bits + rrow * b->stride;
            if (b->bpp == 32) {
                uint8_t *p = row + 4u * (uint32_t)x;
                p[0] = (uint8_t)(ag_color & 0xFF);
                p[1] = (uint8_t)((ag_color >> 8) & 0xFF);
                p[2] = (uint8_t)((ag_color >> 16) & 0xFF);
                p[3] = 255;
            } else if (b->bpp == 24) {
                uint8_t *p = row + 3u * (uint32_t)x;
                p[0] = (uint8_t)(ag_color & 0xFF);
                p[1] = (uint8_t)((ag_color >> 8) & 0xFF);
                p[2] = (uint8_t)((ag_color >> 16) & 0xFF);
            } else if (b->bpp == 8) {
                int best = 0; long bd = 0x7FFFFFFF;
                for (int i = 0; i < b->ct_n && i < GDI_CT_MAX; i++) {
                    long dr = (long)b->ct[i].red   - (long)((ag_color >> 16) & 0xFF);
                    long dg = (long)b->ct[i].green - (long)((ag_color >> 8) & 0xFF);
                    long db = (long)b->ct[i].blue  - (long)(ag_color & 0xFF);
                    long d = dr * dr + dg * dg + db * db;
                    if (d < bd) { bd = d; best = i; }
                }
                row[x] = (uint8_t)best;
            }
        }
    } else {
        int32_t lx = x - s->ox, ly = y - s->oy;
        if (lx < 0 || ly < 0 || lx >= s->w || ly >= s->h) return;
        s->argb[ly * s->w + lx] = ag_color | 0xFF000000u;
    }
}

static uint32_t surf_get(surf_t *s, int32_t x, int32_t y) {
    if (s->is_mem) {
        gdi_bitmap_t *b = s->bmp;
        if (x < 0 || y < 0 || x >= b->w || y >= b->h) return 0;
        if (b->bpp == 32 && !b->is_dib)
            return ((uint32_t *)(void *)b->bits)[y * (int32_t)(b->stride / 4) + x];
        uint32_t rrow = (uint32_t)(b->top_down ? y : (b->h - 1 - y));
        uint8_t *row = b->bits + rrow * b->stride;
        if (b->bpp == 32) {
            uint8_t *p = row + 4u * (uint32_t)x;
            return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
        }
        if (b->bpp == 24) {
            uint8_t *p = row + 3u * (uint32_t)x;
            return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
        }
        if (b->bpp == 8) {
            int i = row[x];
            if (i < b->ct_n && i < GDI_CT_MAX)
                return ((uint32_t)b->ct[i].red << 16) |
                       ((uint32_t)b->ct[i].green << 8) | b->ct[i].blue;
        }
        return 0;
    }
    int32_t lx = x - s->ox, ly = y - s->oy;
    if (lx < 0 || ly < 0 || lx >= s->w || ly >= s->h) return 0;
    return s->argb[ly * s->w + lx] & 0x00FFFFFFu;
}

static int surf_of_dc(gdi_dc_t *d, surf_t *s) {
    memset(s, 0, sizeof *s);
    gdi_bitmap_t *b = dc_bmp(d);
    if (!b) return -1;
    s->dc = d; s->is_mem = 1; s->bmp = b;
    return 0;
}

/* Window-DC rendering: raster into an ARGB temp, then one blit. */
typedef void (*surf_draw_fn)(surf_t *s, void *user);

static int win_raster_blit(gdi_dc_t *d, int32_t bx, int32_t by,
                           int32_t bw, int32_t bh, surf_draw_fn fn, void *user) {
    if (bw <= 0 || bh <= 0) return 1;
    int wid = w32_win_ag_wid(d->slot);
    if (wid < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    size_t want = (size_t)bw * (size_t)bh * 4u;
    uint32_t *tmp = (uint32_t *)pool_alloc(want);
    if (!tmp) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    memset(tmp, 0, want);
    surf_t s;
    memset(&s, 0, sizeof s);
    s.dc = d; s.is_mem = 0; s.argb = tmp;
    s.w = bw; s.h = bh; s.ox = bx; s.oy = by; s.win_wid = wid;
    fn(&s, user);
    ag_blit_alpha(wid, bx, by, (uint32_t)bw, (uint32_t)bh,
                  (const uint32_t *)tmp, (uint32_t)bw);
    pool_free(tmp, want);
    return 1;
}

/* ---- brush sampling --------------------------------------------------- */
static const uint8_t hatch_rows[6][8] = {
    {0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24},   /* HORIZONTAL */
    {0x99,0x99,0x99,0x99,0x99,0x99,0x99,0x99},   /* VERTICAL   */
    {0x81,0x42,0x24,0x18,0x18,0x24,0x42,0x81},   /* FDIAGONAL  */
    {0x18,0x24,0x42,0x81,0x81,0x42,0x24,0x18},   /* BDIAGONAL  */
    {0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C},   /* CROSS      */
    {0x99,0x42,0x24,0x99,0x99,0x24,0x42,0x99},   /* DIAGCROSS  */
};

#define GDI_BRUSH_SKIP 0x80000000u

static uint32_t brush_color_at(gdi_dc_t *d, gdi_brush_t *br, int32_t x, int32_t y) {
    if (br->style == GDI_BS_NULL) return GDI_BRUSH_SKIP;
    if (br->style == GDI_BS_SOLID) return w32_colorref_to_ag(br->color);
    int32_t lx = x - d->borg_x, ly = y - d->borg_y;
    if (br->style == GDI_BS_PATTERN) {
        if (br->pat[ly & 7] & (0x80u >> (lx & 7)))
            return w32_colorref_to_ag(br->color);
    } else {                                        /* hatched */
        int hs = br->hatch;
        if (hs < 0 || hs > 5) hs = W32_HS_CROSS;
        if (hatch_rows[hs][ly & 7] & (0x80u >> (lx & 7)))
            return w32_colorref_to_ag(br->color);
    }
    if (d->bk_mode == W32_OPAQUE) return w32_colorref_to_ag(d->bk_color);
    return GDI_BRUSH_SKIP;
}

static void surf_span_brush(surf_t *s, int32_t x0, int32_t x1, int32_t y) {
    gdi_brush_t *br = dc_brush(s->dc);
    for (int32_t x = x0; x <= x1; x++) {
        uint32_t c = brush_color_at(s->dc, br, x, y);
        if (c == GDI_BRUSH_SKIP) continue;
        surf_put(s, x, y, c);
    }
}
static void surf_span_solid(surf_t *s, int32_t x0, int32_t x1, int32_t y, uint32_t ag) {
    for (int32_t x = x0; x <= x1; x++) surf_put(s, x, y, ag);
}

static void surf_line_pen(surf_t *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    gdi_pen_t *p = dc_pen(s->dc);
    if (p->style == W32_PS_NULL) return;
    uint32_t c = w32_colorref_to_ag(p->color);
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int32_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t sx = x1 >= x0 ? 1 : -1, sy = y1 >= y0 ? 1 : -1;
    int32_t err = dx - dy;
    int32_t x = x0, y = y0;
    int w = p->width > 0 ? p->width : 1;
    for (;;) {
        for (int a = 0; a < w; a++)
            for (int b = 0; b < w; b++)
                surf_put(s, x + a, y + b, c);
        if (x == x1 && y == y1) break;
        int32_t e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 <  dx) { err += dx; y += sy; }
    }
}

/* ---- geometry predicates ---------------------------------------------- */
static int in_ellipse(int32_t px, int32_t py, int32_t l, int32_t t, int32_t r, int32_t b) {
    if (r - l < 2 || b - t < 2) return 0;
    double cx = (l + r - 1) / 2.0, cy = (t + b - 1) / 2.0;
    double rx = (r - l) / 2.0,  ry = (b - t) / 2.0;
    if (rx <= 0 || ry <= 0) return 0;
    double dx = (px - cx) / (rx + 0.5), dy = (py - cy) / (ry + 0.5);
    return dx * dx + dy * dy <= 1.0;
}

static int in_roundrect(int32_t px, int32_t py, int32_t l, int32_t t,
                        int32_t r, int32_t b, int32_t ew, int32_t eh) {
    if (px < l || px >= r || py < t || py >= b) return 0;
    int32_t rx = ew / 2, ry = eh / 2;
    if (rx <= 0 || ry <= 0) return 1;
    int32_t xl = l + rx, xr = r - rx, yt = t + ry, yb = b - ry;
    if (px >= xl && px < xr) return 1;
    if (py >= yt && py < yb) return 1;
    int32_t cx = (px < xl) ? xl : xr;
    int32_t cy = (py < yt) ? yt : yb;
    double dx = (double)(px - cx) / (double)(rx + 0.5);
    double dy = (double)(py - cy) / (double)(ry + 0.5);
    return dx * dx + dy * dy <= 1.0;
}

/* ===================================================================== *
 * Shape raster bodies (run against a surf; shared by every DC kind)     *
 * ===================================================================== */

static void raster_rect(surf_t *s, int32_t l, int32_t t, int32_t r, int32_t b) {
    if (r <= l || b <= t) return;
    for (int32_t y = t; y < b; y++) surf_span_brush(s, l, r - 1, y);
    gdi_pen_t *p = dc_pen(s->dc);
    if (p->style != W32_PS_NULL) {
        int w = p->width > 0 ? p->width : 1;
        uint32_t c = w32_colorref_to_ag(p->color);
        for (int k = 0; k < w; k++) {
            if (t + k < b - 1 - k) surf_span_solid(s, l, r - 1, t + k, c);
            if (b - 1 - k > t + k) surf_span_solid(s, l, r - 1, b - 1 - k, c);
            for (int32_t y = t; y < b; y++) {
                if (l + k <= r - 1) surf_put(s, l + k, y, c);
                if (r - 1 - k >= l) surf_put(s, r - 1 - k, y, c);
            }
        }
    }
}

/* interior test = inside but not on the boundary (a neighbour outside) */
static int ellipse_edge(int32_t x, int32_t y, int32_t l, int32_t t, int32_t r, int32_t b) {
    return in_ellipse(x, y, l, t, r, b) &&
           (!in_ellipse(x - 1, y, l, t, r, b) || !in_ellipse(x + 1, y, l, t, r, b) ||
            !in_ellipse(x, y - 1, l, t, r, b) || !in_ellipse(x, y + 1, l, t, r, b));
}

static void raster_ellipse(surf_t *s, int32_t l, int32_t t, int32_t r, int32_t b) {
    if (r <= l || b <= t) return;
    gdi_pen_t *p = dc_pen(s->dc);
    for (int32_t y = t; y < b; y++)
        for (int32_t x = l; x < r; x++) {
            if (!in_ellipse(x, y, l, t, r, b)) continue;
            if (ellipse_edge(x, y, l, t, r, b)) {
                if (p->style != W32_PS_NULL)
                    surf_put(s, x, y, w32_colorref_to_ag(p->color));
            } else {
                surf_span_brush(s, x, x, y);
            }
        }
}

static int rrect_edge(int32_t x, int32_t y, int32_t l, int32_t t,
                      int32_t r, int32_t b, int32_t ew, int32_t eh) {
    return in_roundrect(x, y, l, t, r, b, ew, eh) &&
           (!in_roundrect(x - 1, y, l, t, r, b, ew, eh) ||
            !in_roundrect(x + 1, y, l, t, r, b, ew, eh) ||
            !in_roundrect(x, y - 1, l, t, r, b, ew, eh) ||
            !in_roundrect(x, y + 1, l, t, r, b, ew, eh));
}

static void raster_roundrect(surf_t *s, int32_t l, int32_t t, int32_t r, int32_t b,
                             int32_t ew, int32_t eh) {
    if (r <= l || b <= t) return;
    gdi_pen_t *p = dc_pen(s->dc);
    for (int32_t y = t; y < b; y++)
        for (int32_t x = l; x < r; x++) {
            if (!in_roundrect(x, y, l, t, r, b, ew, eh)) continue;
            if (rrect_edge(x, y, l, t, r, b, ew, eh)) {
                if (p->style != W32_PS_NULL)
                    surf_put(s, x, y, w32_colorref_to_ag(p->color));
            } else {
                surf_span_brush(s, x, x, y);
            }
        }
}

static void raster_polygon(surf_t *s, const W32_POINT *pts, int n, int outline) {
    if (n < 2) return;
    int32_t miny = pts[0].y, maxy = pts[0].y;
    for (int i = 1; i < n; i++) {
        if (pts[i].y < miny) miny = pts[i].y;
        if (pts[i].y > maxy) maxy = pts[i].y;
    }
    for (int32_t y = miny; y <= maxy; y++) {
        int32_t xs[64]; int nx = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            int32_t yi = pts[i].y, yj = pts[j].y;
            if ((yi > y) != (yj > y)) {
                int32_t xint = pts[j].x + (y - yj) * (pts[i].x - pts[j].x) / (yi - yj);
                if (nx < 64) xs[nx++] = xint;
            }
        }
        for (int a = 1; a < nx; a++) {
            int32_t v = xs[a]; int b2 = a - 1;
            while (b2 >= 0 && xs[b2] > v) { xs[b2 + 1] = xs[b2]; b2--; }
            xs[b2 + 1] = v;
        }
        for (int a = 0; a + 1 < nx; a += 2)
            surf_span_brush(s, xs[a], xs[a + 1], y);
    }
    if (outline)
        for (int i = 0, j = n - 1; i < n; j = i++)
            surf_line_pen(s, pts[j].x, pts[j].y, pts[i].x, pts[i].y);
}

/* ---- glyph rendering --------------------------------------------------- */
static void raster_glyph(surf_t *s, int32_t x, int32_t y, uint8_t ch,
                         uint32_t fg_ag, uint32_t bk_ag, int opaque) {
    const gdi_font_face_t *f = gdi_face();
    if (!f) return;
    if (ch >= f->glyphs) ch = 0;
    const uint8_t *g = f->data + (size_t)ch * f->bytes_per_glyph;
    for (uint32_t ry = 0; ry < f->h; ry++) {
        const uint8_t *rowb = g + ry * f->bytes_per_row;
        for (uint32_t rx = 0; rx < f->w; rx++) {
            int on = rowb[rx >> 3] & (0x80u >> (rx & 7));
            if (on) surf_put(s, x + (int32_t)rx, y + (int32_t)ry, fg_ag);
            else if (opaque) surf_put(s, x + (int32_t)rx, y + (int32_t)ry, bk_ag);
        }
    }
}

static void text_origin(gdi_dc_t *d, int32_t *x, int32_t *y, int32_t w) {
    const gdi_font_face_t *f = gdi_face();
    int32_t h = f ? (int32_t)f->h : 16;
    /* Alignment is two bit FIELDS, not flags: X is bits 1-2 (LEFT=0,
     * RIGHT=2, CENTER=6 -- CENTER contains RIGHT's bit, so an & test
     * would send every RIGHT-aligned string down the CENTER path), Y is
     * bits 3-4 (TOP=0, BOTTOM=8, BASELINE=24).  Compare the masked
     * field, never the raw bits. */
    uint32_t xa = d->text_align & 6u;
    if (xa == W32_TA_CENTER)      *x -= w / 2;
    else if (xa == W32_TA_RIGHT)  *x -= w;
    uint32_t ya = d->text_align & 24u;
    if (ya == W32_TA_BASELINE)    *y -= (f ? (int32_t)f->ascent : 14);
    else if (ya == W32_TA_BOTTOM) *y -= h;
}

static void raster_text(surf_t *s, int32_t x, int32_t y,
                        const uint16_t *str, uint32_t n, const int32_t *dx,
                        const W32_RECT *clip_r) {
    const gdi_font_face_t *f = gdi_face();
    if (!f) return;
    uint32_t fg = w32_colorref_to_ag(s->dc->text_color);
    uint32_t bk = w32_colorref_to_ag(s->dc->bk_color);
    int opaque = (s->dc->bk_mode == W32_OPAQUE);
    int32_t w = (int32_t)(n * f->w);
    text_origin(s->dc, &x, &y, w);
    int32_t cx = x;
    for (uint32_t i = 0; i < n; i++) {
        int32_t adv = dx ? dx[i] : (int32_t)f->w;
        if (!clip_r ||
            (cx < clip_r->right && cx + (int32_t)f->w > clip_r->left &&
             y < clip_r->bottom && y + (int32_t)f->h > clip_r->top))
            raster_glyph(s, cx, y, (uint8_t)str[i], fg, bk, opaque);
        cx += adv;
    }
}

/* ===================================================================== *
 * DC lifecycle + attribute APIs                                         *
 * ===================================================================== */

static gdi_bitmap_t *bmp_create_internal(int32_t w, int32_t h, int bpp,
                                         int is_dib, int top_down,
                                         const W32_RGBQUAD *ct, int ct_n) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    uint32_t stride = (uint32_t)(((size_t)w * (size_t)bpp + 31u) / 32u * 4u);
    size_t alloc = (size_t)stride * (size_t)h;
    uint8_t *bits = (uint8_t *)pool_alloc(alloc);
    if (!bits) return 0;
    memset(bits, 0, alloc);
    gdi_obj_t *o;
    if (!gdi_alloc_obj(GOBJ_BITMAP, &o)) { pool_free(bits, alloc); return 0; }
    gdi_bitmap_t *b = &o->u.bmp;
    b->w = w; b->h = h; b->bpp = bpp;
    b->is_dib = is_dib; b->top_down = top_down;
    b->stride = stride; b->bits = bits; b->alloc = alloc;
    if (ct && ct_n > 0) {
        if (ct_n > GDI_CT_MAX) ct_n = GDI_CT_MAX;
        memcpy(b->ct, ct, (size_t)ct_n * sizeof(W32_RGBQUAD));
        b->ct_n = ct_n;
    }
    return b;
}

static void *handle_of_bmp(gdi_bitmap_t *b) {
    gdi_obj_t *o = (gdi_obj_t *)((char *)b - offsetof(gdi_obj_t, u.bmp));
    return handle_of_obj(o);
}

W32ABI W32_HDC CreateCompatibleDC(W32_HDC hdc) {
    (void)hdc;
    gdi_obj_t *o;
    void *h = gdi_alloc_obj(GOBJ_MEMDC, &o);
    if (!h) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    dc_defaults(&o->u.dc, 1, -1);
    /* Win32 hands a memory DC a 1x1 monochrome bitmap; we hand it a 1x1
     * 32bpp one -- same contract (drawable, replaceable), honest format. */
    gdi_bitmap_t *b = bmp_create_internal(1, 1, 32, 0, 0, 0, 0);
    if (b) o->u.dc.bmp = (uint32_t)(uintptr_t)handle_of_bmp(b);
    return (W32_HDC)h;
}

W32ABI W32_BOOL DeleteDC(W32_HDC hdc) {
    if ((uintptr_t)hdc == GDI_SCREEN_SENTINEL) return 0;
    if (w32_hdc_win_index(hdc) >= 0) return 0;    /* common DCs: ReleaseDC */
    gdi_obj_t *o = obj_from_h(hdc, GOBJ_MEMDC);
    if (!o) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    o->used = 0;
    return 1;
}

W32ABI int32_t SaveDC(W32_HDC hdc) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d || d->kind == 2) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (d->sp >= GDI_SAVE_MAX) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    gdi_save_t *s = &d->save[d->sp];
    s->text_color = d->text_color; s->bk_color = d->bk_color;
    s->bk_mode = d->bk_mode; s->rop2 = d->rop2; s->text_align = d->text_align;
    s->map_mode = d->map_mode;
    s->worg_x = d->worg_x; s->worg_y = d->worg_y;
    s->borg_x = d->borg_x; s->borg_y = d->borg_y;
    s->cur_x = d->cur_x; s->cur_y = d->cur_y;
    s->pen = d->pen; s->brush = d->brush; s->font = d->font; s->pal = d->pal;
    s->clip_n = d->clip_n;
    memcpy(s->clip, d->clip, sizeof d->clip);
    d->sp++;
    return d->sp;                          /* the saved level, 1-based */
}

W32ABI W32_BOOL RestoreDC(W32_HDC hdc, int32_t saved) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d || d->kind == 2) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (saved > 0) {
        if (saved > d->sp) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
        d->sp = saved - 1;
    } else if (saved < 0) {
        if (-saved > d->sp) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
        d->sp += saved;
    } else return 0;
    if (d->sp < 0) d->sp = 0;
    gdi_save_t *s = &d->save[d->sp];
    d->text_color = s->text_color; d->bk_color = s->bk_color;
    d->bk_mode = s->bk_mode; d->rop2 = s->rop2; d->text_align = s->text_align;
    d->map_mode = s->map_mode;
    d->worg_x = s->worg_x; d->worg_y = s->worg_y;
    d->borg_x = s->borg_x; d->borg_y = s->borg_y;
    d->cur_x = s->cur_x; d->cur_y = s->cur_y;
    d->pen = s->pen; d->brush = s->brush; d->font = s->font; d->pal = s->pal;
    d->clip_n = s->clip_n;
    memcpy(d->clip, s->clip, sizeof d->clip);
    return 1;
}

W32ABI W32_HGDIOBJ SelectObject(W32_HDC hdc, W32_HGDIOBJ obj) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (d->kind == 2) { w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED); return 0; }
    int i = idx_from_h(obj);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    uint32_t h = h_from_idx(i);
    uint32_t old = 0;
    switch (gobjs[i].type) {
    case GOBJ_PEN:     old = d->pen;   d->pen = h;   break;
    case GOBJ_BRUSH:   old = d->brush; d->brush = h; break;
    case GOBJ_FONT:    old = d->font;  d->font = h;  break;
    case GOBJ_PALETTE: old = d->pal;   d->pal = h;   break;
    case GOBJ_BITMAP:
        if (d->kind != 1) { w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED); return 0; }
        old = d->bmp; d->bmp = h; d->cur_x = d->cur_y = 0; break;
    case GOBJ_RGN:
        return (W32_HGDIOBJ)(intptr_t)SelectClipRgn(hdc, (W32_HRGN)obj);
    default:
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return (W32_HGDIOBJ)(uintptr_t)old;
}

W32ABI W32_HGDIOBJ GetStockObject(int32_t idx) {
    ensure_stocks();
    gdi_obj_t *o;
    switch (idx) {
    case W32_WHITE_BRUSH: case W32_LTGRAY_BRUSH: case W32_GRAY_BRUSH:
    case W32_DKGRAY_BRUSH: case W32_BLACK_BRUSH: case W32_NULL_BRUSH: {
        if (!gdi_alloc_obj(GOBJ_BRUSH, &o)) return 0;
        o->u.brush = stock_brushes[idx];
        return (W32_HGDIOBJ)handle_of_obj(o);
    }
    case W32_WHITE_PEN: case W32_BLACK_PEN: case W32_NULL_PEN: {
        if (!gdi_alloc_obj(GOBJ_PEN, &o)) return 0;
        o->u.pen = stock_pens[idx - W32_WHITE_PEN];
        return (W32_HGDIOBJ)handle_of_obj(o);
    }
    case W32_OEM_FIXED_FONT: case W32_ANSI_FIXED_FONT: case W32_ANSI_VAR_FONT:
    case W32_SYSTEM_FONT: case W32_DEVICE_DEFAULT_FONT:
    case W32_SYSTEM_FIXED_FONT: case W32_DEFAULT_GUI_FONT: {
        if (!gdi_alloc_obj(GOBJ_FONT, &o)) return 0;
        o->u.font = stock_font;
        return (W32_HGDIOBJ)handle_of_obj(o);
    }
    case W32_DEFAULT_PALETTE: {
        if (!gdi_alloc_obj(GOBJ_PALETTE, &o)) return 0;
        /* the honest default is the 16 EGA colours -- what an 8bpp
         * surface actually needs, and what the fixture asserts. */
        static const W32_PALETTEENTRY def[16] = {
            {0,0,0,0},{0,0,128,0},{0,128,0,0},{0,128,128,0},
            {128,0,0,0},{128,0,128,0},{128,128,0,0},{192,192,192,0},
            {128,128,128,0},{0,0,255,0},{0,255,0,0},{0,255,255,0},
            {255,0,0,0},{255,0,255,0},{255,255,0,0},{255,255,255,0},
        };
        o->u.pal.n = 16;
        memcpy(o->u.pal.e, def, sizeof def);
        return (W32_HGDIOBJ)handle_of_obj(o);
    }
    default:
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
}

W32ABI W32_HGDIOBJ GetCurrentObject(W32_HDC hdc, W32_UINT type) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    switch (type) {
    case W32_OBJ_PEN:    return d->pen   ? (W32_HGDIOBJ)(uintptr_t)d->pen
                                         : GetStockObject(W32_BLACK_PEN);
    case W32_OBJ_BRUSH:  return d->brush ? (W32_HGDIOBJ)(uintptr_t)d->brush
                                         : GetStockObject(W32_BLACK_BRUSH);
    case W32_OBJ_FONT:   return d->font  ? (W32_HGDIOBJ)(uintptr_t)d->font
                                         : GetStockObject(W32_SYSTEM_FONT);
    case W32_OBJ_PAL:    return d->pal   ? (W32_HGDIOBJ)(uintptr_t)d->pal
                                         : GetStockObject(W32_DEFAULT_PALETTE);
    case W32_OBJ_BITMAP: return d->bmp   ? (W32_HGDIOBJ)(uintptr_t)d->bmp : 0;
    default: w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0;
    }
}

W32ABI int32_t GetObjectW(W32_HGDIOBJ obj, int32_t cb, void *buf) {
    int i = idx_from_h(obj);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    switch (gobjs[i].type) {
    case GOBJ_PEN: {
        W32_LOGPEN lp;
        gdi_pen_t *p = &gobjs[i].u.pen;
        lp.lopnStyle = p->style;
        lp.lopnWidth.x = p->width; lp.lopnWidth.y = 0;
        lp.lopnColor = p->color;
        if (!buf || cb <= 0) return (int32_t)sizeof lp;
        size_t n = (size_t)cb < sizeof lp ? (size_t)cb : sizeof lp;
        memcpy(buf, &lp, n);
        return (int32_t)n;
    }
    case GOBJ_BRUSH: {
        W32_LOGBRUSH lb;
        gdi_brush_t *b = &gobjs[i].u.brush;
        lb.lbStyle = b->style; lb.lbColor = b->color;
        lb.lbHatch = (uint32_t)(int32_t)b->hatch;
        if (!buf || cb <= 0) return (int32_t)sizeof lb;
        size_t n = (size_t)cb < sizeof lb ? (size_t)cb : sizeof lb;
        memcpy(buf, &lb, n);
        return (int32_t)n;
    }
    case GOBJ_FONT: {
        gdi_font_t *f = &gobjs[i].u.font;
        if (!buf || cb <= 0) return (int32_t)sizeof(W32_LOGFONTW);
        size_t n = (size_t)cb < sizeof(W32_LOGFONTW) ? (size_t)cb : sizeof(W32_LOGFONTW);
        memcpy(buf, &f->lf, n);
        return (int32_t)n;
    }
    case GOBJ_BITMAP: {
        gdi_bitmap_t *b = &gobjs[i].u.bmp;
        W32_BITMAP bm;
        bm.bmType = 0;
        bm.bmWidth = b->w; bm.bmHeight = b->h;
        bm.bmWidthBytes = (int32_t)b->stride;
        bm.bmPlanes = 1; bm.bmBitsPixel = (uint16_t)b->bpp;
        bm.bmBits = b->bits;
        if (!buf || cb <= 0) return (int32_t)sizeof bm;
        size_t n = (size_t)cb < sizeof bm ? (size_t)cb : sizeof bm;
        memcpy(buf, &bm, n);
        return (int32_t)n;
    }
    default:
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
}

W32ABI int32_t GetObjectA(W32_HGDIOBJ obj, int32_t cb, void *buf) {
    int i = idx_from_h(obj);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (gobjs[i].type == GOBJ_FONT) {
        gdi_font_t *f = &gobjs[i].u.font;
        W32_LOGFONTA lfa;
        memset(&lfa, 0, sizeof lfa);
        lfa.lfHeight = f->lf.lfHeight; lfa.lfWidth = f->lf.lfWidth;
        lfa.lfEscapement = f->lf.lfEscapement;
        lfa.lfOrientation = f->lf.lfOrientation;
        lfa.lfWeight = f->lf.lfWeight;
        lfa.lfItalic = f->lf.lfItalic; lfa.lfUnderline = f->lf.lfUnderline;
        lfa.lfStrikeOut = f->lf.lfStrikeOut; lfa.lfCharSet = f->lf.lfCharSet;
        lfa.lfOutPrecision = f->lf.lfOutPrecision;
        lfa.lfClipPrecision = f->lf.lfClipPrecision;
        lfa.lfQuality = f->lf.lfQuality;
        lfa.lfPitchAndFamily = f->lf.lfPitchAndFamily;
        for (int k = 0; k < 31 && f->lf.lfFaceName[k]; k++)
            lfa.lfFaceName[k] = (char)f->lf.lfFaceName[k];
        if (!buf || cb <= 0) return (int32_t)sizeof lfa;
        size_t n = (size_t)cb < sizeof lfa ? (size_t)cb : sizeof lfa;
        memcpy(buf, &lfa, n);
        return (int32_t)n;
    }
    return GetObjectW(obj, cb, buf);   /* pens/brushes/bitmaps: no A/W split */
}

W32ABI W32_BOOL UnrealizeObject(W32_HGDIOBJ obj) {
    int i = idx_from_h(obj);
    if (i < 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (gobjs[i].type == GOBJ_PALETTE) gobjs[i].u.pal.realized = 0;
    return 1;
}

W32ABI W32_BOOL DeleteObject(W32_HGDIOBJ obj) {
    int i = idx_from_h(obj);
    if (i < 0) return W32_TRUE;    /* A-5 contract: delete of a raw value
                                    * (colour-brush legacy) is a no-op */
    switch (gobjs[i].type) {
    case GOBJ_BITMAP:
        pool_free(gobjs[i].u.bmp.bits, gobjs[i].u.bmp.alloc);
        break;
    case GOBJ_ICON:
        pool_free(gobjs[i].u.icon.argb, gobjs[i].u.icon.alloc);
        break;
    default:
        break;
    }
    gobjs[i].used = 0;
    return W32_TRUE;
}

/* ---- attribute setters ---------------------------------------------- */
W32ABI W32_UINT GetBkMode(W32_HDC hdc) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    return d->bk_mode;
}
W32ABI int32_t SetBkMode(W32_HDC hdc, int32_t mode) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (mode != W32_TRANSPARENT && mode != W32_OPAQUE) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0;
    }
    int32_t old = (int32_t)d->bk_mode;
    d->bk_mode = (uint32_t)mode;
    return old;
}
W32ABI W32_DWORD SetBkColor(W32_HDC hdc, W32_DWORD color) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0xFFFFFFFFu; }
    W32_DWORD old = d->bk_color;
    d->bk_color = color;
    return old;
}
W32ABI W32_DWORD SetTextColor(W32_HDC hdc, W32_DWORD color) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0xFFFFFFFFu; }
    W32_DWORD old = d->text_color;
    d->text_color = color;
    return old;
}
W32ABI W32_UINT SetTextAlign(W32_HDC hdc, W32_UINT align) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return (W32_UINT)W32_GDI_ERROR; }
    W32_UINT old = d->text_align;
    d->text_align = align;
    return old;
}
W32ABI int32_t SetROP2(W32_HDC hdc, int32_t mode) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    int32_t old = (int32_t)d->rop2;
    d->rop2 = (uint32_t)mode;
    return old;
}
W32ABI int32_t GetROP2(W32_HDC hdc) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    return (int32_t)d->rop2;
}
W32ABI int32_t SetMapMode(W32_HDC hdc, int32_t mode) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (mode < W32_MM_TEXT || mode > W32_MM_ANISOTROPIC) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0;
    }
    int32_t old = d->map_mode;
    d->map_mode = mode;
    return old;
}
W32ABI W32_BOOL DPtoLP(W32_HDC hdc, W32_POINT *pts, int32_t n) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d || !pts || n <= 0) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    /* The ladder carries SetWindowOrgEx but no extent setters, so the
     * transform is the window-origin offset scaled by the mode's unit
     * (LOGPIXELS from GetDeviceCaps -- the DPI record honoured here
     * too).  MM_TEXT and the metric modes alike. */
    int32_t dpi = (int32_t)0;
    switch (d->map_mode) {
    case W32_MM_LOMETRIC:  dpi = -(int32_t)GetDeviceCaps(hdc, W32_LOGPIXELSX) / 1000; break;
    case W32_MM_HIMETRIC:  dpi = -(int32_t)GetDeviceCaps(hdc, W32_LOGPIXELSX) / 10000; break;
    case W32_MM_LOENGLISH: dpi = -(int32_t)GetDeviceCaps(hdc, W32_LOGPIXELSX) / 1000; break;
    case W32_MM_HIENGLISH: dpi = -(int32_t)GetDeviceCaps(hdc, W32_LOGPIXELSX) / 10000; break;
    case W32_MM_TWIPS:     dpi = -(int32_t)GetDeviceCaps(hdc, W32_LOGPIXELSX) / 1440; break;
    default:               dpi = 1; break;   /* MM_TEXT, ISO/ANISOTROPIC 1:1 */
    }
    if (dpi == 0) dpi = 1;
    for (int i = 0; i < n; i++) {
        int32_t dx = pts[i].x - d->worg_x;
        int32_t dy = pts[i].y - d->worg_y;
        pts[i].x = dx / dpi;
        pts[i].y = dy / dpi;
    }
    return 1;
}
W32ABI W32_BOOL SetWindowOrgEx(W32_HDC hdc, int32_t x, int32_t y, W32_POINT *old) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (old) { old->x = d->worg_x; old->y = d->worg_y; }
    d->worg_x = x; d->worg_y = y;
    return 1;
}
W32ABI W32_BOOL OffsetWindowOrgEx(W32_HDC hdc, int32_t dx, int32_t dy, W32_POINT *old) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (old) { old->x = d->worg_x; old->y = d->worg_y; }
    d->worg_x += dx; d->worg_y += dy;
    return 1;
}
W32ABI W32_BOOL SetBrushOrgEx(W32_HDC hdc, int32_t x, int32_t y, W32_POINT *old) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (old) { old->x = d->borg_x; old->y = d->borg_y; }
    d->borg_x = x; d->borg_y = y;
    return 1;
}

/* ---- GetDeviceCaps: DPI comes from the theme + manifest record ------- *
 * The loader parsed the application manifest in W32A-1 and recorded the
 * dpiAware fact; it hands it to the personality here.  An unaware app
 * gets 96 -- the Windows compatibility behaviour -- while an aware one
 * gets the desktop's configured DPI (gtheme --dpi, applied by glaunch). */
static int gdi_app_dpi_aware;

void w32_gdi_set_dpi_aware(int aware) { gdi_app_dpi_aware = aware ? 1 : 0; }

W32_UINT w32_gdi_dpi_for(W32_HDC hdc) {
    if (!gdi_app_dpi_aware) return 96;
#ifdef AURALITE_W32_HOST_TEST
    uint32_t dpi = w32_gdi_host_dpi();     /* the suite's theme record */
#else
    ag_theme_t t;
    if (!ag_theme_get(&t) || !t.dpi) t.dpi = 96;
    uint32_t dpi = t.dpi;
#endif
    if (dpi < 48) dpi = 48;
    if (dpi > 480) dpi = 480;
    (void)hdc;
    return dpi;
}

W32ABI int32_t GetDeviceCaps(W32_HDC hdc, int32_t idx) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    switch (idx) {
    case W32_DRIVERVERSION:  return 1;
    case W32_TECHNOLOGY:     return W32_DT_RASDISPLAY;
    case W32_HORZSIZE:
    case W32_VERTSIZE:       return (W32_UINT)(idx == W32_HORZSIZE ? 640 * 254 / (int)w32_gdi_dpi_for(hdc) : 480 * 254 / (int)w32_gdi_dpi_for(hdc));
    case W32_HORZRES:        return 640;
    case W32_VERTRES:        return 480;
    case W32_BITSPIXEL:      return 32;
    case W32_PLANES:         return 1;
    case W32_NUMBRUSHES:     return 8;
    case W32_NUMPENS:        return 16;
    case W32_NUMMARKERS:     return 0;
    case W32_NUMFONTS:       return 1;
    case W32_NUMCOLORS:      return -1;
    case W32_PDEVICESIZE:    return 0;
    case W32_CURVECAPS:      return 0xFFFF;
    case W32_LINECAPS:       return 0xFFFF;
    case W32_POLYGONALCAPS:  return 0xFFFF;
    case W32_TEXTCAPS:       return W32_TC_OP_CHARACTER;
    case W32_CLIPCAPS:       return 1;
    case W32_RASTERCAPS:     return W32_RC_BITBLT | W32_RC_BITMAP64 | W32_RC_DI_BITMAP;
    case W32_ASPECTX:
    case W32_ASPECTY:        return 36;
    case W32_ASPECTXY:       return 51;
    case W32_LOGPIXELSX:
    case W32_LOGPIXELSY:     return w32_gdi_dpi_for(hdc);
    case W32_SIZEPALETTE:
    case W32_NUMRESERVED:    return 256;
    case W32_COLORRES:       return 24;
    case W32_PHYSICALWIDTH:
    case W32_PHYSICALHEIGHT: return 0;
    case W32_PHYSICALOFFSETX:
    case W32_PHYSICALOFFSETY: return 0;
    case W32_SCALINGFACTORX:
    case W32_SCALINGFACTORY: return 0;
    case W32_VREFRESH:       return 60;
    case W32_DESKTOPHORZRES: return 640;
    case W32_DESKTOPVERTRES: return 480;
    case W32_BLTALIGNMENT:   return 1;
    case W32_SHADEBLENDCAPS: return W32_SB_NONE;
    case W32_COLORMGMTCAPS:  return W32_CMGRAST_NONE;
    default:
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
}

/* ---- pixel access ----------------------------------------------------- */
W32ABI W32_DWORD GetPixel(W32_HDC hdc, int32_t x, int32_t y) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) return W32_CLR_INVALID;
    if (!clip_has(d, x, y)) return W32_CLR_INVALID;
    if (d->kind == 1) {
        gdi_bitmap_t *b = dc_bmp(d);
        if (!b || x < 0 || y < 0 || x >= b->w || y >= b->h)
            return W32_CLR_INVALID;
        surf_t s;
        if (surf_of_dc(d, &s) == 0)
            return w32_ag_to_colorref(surf_get(&s, x, y));
        return W32_CLR_INVALID;
    }
    if (d->kind == 0) {
        int wid = w32_win_ag_wid(d->slot);
        if (wid < 0) return W32_CLR_INVALID;
        int32_t ag = ag_get_pixel(wid, x, y);
        if (ag < 0) return W32_CLR_INVALID;
        return w32_ag_to_colorref((uint32_t)ag);
    }
    return W32_CLR_INVALID;                     /* screen: metrics only */
}

W32ABI W32_DWORD SetPixel(W32_HDC hdc, int32_t x, int32_t y, W32_DWORD color) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) return W32_CLR_INVALID;
    uint32_t ag = w32_colorref_to_ag(color);
    if (d->kind == 1) {
        surf_t s;
        if (surf_of_dc(d, &s) != 0) return W32_CLR_INVALID;
        surf_put(&s, x, y, ag);
        return color;
    }
    if (d->kind == 0) {
        int wid = w32_win_ag_wid(d->slot);
        if (wid < 0) return W32_CLR_INVALID;
        ag_fill_rect(wid, (uint32_t)x, (uint32_t)y, 1, 1, ag);
        return color;
    }
    return W32_CLR_INVALID;
}

/* ===================================================================== *
 * Blits and raster ops                                                  *
 * ===================================================================== */


static uint32_t rop_mix(uint32_t rop, uint32_t s, uint32_t d, uint32_t p) {
    switch (rop) {
    case W32_BLACKNESS:  return 0;
    case W32_WHITENESS:  return 0x00FFFFFFu;
    case W32_DSTINVERT:  return d ^ 0x00FFFFFFu;
    case W32_NOTSRCCOPY: return s ^ 0x00FFFFFFu;
    case W32_SRCCOPY:    return s;
    case W32_SRCAND:     return s & d;
    case W32_SRCINVERT:  return s ^ d;
    case W32_SRCPAINT:   return s | d;
    case W32_PATCOPY:    return p;
    case W32_PATINVERT:  return p ^ d;
    default:                 return 0;
    }
}

static int rop_supported(uint32_t rop) {
    switch (rop) {
    case W32_BLACKNESS: case W32_WHITENESS: case W32_DSTINVERT:
    case W32_NOTSRCCOPY: case W32_SRCCOPY: case W32_SRCAND:
    case W32_SRCINVERT: case W32_SRCPAINT: case W32_PATCOPY:
    case W32_PATINVERT:
        return 1;
    default:
        return 0;
    }
}

static gdi_bitmap_t *bmp_from_h(W32_HGDIOBJ h) {
    gdi_obj_t *o = obj_from_h(h, GOBJ_BITMAP);
    return o ? &o->u.bmp : 0;
}

static void blit_dst_pixel_put(surf_t *s, int32_t x, int32_t y, uint32_t ag) {
    surf_put(s, x, y, ag);
}

typedef struct {
    int      src_mem;
    surf_t  *ss;
    gdi_dc_t *src_dc;             /* window source */
    int32_t  sx, sy;
    uint32_t rop;
    gdi_brush_t *br;
    gdi_dc_t *dst_dc;             /* for brush anchoring */
} blit_ctx_t;

static void blit_body(surf_t *dst, void *user) {
    blit_ctx_t *c = (blit_ctx_t *)user;
    /* Absolute coordinates: ox/oy carry the destination origin, so a
     * memory-DC blit writes at (ox+x, oy+y) and a window-DC blit's
     * temp-local write subtracts them again inside surf_put. */
    for (int32_t y = 0; y < dst->h; y++) {
        for (int32_t x = 0; x < dst->w; x++) {
            int32_t X = dst->ox + x, Y = dst->oy + y;
            uint32_t s = 0;
            if (c->rop != W32_BLACKNESS && c->rop != W32_WHITENESS &&
                c->rop != W32_PATCOPY && c->rop != W32_PATINVERT) {
                if (c->src_mem) {
                    s = surf_get(c->ss, c->sx + x, c->sy + y);
                } else if (c->src_dc && c->src_dc->kind == 0) {
                    int wid = w32_win_ag_wid(c->src_dc->slot);
                    int32_t p = wid >= 0
                        ? ag_get_pixel(wid, c->sx + x, c->sy + y) : -1;
                    s = p >= 0 ? (uint32_t)p : 0;
                }
            }
            uint32_t d = surf_get(dst, X, Y);
            uint32_t p = brush_color_at(c->dst_dc, c->br, X, Y);
            if (p == GDI_BRUSH_SKIP) p = 0;
            uint32_t out = rop_mix(c->rop, s, d, p);
            blit_dst_pixel_put(dst, X, Y, out);
        }
    }
}

W32ABI W32_BOOL BitBlt(W32_HDC dd, int32_t x, int32_t y, int32_t w, int32_t h,
                       W32_HDC sd, int32_t sx, int32_t sy, W32_DWORD rop) {
    if (w <= 0 || h <= 0) return 1;
    if (!rop_supported(rop)) {
        w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    gdi_dc_t *ddc = dc_from_h(dd), *sdc = dc_from_h(sd);
    if (!ddc || !sdc) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    W32_RECT r = { x, y, x + w, y + h };
    if (!clip_intersects(ddc, &r)) return 1;
    blit_ctx_t c;
    memset(&c, 0, sizeof c);
    c.rop = rop; c.dst_dc = ddc;
    c.sx = sx; c.sy = sy;
    c.br = dc_brush(ddc);
    surf_t ssrc;
    if (sdc->kind == 1) {
        if (surf_of_dc(sdc, &ssrc) == 0) { c.src_mem = 1; c.ss = &ssrc; }
    } else {
        c.src_dc = sdc;
    }
    if (ddc->kind == 1) {
        surf_t sdst;
        if (surf_of_dc(ddc, &sdst) != 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
        surf_t tmp = sdst;
        tmp.ox = x; tmp.oy = y; tmp.w = w; tmp.h = h;
        blit_body(&tmp, &c);
        return 1;
    }
    if (ddc->kind == 0) {
        int ok = win_raster_blit(ddc, x, y, w, h, blit_body, &c);
        return ok;
    }
    w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    return 0;
}

static void pat_body(surf_t *dst, void *user) {
    blit_ctx_t *c = (blit_ctx_t *)user;
    for (int32_t y = 0; y < dst->h; y++)
        for (int32_t x = 0; x < dst->w; x++) {
            int32_t X = dst->ox + x, Y = dst->oy + y;
            uint32_t d = surf_get(dst, X, Y);
            uint32_t p = brush_color_at(c->dst_dc, c->br, X, Y);
            if (p == GDI_BRUSH_SKIP) p = 0;
            blit_dst_pixel_put(dst, X, Y, rop_mix(c->rop, 0, d, p));
        }
}

W32ABI W32_BOOL PatBlt(W32_HDC hdc, int32_t x, int32_t y, int32_t w, int32_t h,
                       W32_DWORD rop) {
    if (w <= 0 || h <= 0) return 1;
    uint32_t rop2 = rop & 0x00FFFFFFu;
    if (rop2 != W32_PATCOPY && rop2 != W32_PATINVERT &&
        rop2 != W32_DSTINVERT && rop2 != W32_BLACKNESS &&
        rop2 != W32_WHITENESS) {
        w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    blit_ctx_t c;
    memset(&c, 0, sizeof c);
    c.rop = rop2 ? rop2 : W32_PATCOPY;
    c.dst_dc = d;
    c.br = dc_brush(d);
    if (d->kind == 1) {
        surf_t s;
        if (surf_of_dc(d, &s) != 0) return 0;
        surf_t tmp = s;
        tmp.ox = x; tmp.oy = y; tmp.w = w; tmp.h = h;
        pat_body(&tmp, &c);
        return 1;
    }
    if (d->kind == 0)
        return win_raster_blit(d, x, y, w, h, pat_body, &c);
    w32_set_last_error(W32_ERROR_INVALID_HANDLE);
    return 0;
}

typedef struct {
    W32_BLENDFUNCTION bf;
    surf_t *ss;
} alpha_ctx_t;

static void alpha_body(surf_t *dst, void *user) {
    alpha_ctx_t *c = (alpha_ctx_t *)user;
    for (int32_t y = 0; y < dst->h; y++)
        for (int32_t x = 0; x < dst->w; x++) {
            int32_t X = dst->ox + x, Y = dst->oy + y;
            uint32_t s = surf_get(c->ss, x, y);
            uint32_t d = surf_get(dst, X, Y);
            uint32_t a = c->bf.SourceConstantAlpha;
            if (!a) continue;
            uint32_t out = 0;
            out |= ((s & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
            out |= ((((s >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255) << 8;
            out |= ((((s >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255) << 16;
            blit_dst_pixel_put(dst, X, Y, out);
        }
}

W32ABI W32_BOOL GdiAlphaBlend(W32_HDC dd, int32_t x, int32_t y,
                              int32_t w, int32_t h, W32_HDC sd,
                              int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                              W32_BLENDFUNCTION bf) {
    (void)sx; (void)sy; (void)sw; (void)sh;    /* 1:1 -- no scaling yet */
    if (w <= 0 || h <= 0) return 1;
    gdi_dc_t *ddc = dc_from_h(dd), *sdc = dc_from_h(sd);
    if (!ddc || !sdc || sdc->kind != 1 || ddc->kind != 1) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return 0;
    }
    surf_t ssrc, sdst;
    if (surf_of_dc(sdc, &ssrc) != 0 || surf_of_dc(ddc, &sdst) != 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return 0;
    }
    alpha_ctx_t c;
    memset(&c, 0, sizeof c);
    c.bf = bf; c.ss = &ssrc;
    surf_t tmp = sdst;
    tmp.ox = x; tmp.oy = y; tmp.w = w; tmp.h = h;
    alpha_body(&tmp, &c);
    return 1;
}

/* ===================================================================== *
 * Pens and brushes                                                      *
 * ===================================================================== */

W32ABI W32_HPEN CreatePen(int32_t style, int32_t width, W32_DWORD color) {
    if (style < 0 || style > W32_PS_DASHDOTDOT || style == W32_PS_NULL + 1) {
        if (style != W32_PS_NULL) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return 0;
        }
    }
    gdi_obj_t *o;
    W32_HPEN h = (W32_HPEN)gdi_alloc_obj(GOBJ_PEN, &o);
    if (!h) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    o->u.pen.style = (uint32_t)style;
    o->u.pen.width = width > 0 ? width : 1;
    o->u.pen.color = color;
    return h;
}

W32ABI W32_HPEN ExtCreatePen(W32_DWORD style, W32_DWORD width,
                             const W32_LOGBRUSH *lb, W32_DWORD style_count,
                             const W32_DWORD *style_vals) {
    if (!lb) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    (void)style_count; (void)style_vals;    /* user-style dashes: unused,
                                             * we draw solid -- and say so
                                             * in the plan's deviations */
    gdi_obj_t *o;
    W32_HPEN h = (W32_HPEN)gdi_alloc_obj(GOBJ_PEN, &o);
    if (!h) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    uint32_t base = style & 0xFFu;
    o->u.pen.style = base <= W32_PS_DASHDOTDOT ? base : (uint32_t)W32_PS_SOLID;
    o->u.pen.width = (int32_t)width > 0 ? (int32_t)width : 1;
    o->u.pen.color = lb->lbColor;
    return h;
}

W32ABI W32_HBRUSH CreateSolidBrush(W32_DWORD color) {
    gdi_obj_t *o;
    W32_HBRUSH h = (W32_HBRUSH)gdi_alloc_obj(GOBJ_BRUSH, &o);
    if (!h) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    o->u.brush.style = GDI_BS_SOLID;
    o->u.brush.color = color;
    return h;
}

W32ABI W32_HBRUSH CreateHatchBrush(int32_t style, W32_DWORD color) {
    if (style < W32_HS_HORIZONTAL || style > W32_HS_DIAGCROSS) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    gdi_obj_t *o;
    W32_HBRUSH h = (W32_HBRUSH)gdi_alloc_obj(GOBJ_BRUSH, &o);
    if (!h) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    o->u.brush.style = GDI_BS_HATCHED;
    o->u.brush.color = color;
    o->u.brush.hatch = style;
    return h;
}

W32ABI W32_HBRUSH CreatePatternBrush(W32_HBITMAP bmp) {
    gdi_bitmap_t *b = bmp_from_h(bmp);
    if (!b) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    gdi_obj_t *o;
    W32_HBRUSH h = (W32_HBRUSH)gdi_alloc_obj(GOBJ_BRUSH, &o);
    if (!h) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    o->u.brush.style = GDI_BS_PATTERN;
    /* The pattern is the bitmap's first 8 rows at 1 bit per pixel,
     * sampled like a monochrome DIB (MSB first).  Colour bitmaps reduce
     * to their luminance bit -- the documented rule. */
    o->u.brush.color = 0x00FFFFFFu;
    int rows = b->h < 8 ? b->h : 8;
    for (int y = 0; y < rows; y++) {
        uint8_t bits = 0;
        for (int x = 0; x < 8 && x < b->w; x++) {
            uint32_t ag = 0;
            if (b->bpp == 32 && !b->is_dib) {
                ag = ((uint32_t *)(void *)b->bits)[y * (int32_t)(b->stride / 4) + x];
            } else {
                uint32_t rrow = (uint32_t)(b->top_down ? y : (b->h - 1 - y));
                uint8_t *row = b->bits + rrow * b->stride;
                if (b->bpp == 32) {
                    uint8_t *p = row + 4u * (uint32_t)x;
                    ag = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
                } else if (b->bpp == 24) {
                    uint8_t *p = row + 3u * (uint32_t)x;
                    ag = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
                }
            }
            uint32_t lum = ((ag >> 16) & 0xFF) * 30 + ((ag >> 8) & 0xFF) * 59 +
                           (ag & 0xFF) * 11;
            if (lum >= 128 * 100) bits |= (uint8_t)(0x80u >> x);
        }
        o->u.brush.pat[y] = bits;
    }
    return h;
}

/* Colour decode for the A-5 raw-colour brush convention: user32 class
 * backgrounds still pass colours, and FillRect accepts both. */
W32_DWORD w32_gdi_brush_color(W32_HBRUSH brush) {
    gdi_obj_t *o = obj_from_h(brush, GOBJ_BRUSH);
    if (o && o->u.brush.style == GDI_BS_SOLID) return o->u.brush.color;
    if (o) return 0x00FFFFFFu;
    return (W32_DWORD)(uintptr_t)brush;     /* A-5: the raw colour */
}

/* ===================================================================== *
 * Bitmaps and DIBs                                                      *
 * ===================================================================== */

W32ABI W32_HBITMAP CreateCompatibleBitmap(W32_HDC hdc, int32_t w, int32_t h) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    /* Compatible with the screen: 32bpp. */
    gdi_bitmap_t *b = bmp_create_internal(w, h, 32, 0, 0, 0, 0);
    if (!b) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    return (W32_HBITMAP)handle_of_bmp(b);
}

W32ABI W32_HBITMAP CreateBitmap(int32_t w, int32_t h, W32_UINT planes,
                                W32_UINT bpp, const void *bits) {
    if (planes != 1) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    if (bpp != 1 && bpp != 8 && bpp != 24 && bpp != 32) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    gdi_bitmap_t *b = bmp_create_internal(w, h, (int)bpp, 0, 0, 0, 0);
    if (!b) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    if (bits) {
        /* CreateBitmap rows are DWORD-aligned, top-down, no colour table. */
        uint32_t src_stride = ((uint32_t)w * bpp + 31u) / 32u * 4u;
        size_t n = (size_t)src_stride * (size_t)h;
        if (n <= b->alloc) memcpy(b->bits, bits, n);
    }
    return (W32_HBITMAP)handle_of_bmp(b);
}

W32ABI W32_HBITMAP CreateDIBSection(W32_HDC hdc, const W32_BITMAPINFO *bi,
                                    W32_UINT usage, void **bits,
                                    void *section, W32_DWORD offset) {
    (void)hdc; (void)section; (void)offset;  /* no file-mapping DIBs yet */
    if (!bi || !bits || bi->bmiHeader.biSize < 40) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (bi->bmiHeader.biCompression != W32_BI_RGB) {
        w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    int32_t w = bi->bmiHeader.biWidth;
    int32_t h = bi->bmiHeader.biHeight;
    int top_down = 0;
    if (h < 0) { top_down = 1; h = -h; }
    int bpp = bi->bmiHeader.biBitCount;
    if (bpp != 8 && bpp != 24 && bpp != 32) {
        w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    int ct_n = 0;
    if (bpp == 8) ct_n = 256;
    if (usage == W32_DIB_PAL_COLORS && bpp == 8) {
        /* palette indices: the 16-colour default palette is the table */
        gdi_bitmap_t *b = bmp_create_internal(w, h, 8, 1, top_down, 0, 0);
        if (!b) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
        for (int i = 0; i < 16; i++) {
            b->ct[i].red   = (uint8_t)((i & 1) ? 255 : 0);
            b->ct[i].green = (uint8_t)((i & 2) ? 255 : 0);
            b->ct[i].blue  = (uint8_t)((i & 4) ? 255 : 0);
        }
        b->ct_n = 16;
        *bits = b->bits;
        return (W32_HBITMAP)handle_of_bmp(b);
    }
    gdi_bitmap_t *b = bmp_create_internal(w, h, bpp, 1, top_down,
                                          bi->bmiColors, ct_n);
    if (!b) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    *bits = b->bits;
    return (W32_HBITMAP)handle_of_bmp(b);
}

/* DIB row packing for GetDIBits/SetDIBits: caller's BITMAPINFOHEADER
 * governs the format; we support the formats the engine stores. */
static int dib_pack(gdi_bitmap_t *b, W32_BITMAPINFOHEADER *bh,
                    uint8_t *out, size_t out_cap, int get) {
    int32_t w = bh->biWidth;
    int32_t h = bh->biHeight;
    int top_down = 0;
    if (h < 0) { top_down = 1; h = -h; }
    int bpp = bh->biBitCount;
    if (w != b->w || h != b->h || bh->biCompression != W32_BI_RGB) return -1;
    if (bpp != 8 && bpp != 24 && bpp != 32) return -1;
    uint32_t stride = (uint32_t)(((size_t)w * (size_t)bpp + 31u) / 32u * 4u);
    size_t need = (size_t)stride * (size_t)h;
    if (out && need > out_cap) return -1;
    uint32_t ct_n = bh->biClrUsed ? bh->biClrUsed : (bpp == 8 ? 256u : 0u);
    if (!get && bpp == 8 && ct_n == 0) return -1;
    for (int32_t y = 0; y < h; y++) {
        int32_t sy = top_down ? y : (h - 1 - y);
        uint8_t *dst = out ? out + (size_t)y * stride : 0;
        for (int32_t x = 0; x < w; x++) {
            uint32_t ag;
            if (get) {
                /* read through the surface reader (handles our layout) */
                if (b->bpp == 32 && !b->is_dib)
                    ag = ((uint32_t *)(void *)b->bits)[sy * (int32_t)(b->stride / 4) + x];
                else {
                    uint32_t rrow = (uint32_t)(b->top_down ? sy : (b->h - 1 - sy));
                    uint8_t *row = b->bits + rrow * b->stride;
                    if (b->bpp == 32) {
                        uint8_t *p = row + 4u * (uint32_t)x;
                        ag = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
                    } else if (b->bpp == 24) {
                        uint8_t *p = row + 3u * (uint32_t)x;
                        ag = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
                    } else {
                        int i = row[x];
                        ag = (i < b->ct_n && i < GDI_CT_MAX)
                           ? (((uint32_t)b->ct[i].red << 16) |
                              ((uint32_t)b->ct[i].green << 8) | b->ct[i].blue)
                           : 0;
                    }
                }
                if (dst) {
                    if (bpp == 32) {
                        dst[4u * (uint32_t)x + 0] = (uint8_t)(ag & 0xFF);
                        dst[4u * (uint32_t)x + 1] = (uint8_t)((ag >> 8) & 0xFF);
                        dst[4u * (uint32_t)x + 2] = (uint8_t)((ag >> 16) & 0xFF);
                        dst[4u * (uint32_t)x + 3] = 255;
                    } else if (bpp == 24) {
                        dst[3u * (uint32_t)x + 0] = (uint8_t)(ag & 0xFF);
                        dst[3u * (uint32_t)x + 1] = (uint8_t)((ag >> 8) & 0xFF);
                        dst[3u * (uint32_t)x + 2] = (uint8_t)((ag >> 16) & 0xFF);
                    } else {
                        /* nearest colour in the caller's table */
                        const W32_RGBQUAD *ct = (const W32_RGBQUAD *)(bh + 1);
                        int best = 0; long bd = 0x7FFFFFFF;
                        for (uint32_t i = 0; i < ct_n && i < 256; i++) {
                            long dr = (long)ct[i].red   - (long)((ag >> 16) & 0xFF);
                            long dg = (long)ct[i].green - (long)((ag >> 8) & 0xFF);
                            long db = (long)ct[i].blue  - (long)(ag & 0xFF);
                            long d = dr * dr + dg * dg + db * db;
                            if (d < bd) { bd = d; best = (int)i; }
                        }
                        dst[x] = (uint8_t)best;
                    }
                }
            } else {
                const uint8_t *src = out + (size_t)y * stride;
                uint32_t ag;
                if (bpp == 32)
                    ag = ((uint32_t)src[4u * (uint32_t)x + 2] << 16) |
                         ((uint32_t)src[4u * (uint32_t)x + 1] << 8) |
                         src[4u * (uint32_t)x + 0];
                else if (bpp == 24)
                    ag = ((uint32_t)src[3u * (uint32_t)x + 2] << 16) |
                         ((uint32_t)src[3u * (uint32_t)x + 1] << 8) |
                         src[3u * (uint32_t)x + 0];
                else {
                    const W32_RGBQUAD *ct = (const W32_RGBQUAD *)(bh + 1);
                    int i = src[x];
                    ag = (i < (int)ct_n && i < 256)
                       ? (((uint32_t)ct[i].red << 16) |
                          ((uint32_t)ct[i].green << 8) | ct[i].blue)
                       : 0;
                }
                if (b->bpp == 32 && !b->is_dib)
                    ((uint32_t *)(void *)b->bits)[sy * (int32_t)(b->stride / 4) + x] = ag;
                else {
                    uint32_t rrow = (uint32_t)(b->top_down ? sy : (b->h - 1 - sy));
                    uint8_t *row = b->bits + rrow * b->stride;
                    if (b->bpp == 32) {
                        row[4u * (uint32_t)x + 0] = (uint8_t)(ag & 0xFF);
                        row[4u * (uint32_t)x + 1] = (uint8_t)((ag >> 8) & 0xFF);
                        row[4u * (uint32_t)x + 2] = (uint8_t)((ag >> 16) & 0xFF);
                        row[4u * (uint32_t)x + 3] = 255;
                    } else if (b->bpp == 24) {
                        row[3u * (uint32_t)x + 0] = (uint8_t)(ag & 0xFF);
                        row[3u * (uint32_t)x + 1] = (uint8_t)((ag >> 8) & 0xFF);
                        row[3u * (uint32_t)x + 2] = (uint8_t)((ag >> 16) & 0xFF);
                    } else {
                        int best = 0; long bd = 0x7FFFFFFF;
                        for (int i = 0; i < b->ct_n && i < GDI_CT_MAX; i++) {
                            long dr = (long)b->ct[i].red   - (long)((ag >> 16) & 0xFF);
                            long dg = (long)b->ct[i].green - (long)((ag >> 8) & 0xFF);
                            long db = (long)b->ct[i].blue  - (long)(ag & 0xFF);
                            long d = dr * dr + dg * dg + db * db;
                            if (d < bd) { bd = d; best = i; }
                        }
                        row[x] = (uint8_t)best;
                    }
                }
            }
        }
    }
    return (int32_t)h;
}

W32ABI int32_t GetDIBits(W32_HDC hdc, W32_HBITMAP bmp, W32_UINT start,
                         W32_UINT lines, void *bits, W32_BITMAPINFO *bi,
                         W32_UINT usage) {
    (void)hdc; (void)usage;
    gdi_bitmap_t *b = bmp_from_h(bmp);
    if (!b || !bi || bi->bmiHeader.biSize < 40) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (bits) {
        if (bi->bmiHeader.biWidth == 0) bi->bmiHeader.biWidth = b->w;
        if (bi->bmiHeader.biHeight == 0) bi->bmiHeader.biHeight = b->h;
        int top_down = bi->bmiHeader.biHeight < 0;
        int32_t h = top_down ? -bi->bmiHeader.biHeight
                             : bi->bmiHeader.biHeight;
        if (h != b->h ||
            (bi->bmiHeader.biBitCount != 8 &&
             bi->bmiHeader.biBitCount != 24 &&
             bi->bmiHeader.biBitCount != 32)) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return 0;
        }
        if (start >= (W32_UINT)b->h) return 0;
        if (start + lines > (W32_UINT)b->h) lines = (W32_UINT)b->h - start;
        uint32_t bpp = bi->bmiHeader.biBitCount;
        uint32_t stride = ((uint32_t)b->w * bpp + 31u) / 32u * 4u;
        uint8_t *out = (uint8_t *)bits;
        for (W32_UINT i = 0; i < lines; i++) {
            /* DIB row order: bottom-up unless biHeight < 0. */
            int32_t sy = top_down ? (int32_t)(start + i)
                                  : (int32_t)(b->h - start - i - 1);
            uint8_t *dst = out + (size_t)i * stride;
            for (int32_t x = 0; x < b->w; x++) {
                uint32_t ag;
                if (b->bpp == 32 && !b->is_dib) {
                    ag = ((uint32_t *)(void *)b->bits)
                         [sy * (int32_t)(b->stride / 4) + x];
                } else {
                    uint32_t rrow = (uint32_t)(b->top_down
                                               ? sy : (b->h - 1 - sy));
                    uint8_t *row = b->bits + rrow * b->stride;
                    if (b->bpp == 32) {
                        uint8_t *q = row + 4u * (uint32_t)x;
                        ag = ((uint32_t)q[2] << 16) |
                             ((uint32_t)q[1] << 8) | q[0];
                    } else if (b->bpp == 24) {
                        uint8_t *q = row + 3u * (uint32_t)x;
                        ag = ((uint32_t)q[2] << 16) |
                             ((uint32_t)q[1] << 8) | q[0];
                    } else {
                        int ci = row[x];
                        ag = (ci < b->ct_n && ci < GDI_CT_MAX)
                           ? (((uint32_t)b->ct[ci].red << 16) |
                              ((uint32_t)b->ct[ci].green << 8) |
                              b->ct[ci].blue)
                           : 0;
                    }
                }
                if (bpp == 32) {
                    dst[4u * (uint32_t)x + 0] = (uint8_t)(ag & 0xFF);
                    dst[4u * (uint32_t)x + 1] = (uint8_t)((ag >> 8) & 0xFF);
                    dst[4u * (uint32_t)x + 2] = (uint8_t)((ag >> 16) & 0xFF);
                    dst[4u * (uint32_t)x + 3] = 255;
                } else if (bpp == 24) {
                    dst[3u * (uint32_t)x + 0] = (uint8_t)(ag & 0xFF);
                    dst[3u * (uint32_t)x + 1] = (uint8_t)((ag >> 8) & 0xFF);
                    dst[3u * (uint32_t)x + 2] = (uint8_t)((ag >> 16) & 0xFF);
                } else {
                    /* nearest entry of the caller's colour table */
                    const W32_RGBQUAD *ct =
                        (const W32_RGBQUAD *)&bi->bmiColors;
                    uint32_t ct_n = bi->bmiHeader.biClrUsed
                                  ? bi->bmiHeader.biClrUsed : 256u;
                    int best = 0; long bd = 0x7FFFFFFF;
                    for (uint32_t ci = 0; ci < ct_n && ci < 256; ci++) {
                        long dr = (long)ct[ci].red   - (long)((ag >> 16) & 0xFF);
                        long dg = (long)ct[ci].green - (long)((ag >> 8) & 0xFF);
                        long db = (long)ct[ci].blue  - (long)(ag & 0xFF);
                        long d = dr * dr + dg * dg + db * db;
                        if (d < bd) { bd = d; best = (int)ci; }
                    }
                    dst[x] = (uint8_t)best;
                }
            }
        }
        return (int32_t)lines;
    }
    /* header-only query: report the bitmap's own geometry */
    bi->bmiHeader.biWidth = b->w;
    bi->bmiHeader.biHeight = b->h;
    bi->bmiHeader.biPlanes = 1;
    bi->bmiHeader.biBitCount = (uint16_t)b->bpp;
    bi->bmiHeader.biCompression = W32_BI_RGB;
    bi->bmiHeader.biSizeImage = b->stride * (uint32_t)b->h;
    if (b->bpp == 8 && b->ct_n)
        for (int i = 0; i < b->ct_n && i < 256; i++)
            bi->bmiColors[i] = b->ct[i];
    return b->h;
}

W32ABI int32_t SetDIBits(W32_HDC hdc, W32_HBITMAP bmp, W32_UINT start,
                         W32_UINT lines, const void *bits,
                         const W32_BITMAPINFO *bi, W32_UINT usage) {
    (void)hdc; (void)usage;
    gdi_bitmap_t *b = bmp_from_h(bmp);
    if (!b || !bi || !bits || bi->bmiHeader.biSize < 40) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (start >= (W32_UINT)b->h) return 0;
    if (start + lines > (W32_UINT)b->h) lines = (W32_UINT)b->h - start;
    if (bi->bmiHeader.biWidth != b->w || bi->bmiHeader.biBitCount == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    int rc = dib_pack(b, (W32_BITMAPINFOHEADER *)&bi->bmiHeader,
                      (uint8_t *)bits, (size_t)-1, 0);
    return rc < 0 ? 0 : (int32_t)lines;
}

/* ===================================================================== *
 * Regions and clipping                                                  *
 * ===================================================================== */

static gdi_rgn_t *rgn_from_h(W32_HRGN h) {
    gdi_obj_t *o = obj_from_h(h, GOBJ_RGN);
    return o ? &o->u.rgn : 0;
}

W32ABI W32_HRGN CreateRectRgn(int32_t l, int32_t t, int32_t r, int32_t b) {
    gdi_obj_t *o;
    W32_HRGN h = (W32_HRGN)gdi_alloc_obj(GOBJ_RGN, &o);
    if (!h) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    if (r > l && b > t) {
        o->u.rgn.n = 1;
        o->u.rgn.r[0].left = l; o->u.rgn.r[0].top = t;
        o->u.rgn.r[0].right = r; o->u.rgn.r[0].bottom = b;
    } else {
        o->u.rgn.n = 0;                    /* Win32: empty region, not error */
    }
    return h;
}

W32ABI W32_HRGN CreateRectRgnIndirect(const W32_RECT *r) {
    if (!r) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    return CreateRectRgn(r->left, r->top, r->right, r->bottom);
}

/* Rectangle-list algebra: intersect / union (append, no coalescing --
 * the predicate is membership, and duplicates are harmless), subtract,
 * copy.  Bounded at GDI_RGN_MAX rects: an app that overflows gets the
 * list truncated to the first GDI_RGN_MAX rects and the region stays
 * usable, which is the honest subset of "arbitrary regions". */
static void rgn_clip_list(gdi_rgn_t *dst, const gdi_rgn_t *a, const gdi_rgn_t *b,
                          int32_t mode) {
    int n = 0;
    if (mode == W32_RGN_AND) {
        for (int i = 0; i < a->n && n < GDI_RGN_MAX; i++)
            for (int j = 0; j < b->n && n < GDI_RGN_MAX; j++) {
                int32_t l = a->r[i].left  > b->r[j].left  ? a->r[i].left  : b->r[j].left;
                int32_t t = a->r[i].top   > b->r[j].top   ? a->r[i].top   : b->r[j].top;
                int32_t r = a->r[i].right < b->r[j].right ? a->r[i].right : b->r[j].right;
                int32_t bo = a->r[i].bottom < b->r[j].bottom ? a->r[i].bottom : b->r[j].bottom;
                if (r > l && bo > t) {
                    dst->r[n].left = l; dst->r[n].top = t;
                    dst->r[n].right = r; dst->r[n].bottom = bo;
                    n++;
                }
            }
    } else if (mode == W32_RGN_OR || mode == W32_RGN_COPY) {
        for (int i = 0; i < a->n && n < GDI_RGN_MAX; i++) dst->r[n++] = a->r[i];
        if (mode == W32_RGN_OR)
            for (int j = 0; j < b->n && n < GDI_RGN_MAX; j++) dst->r[n++] = b->r[j];
    } else if (mode == W32_RGN_DIFF) {
        /* a minus b, per rect of a, as up to 4 bands per b-rect -- the
         * classic rectangle-subtraction carve. */
        for (int i = 0; i < a->n; i++) {
            W32_RECT cur[8];
            int cn = 1;
            cur[0] = a->r[i];
            for (int j = 0; j < b->n && cn > 0; j++) {
                int k = 0;
                W32_RECT next[8];
                for (int c = 0; c < cn; c++) {
                    int32_t l = cur[c].left, t = cur[c].top,
                            r = cur[c].right, bt = cur[c].bottom;
                    int32_t bl = b->r[j].left, btp = b->r[j].top,
                            br = b->r[j].right, bb = b->r[j].bottom;
                    if (r <= bl || br <= l || bt <= btp || bb <= t) {
                        if (k < 8) next[k++] = cur[c];
                        continue;
                    }
                    if (btp > t) {
                        if (k < 8) { next[k].left = l; next[k].top = t;
                                     next[k].right = r; next[k].bottom = btp; k++; }
                        t = btp;
                    }
                    if (bb < bt) {
                        if (k < 8) { next[k].left = l; next[k].top = bb;
                                     next[k].right = r; next[k].bottom = bt; k++; }
                        bt = bb;
                    }
                    if (bl > l) {
                        if (k < 8) { next[k].left = l; next[k].top = t;
                                     next[k].right = bl; next[k].bottom = bt; k++; }
                        l = bl;
                    }
                    if (br < r) {
                        if (k < 8) { next[k].left = br; next[k].top = t;
                                     next[k].right = r; next[k].bottom = bt; k++; }
                        r = br;
                    }
                }
                memcpy(cur, next, (size_t)k * sizeof(W32_RECT));
                cn = k;
            }
            for (int c = 0; c < cn && n < GDI_RGN_MAX; c++) dst->r[n++] = cur[c];
        }
    }
    dst->n = n;
}

W32ABI int32_t CombineRgn(W32_HRGN dst, W32_HRGN ha, W32_HRGN hb, int32_t mode) {
    gdi_rgn_t *d = rgn_from_h(dst);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_RGN_ERROR; }
    if (mode == W32_RGN_COPY) {
        gdi_rgn_t *a = rgn_from_h(ha);
        if (!a) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_RGN_ERROR; }
        memcpy(d->r, a->r, (size_t)a->n * sizeof(W32_RECT));
        d->n = a->n;
    } else if (mode == W32_RGN_AND || mode == W32_RGN_OR || mode == W32_RGN_DIFF ||
               mode == W32_RGN_XOR) {
        gdi_rgn_t *a = rgn_from_h(ha), *b = rgn_from_h(hb);
        if (!a || !b) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_RGN_ERROR; }
        if (mode == W32_RGN_XOR) {
            /* (a - b) then (b - a) appended: membership predicate holds */
            gdi_rgn_t ab, ba;
            rgn_clip_list(&ab, a, b, W32_RGN_DIFF);
            rgn_clip_list(&ba, b, a, W32_RGN_DIFF);
            int n = 0;
            for (int i = 0; i < ab.n && n < GDI_RGN_MAX; i++) d->r[n++] = ab.r[i];
            for (int i = 0; i < ba.n && n < GDI_RGN_MAX; i++) d->r[n++] = ba.r[i];
            d->n = n;
        } else {
            rgn_clip_list(d, a, b, mode);
        }
    } else {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_RGN_ERROR;
    }
    if (d->n == 0) return W32_NULLREGION;
    return d->n == 1 ? W32_SIMPLEREGION : W32_COMPLEXREGION;
}

W32ABI int32_t SelectClipRgn(W32_HDC hdc, W32_HRGN rgn) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_RGN_ERROR; }
    if (!rgn) {                            /* NULL: clip to everything */
        d->clip_n = -1;
        return W32_SIMPLEREGION;
    }
    gdi_rgn_t *r = rgn_from_h(rgn);
    if (!r) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_RGN_ERROR; }
    int n = r->n < GDI_RGN_MAX ? r->n : GDI_RGN_MAX;
    memcpy(d->clip, r->r, (size_t)n * sizeof(W32_RECT));
    d->clip_n = n;
    if (n == 0) return W32_NULLREGION;
    return n == 1 ? W32_SIMPLEREGION : W32_COMPLEXREGION;
}

W32ABI int32_t GetClipRgn(W32_HDC hdc, W32_HRGN rgn) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (!rgn) return d->clip_n < 0 ? 0 : d->clip_n;
    gdi_rgn_t *r = rgn_from_h(rgn);
    if (!r) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (d->clip_n < 0) { r->n = 0; return 0; }
    memcpy(r->r, d->clip, (size_t)d->clip_n * sizeof(W32_RECT));
    r->n = d->clip_n;
    return d->clip_n;
}

W32ABI int32_t ExcludeClipRect(W32_HDC hdc, int32_t l, int32_t t, int32_t r, int32_t b) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_RGN_ERROR; }
    gdi_rgn_t cur;
    if (d->clip_n < 0) {
        cur.n = 1;
        cur.r[0].left = -0x7FFFFFFF; cur.r[0].top = -0x7FFFFFFF;
        cur.r[0].right = 0x7FFFFFFF; cur.r[0].bottom = 0x7FFFFFFF;
    } else {
        memcpy(cur.r, d->clip, (size_t)d->clip_n * sizeof(W32_RECT));
        cur.n = d->clip_n;
    }
    gdi_rgn_t hole;
    hole.n = 1;
    hole.r[0].left = l; hole.r[0].top = t;
    hole.r[0].right = r; hole.r[0].bottom = b;
    gdi_rgn_t out;
    rgn_clip_list(&out, &cur, &hole, W32_RGN_DIFF);
    int n = out.n < GDI_RGN_MAX ? out.n : GDI_RGN_MAX;
    memcpy(d->clip, out.r, (size_t)n * sizeof(W32_RECT));
    d->clip_n = n;
    return n == 0 ? W32_NULLREGION : (n == 1 ? W32_SIMPLEREGION : W32_COMPLEXREGION);
}

W32ABI int32_t IntersectClipRect(W32_HDC hdc, int32_t l, int32_t t, int32_t r, int32_t b) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_RGN_ERROR; }
    gdi_rgn_t add;
    add.n = 1;
    add.r[0].left = l; add.r[0].top = t;
    add.r[0].right = r; add.r[0].bottom = b;
    if (d->clip_n < 0) {
        memcpy(d->clip, add.r, sizeof(W32_RECT));
        d->clip_n = 1;
    } else {
        gdi_rgn_t cur, out;
        memcpy(cur.r, d->clip, (size_t)d->clip_n * sizeof(W32_RECT));
        cur.n = d->clip_n;
        rgn_clip_list(&out, &cur, &add, W32_RGN_AND);
        int n = out.n < GDI_RGN_MAX ? out.n : GDI_RGN_MAX;
        memcpy(d->clip, out.r, (size_t)n * sizeof(W32_RECT));
        d->clip_n = n;
    }
    return d->clip_n == 0 ? W32_NULLREGION
         : (d->clip_n == 1 ? W32_SIMPLEREGION : W32_COMPLEXREGION);
}

W32ABI W32_BOOL RectVisible(W32_HDC hdc, const W32_RECT *r) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d || !r) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    return clip_intersects(d, r) ? 1 : 0;
}

/* ===================================================================== *
 * Shapes                                                                *
 * ===================================================================== */

static int shapes_prep(W32_HDC hdc, surf_t *s) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return -1; }
    if (d->kind == 1) {
        if (surf_of_dc(d, s) != 0) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return -1; }
        return 0;
    }
    return 1;                               /* window: caller raster-blits */
}

typedef struct { int32_t l, t, r, b, ew, eh; const W32_POINT *pts; int n; int what; } shape_ctx_t;

enum { SHAPE_RECT, SHAPE_ELLIPSE, SHAPE_ROUNDRECT, SHAPE_POLYFILL, SHAPE_POLYLINE };

static void shape_body(surf_t *s, void *user) {
    shape_ctx_t *c = (shape_ctx_t *)user;
    switch (c->what) {
    case SHAPE_RECT:      raster_rect(s, c->l, c->t, c->r, c->b); break;
    case SHAPE_ELLIPSE:   raster_ellipse(s, c->l, c->t, c->r, c->b); break;
    case SHAPE_ROUNDRECT: raster_roundrect(s, c->l, c->t, c->r, c->b, c->ew, c->eh); break;
    case SHAPE_POLYFILL:  raster_polygon(s, c->pts, c->n, 1); break;
    case SHAPE_POLYLINE:  raster_polygon(s, c->pts, c->n, 0); break;
    }
}

static W32_BOOL shape_dispatch(W32_HDC hdc, shape_ctx_t *c, int32_t bx, int32_t by,
                               int32_t bw, int32_t bh) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) return 0;
    if (d->kind == 1) {
        surf_t s;
        if (surf_of_dc(d, &s) != 0) return 0;
        shape_body(&s, c);
        return 1;
    }
    return win_raster_blit(d, bx, by, bw, bh, shape_body, c);
}

static void shape_bounds(const shape_ctx_t *c, int32_t *bx, int32_t *by,
                         int32_t *bw, int32_t *bh) {
    if (c->what == SHAPE_POLYFILL || c->what == SHAPE_POLYLINE) {
        int32_t minx = c->pts[0].x, maxx = c->pts[0].x;
        int32_t miny = c->pts[0].y, maxy = c->pts[0].y;
        for (int i = 1; i < c->n; i++) {
            if (c->pts[i].x < minx) minx = c->pts[i].x;
            if (c->pts[i].x > maxx) maxx = c->pts[i].x;
            if (c->pts[i].y < miny) miny = c->pts[i].y;
            if (c->pts[i].y > maxy) maxy = c->pts[i].y;
        }
        *bx = minx - 2; *by = miny - 2;
        *bw = (maxx - minx) + 5; *bh = (maxy - miny) + 5;
    } else {
        *bx = c->l; *by = c->t;
        *bw = c->r - c->l; *bh = c->b - c->t;
    }
}

W32ABI W32_BOOL Rectangle(W32_HDC hdc, int32_t l, int32_t t, int32_t r, int32_t b) {
    surf_t s;
    if (shapes_prep(hdc, &s) < 0) return 0;
    shape_ctx_t c = { l, t, r, b, 0, 0, 0, 0, SHAPE_RECT };
    int32_t bx, by, bw, bh;
    shape_bounds(&c, &bx, &by, &bw, &bh);
    if (bw <= 0 || bh <= 0) return 1;
    return shape_dispatch(hdc, &c, bx, by, bw, bh);
}

W32ABI W32_BOOL Ellipse(W32_HDC hdc, int32_t l, int32_t t, int32_t r, int32_t b) {
    surf_t s;
    if (shapes_prep(hdc, &s) < 0) return 0;
    shape_ctx_t c = { l, t, r, b, 0, 0, 0, 0, SHAPE_ELLIPSE };
    int32_t bx, by, bw, bh;
    shape_bounds(&c, &bx, &by, &bw, &bh);
    if (bw <= 0 || bh <= 0) return 1;
    return shape_dispatch(hdc, &c, bx, by, bw, bh);
}

W32ABI W32_BOOL RoundRect(W32_HDC hdc, int32_t l, int32_t t, int32_t r, int32_t b,
                          int32_t ew, int32_t eh) {
    surf_t s;
    if (shapes_prep(hdc, &s) < 0) return 0;
    shape_ctx_t c = { l, t, r, b, ew, eh, 0, 0, SHAPE_ROUNDRECT };
    int32_t bx, by, bw, bh;
    shape_bounds(&c, &bx, &by, &bw, &bh);
    if (bw <= 0 || bh <= 0) return 1;
    return shape_dispatch(hdc, &c, bx, by, bw, bh);
}

W32ABI W32_BOOL Polygon(W32_HDC hdc, const W32_POINT *pts, int32_t n) {
    surf_t s;
    if (shapes_prep(hdc, &s) < 0) return 0;
    if (!pts || n < 2) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    shape_ctx_t c = { 0, 0, 0, 0, 0, 0, pts, n, SHAPE_POLYFILL };
    int32_t bx, by, bw, bh;
    shape_bounds(&c, &bx, &by, &bw, &bh);
    return shape_dispatch(hdc, &c, bx, by, bw, bh);
}

W32ABI W32_BOOL Polyline(W32_HDC hdc, const W32_POINT *pts, int32_t n) {
    surf_t s;
    if (shapes_prep(hdc, &s) < 0) return 0;
    if (!pts || n < 2) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    shape_ctx_t c = { 0, 0, 0, 0, 0, 0, pts, n, SHAPE_POLYLINE };
    int32_t bx, by, bw, bh;
    shape_bounds(&c, &bx, &by, &bw, &bh);
    return shape_dispatch(hdc, &c, bx, by, bw, bh);
}

/* FrameRect is a user32 export (the ledger keeps it there) but the
 * raster body is ours; user32.c's wrapper calls through.  It paints a
 * 1-pixel border with the brush -- solid brushes as their colour, hatch
 * brushes dithered by the same anchored sampler as fills. */
typedef struct { int32_t l, t, r, b; gdi_brush_t *br; } frame_ctx_t;

static void frame_span(surf_t *s, int32_t x0, int32_t x1, int32_t y,
                       gdi_brush_t *br) {
    for (int32_t x = x0; x < x1; x++) {
        uint32_t c = brush_color_at(s->dc, br, x, y);
        if (c == GDI_BRUSH_SKIP) continue;
        surf_put(s, x, y, c);
    }
}

static void frame_body(surf_t *s, void *user) {
    frame_ctx_t *c = (frame_ctx_t *)user;
    frame_span(s, c->l, c->r, c->t, c->br);
    frame_span(s, c->l, c->r, c->b - 1, c->br);
    frame_span(s, c->l, c->l + 1, c->t, c->br);
    frame_span(s, c->l, c->l + 1, c->b, c->br);
    frame_span(s, c->r - 1, c->r, c->t, c->br);
    frame_span(s, c->r - 1, c->r, c->b, c->br);
}

W32ABI int32_t FrameRect(W32_HDC hdc, const W32_RECT *r, W32_HBRUSH brush) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d || !r) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (r->right <= r->left || r->bottom <= r->top) return 0;
    gdi_brush_t frameb;
    gdi_brush_t *br = dc_brush(d);
    if (brush && (uintptr_t)brush >= GDI_HANDLE_BIAS) {
        gdi_obj_t *o = obj_from_h(brush, GOBJ_BRUSH);
        if (o) br = &o->u.brush;
    } else if (brush) {
        /* the A-5 raw-colour convention, decoded */
        frameb.style = GDI_BS_SOLID;
        frameb.color = (W32_DWORD)(uintptr_t)brush;
        br = &frameb;
    }
    frame_ctx_t c;
    c.l = r->left; c.t = r->top; c.r = r->right; c.b = r->bottom;
    c.br = br;
    if (d->kind == 1) {
        surf_t s;
        if (surf_of_dc(d, &s) != 0) return 0;
        frame_body(&s, &c);
        return 1;
    }
    return win_raster_blit(d, r->left, r->top,
                           r->right - r->left, r->bottom - r->top,
                           frame_body, &c) ? 1 : 0;
}

/* ===================================================================== *
 * Fonts and text                                                        *
 * ===================================================================== */

static W32_HFONT font_from_lf(const W32_LOGFONTW *lf) {
    if (!lf) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    gdi_obj_t *o;
    W32_HFONT h = (W32_HFONT)gdi_alloc_obj(GOBJ_FONT, &o);
    if (!h) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    o->u.font.lf = *lf;                    /* the request, verbatim */
    return h;
}

W32ABI W32_HFONT CreateFontIndirectW(const W32_LOGFONTW *lf) {
    return font_from_lf(lf);
}

W32ABI W32_HFONT CreateFontIndirectA(const W32_LOGFONTA *lf) {
    if (!lf) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    W32_LOGFONTW w;
    memset(&w, 0, sizeof w);
    w.lfHeight = lf->lfHeight; w.lfWidth = lf->lfWidth;
    w.lfEscapement = lf->lfEscapement; w.lfOrientation = lf->lfOrientation;
    w.lfWeight = lf->lfWeight;
    w.lfItalic = lf->lfItalic; w.lfUnderline = lf->lfUnderline;
    w.lfStrikeOut = lf->lfStrikeOut; w.lfCharSet = lf->lfCharSet;
    w.lfOutPrecision = lf->lfOutPrecision;
    w.lfClipPrecision = lf->lfClipPrecision;
    w.lfQuality = lf->lfQuality;
    w.lfPitchAndFamily = lf->lfPitchAndFamily;
    for (int i = 0; i < 31 && lf->lfFaceName[i]; i++)
        w.lfFaceName[i] = (uint8_t)lf->lfFaceName[i];
    return font_from_lf(&w);
}

W32ABI W32_HFONT CreateFontW(int32_t h, int32_t w, int32_t esc, int32_t ori,
                              int32_t weight, W32_DWORD italic, W32_DWORD underline,
                              W32_DWORD strikeout, W32_DWORD charset,
                              W32_DWORD outprec, W32_DWORD clipprec,
                              W32_DWORD quality, W32_DWORD pitch,
                              const uint16_t *face) {
    W32_LOGFONTW lf;
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = h; lf.lfWidth = w;
    lf.lfEscapement = esc; lf.lfOrientation = ori;
    lf.lfWeight = weight;
    lf.lfItalic = (uint8_t)italic; lf.lfUnderline = (uint8_t)underline;
    lf.lfStrikeOut = (uint8_t)strikeout; lf.lfCharSet = (uint8_t)charset;
    lf.lfOutPrecision = (uint8_t)outprec;
    lf.lfClipPrecision = (uint8_t)clipprec;
    lf.lfQuality = (uint8_t)quality;
    lf.lfPitchAndFamily = (uint8_t)pitch;
    if (face)
        for (int i = 0; i < 31 && face[i]; i++) lf.lfFaceName[i] = face[i];
    return font_from_lf(&lf);
}

W32ABI W32_HFONT CreateFontA(int32_t h, int32_t w, int32_t esc, int32_t ori,
                              int32_t weight, W32_DWORD italic, W32_DWORD underline,
                              W32_DWORD strikeout, W32_DWORD charset,
                              W32_DWORD outprec, W32_DWORD clipprec,
                              W32_DWORD quality, W32_DWORD pitch,
                              const char *face) {
    uint16_t wface[32];
    memset(wface, 0, sizeof wface);
    if (face)
        for (int i = 0; i < 31 && face[i]; i++) wface[i] = (uint8_t)face[i];
    return CreateFontW(h, w, esc, ori, weight, italic, underline, strikeout,
                       charset, outprec, clipprec, quality, pitch, wface);
}

static void fill_tm_common(int32_t *height, int32_t *ascent, int32_t *descent,
                           int32_t *internal_leading, int32_t *ave, int32_t *maxw,
                           uint16_t *first, uint16_t *last,
                           uint16_t *def, uint16_t *brk) {
    const gdi_font_face_t *f = gdi_face();
    if (!f) {                              /* unreachable: blob is shipped */
        *height = 16; *ascent = 14; *descent = 2;
        *internal_leading = 0; *ave = 8; *maxw = 8;
        *first = 0; *last = 255; *def = 32; *brk = 32;
        return;
    }
    *height = (int32_t)f->h;
    *ascent = (int32_t)f->ascent;
    *descent = (int32_t)f->descent;
    *internal_leading = 0;                 /* no OS/2 table in a PSF2 blob */
    *ave = (int32_t)f->w;
    *maxw = (int32_t)f->w;
    *first = 0;
    *last = (uint16_t)(f->glyphs - 1);
    *def = 32;
    *brk = 32;
}

W32ABI W32_BOOL GetTextMetricsW(W32_HDC hdc, W32_TEXTMETRICW *tm) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d || !tm) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    memset(tm, 0, sizeof *tm);
    fill_tm_common(&tm->tmHeight, &tm->tmAscent, &tm->tmDescent,
                   &tm->tmInternalLeading, &tm->tmAveCharWidth,
                   &tm->tmMaxCharWidth, &tm->tmFirstChar, &tm->tmLastChar,
                   &tm->tmDefaultChar, &tm->tmBreakChar);
    gdi_font_t *f = dc_font(d);
    tm->tmWeight = f->lf.lfWeight ? f->lf.lfWeight : 400;
    tm->tmOverhang = 0;
    tm->tmDigitizedAspectX = (int32_t)GetDeviceCaps(hdc, W32_LOGPIXELSX);
    tm->tmDigitizedAspectY = (int32_t)GetDeviceCaps(hdc, W32_LOGPIXELSY);
    tm->tmItalic = f->lf.lfItalic;
    tm->tmUnderlined = f->lf.lfUnderline;
    tm->tmStruckOut = f->lf.lfStrikeOut;
    tm->tmPitchAndFamily = f->lf.lfPitchAndFamily ? f->lf.lfPitchAndFamily
                                                  : 0x01;      /* FIXED_PITCH */
    tm->tmCharSet = f->lf.lfCharSet ? f->lf.lfCharSet : W32_ANSI_CHARSET;
    return 1;
}

W32ABI W32_BOOL GetTextMetricsA(W32_HDC hdc, W32_TEXTMETRICA *tm) {
    W32_TEXTMETRICW w;
    if (!GetTextMetricsW(hdc, &w)) return 0;
    tm->tmHeight = w.tmHeight; tm->tmAscent = w.tmAscent;
    tm->tmDescent = w.tmDescent; tm->tmInternalLeading = w.tmInternalLeading;
    tm->tmExternalLeading = w.tmExternalLeading;
    tm->tmAveCharWidth = w.tmAveCharWidth; tm->tmMaxCharWidth = w.tmMaxCharWidth;
    tm->tmWeight = w.tmWeight; tm->tmOverhang = w.tmOverhang;
    tm->tmDigitizedAspectX = w.tmDigitizedAspectX;
    tm->tmDigitizedAspectY = w.tmDigitizedAspectY;
    tm->tmFirstChar = (uint8_t)w.tmFirstChar;
    tm->tmLastChar = (uint8_t)w.tmLastChar;
    tm->tmDefaultChar = (uint8_t)w.tmDefaultChar;
    tm->tmBreakChar = (uint8_t)w.tmBreakChar;
    tm->tmItalic = w.tmItalic; tm->tmUnderlined = w.tmUnderlined;
    tm->tmStruckOut = w.tmStruckOut;
    tm->tmPitchAndFamily = w.tmPitchAndFamily;
    tm->tmCharSet = w.tmCharSet;
    return 1;
}

typedef struct {
    int32_t x, y;
    const uint16_t *str;
    uint32_t n;
    const int32_t *dx;
    const W32_RECT *clip;
} text_ctx_t;

static void text_body(surf_t *s, void *user) {
    text_ctx_t *c = (text_ctx_t *)user;
    raster_text(s, c->x, c->y, c->str, c->n, c->dx, c->clip);
}

static W32_BOOL text_dispatch(W32_HDC hdc, int32_t x, int32_t y,
                              const uint16_t *str, uint32_t n,
                              const int32_t *dx, const W32_RECT *clip) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) return 0;
    const gdi_font_face_t *f = gdi_face();
    if (!f) return 0;
    text_ctx_t c;
    c.x = x; c.y = y; c.str = str; c.n = n; c.dx = dx; c.clip = clip;
    if (d->kind == 1) {
        surf_t s;
        if (surf_of_dc(d, &s) != 0) return 0;
        text_body(&s, &c);
        return 1;
    }
    int32_t w = (int32_t)(n * f->w) + 4, h = (int32_t)f->h + 4;
    int32_t bx = x, by = y;
    uint32_t xa = d->text_align & 6u;
    if (xa == W32_TA_CENTER)      bx -= w / 2;
    else if (xa == W32_TA_RIGHT)  bx -= w;
    uint32_t ya = d->text_align & 24u;
    if (ya == W32_TA_BASELINE)    by -= (int32_t)f->ascent;
    else if (ya == W32_TA_BOTTOM) by -= h;
    if (bx > x) bx = x;
    if (by > y) by = y;
    return win_raster_blit(d, bx, by, w, h, text_body, &c);
}

W32ABI W32_BOOL ExtTextOutW(W32_HDC hdc, int32_t x, int32_t y, W32_UINT flags,
                            const W32_RECT *r, const uint16_t *s, W32_UINT len,
                            const int32_t *dx) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (!s) return 1;
    const W32_RECT *clip = (flags & W32_ETO_CLIPPED) ? r : 0;
    const gdi_font_face_t *f = gdi_face();
    if ((flags & W32_ETO_OPAQUE) && r && f) {
        /* ETO opaquing paints the rectangle with the DC's bk colour
         * before the glyphs -- with the DC clip honoured. */
        uint32_t bk = w32_colorref_to_ag(d->bk_color);
        if (d->kind == 1) {
            surf_t su;
            if (surf_of_dc(d, &su) == 0) {
                for (int32_t yy = r->top; yy < r->bottom; yy++)
                    for (int32_t xx = r->left; xx < r->right; xx++)
                        surf_put(&su, xx, yy, bk);
            }
        } else if (d->kind == 0) {
            int wid = w32_win_ag_wid(d->slot);
            if (wid >= 0)
                for (int32_t yy = r->top; yy < r->bottom; yy++)
                    for (int32_t xx = r->left; xx < r->right; xx++)
                        if (clip_has(d, xx, yy))
                            ag_fill_rect(wid, xx, yy, 1, 1, bk);
        }
    }
    return text_dispatch(hdc, x, y, s, len, dx, clip);
}

W32ABI W32_BOOL ExtTextOutA(W32_HDC hdc, int32_t x, int32_t y, W32_UINT flags,
                            const W32_RECT *r, const char *s, W32_UINT len,
                            const int32_t *dx) {
    if (!s) return 1;
    uint16_t wb[512];
    uint32_t n = 0;
    while (n < len && n < 511) { wb[n] = (uint8_t)s[n]; n++; }
    wb[n] = 0;
    return ExtTextOutW(hdc, x, y, flags, r, wb, n, dx);
}

W32ABI W32_BOOL TextOutW(W32_HDC hdc, int32_t x, int32_t y,
                         const uint16_t *s, int32_t len) {
    if (!s) return 1;
    if (len < 0) {
        int n = 0;
        while (s[n] && n < 511) n++;
        len = n;
    }
    if (len > 511) len = 511;
    return ExtTextOutW(hdc, x, y, 0, 0, s, (uint32_t)len, 0);
}

W32ABI W32_BOOL TextOutA(W32_HDC hdc, int32_t x, int32_t y,
                         const char *s, int32_t len) {
    if (!s) return 1;
    uint16_t wb[512];
    int n = 0;
    if (len < 0) { while (s[n] && n < 511) { wb[n] = (uint8_t)s[n]; n++; } }
    else { while (n < len && n < 511) { wb[n] = (uint8_t)s[n]; n++; } }
    wb[n] = 0;
    return ExtTextOutW(hdc, x, y, 0, 0, wb, (uint32_t)n, 0);
}

/* ---- extents / widths -------------------------------------------------- */
static W32_BOOL extent_of(const uint16_t *s, int32_t len, W32_SIZE *sz) {
    const gdi_font_face_t *f = gdi_face();
    if (!sz || !f) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    int n = len;
    if (n < 0) { n = 0; while (s[n] && n < 4096) n++; }
    sz->cx = (int32_t)((uint32_t)n * f->w);
    sz->cy = (int32_t)f->h;
    return 1;
}

W32ABI W32_BOOL GetTextExtentPoint32W(W32_HDC hdc, const uint16_t *s,
                                      int32_t len, W32_SIZE *sz) {
    (void)hdc;
    if (!s) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    return extent_of(s, len, sz);
}
W32ABI W32_BOOL GetTextExtentPoint32A(W32_HDC hdc, const char *s,
                                      int32_t len, W32_SIZE *sz) {
    (void)hdc;
    if (!s) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    uint16_t wb[512];
    int n = 0;
    if (len < 0) { while (s[n] && n < 511) { wb[n] = (uint8_t)s[n]; n++; } }
    else { while (n < len && n < 511) { wb[n] = (uint8_t)s[n]; n++; } }
    wb[n] = 0;
    return extent_of(wb, n, sz);
}
W32ABI W32_BOOL GetTextExtentPointW(W32_HDC hdc, const uint16_t *s,
                                    int32_t len, W32_SIZE *sz) {
    return GetTextExtentPoint32W(hdc, s, len, sz);
}
W32ABI W32_BOOL GetTextExtentPointA(W32_HDC hdc, const char *s,
                                    int32_t len, W32_SIZE *sz) {
    return GetTextExtentPoint32A(hdc, s, len, sz);
}

W32ABI W32_BOOL GetTextExtentExPointW(W32_HDC hdc, const uint16_t *s,
                                      int32_t len, int32_t max_extent,
                                      int32_t *fit, int32_t *dx, W32_SIZE *sz) {
    (void)hdc;
    const gdi_font_face_t *f = gdi_face();
    if (!f || !s) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    int n = len;
    if (n < 0) { n = 0; while (s[n] && n < 4096) n++; }
    int k = 0;
    while (k < n && (int32_t)((uint32_t)(k + 1) * f->w) <= max_extent) k++;
    if (fit) *fit = k;
    if (dx)
        for (int i = 0; i < k; i++) dx[i] = (int32_t)f->w;
    return extent_of(s, len, sz);
}
W32ABI W32_BOOL GetTextExtentExPointA(W32_HDC hdc, const char *s,
                                      int32_t len, int32_t max_extent,
                                      int32_t *fit, int32_t *dx, W32_SIZE *sz) {
    uint16_t wb[512];
    int n = 0;
    if (len < 0) { while (s[n] && n < 511) { wb[n] = (uint8_t)s[n]; n++; } }
    else { while (n < len && n < 511) { wb[n] = (uint8_t)s[n]; n++; } }
    wb[n] = 0;
    return GetTextExtentExPointW(hdc, wb, n, max_extent, fit, dx, sz);
}

W32ABI W32_BOOL GetCharWidthW(W32_HDC hdc, W32_UINT first, W32_UINT last,
                              int32_t *buf) {
    (void)hdc;
    const gdi_font_face_t *f = gdi_face();
    if (!f || !buf || last < first || last - first > 255) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (W32_UINT c = first; c <= last; c++)
        buf[c - first] = (c < f->glyphs) ? (int32_t)f->w : 0;
    return 1;
}
W32ABI W32_BOOL GetCharWidthA(W32_HDC hdc, W32_UINT first, W32_UINT last,
                              int32_t *buf) {
    return GetCharWidthW(hdc, first, last, buf);
}
W32ABI W32_BOOL GetCharWidth32W(W32_HDC hdc, W32_UINT first, W32_UINT last,
                                int32_t *buf) {
    return GetCharWidthW(hdc, first, last, buf);
}
W32ABI W32_BOOL GetCharWidth32A(W32_HDC hdc, W32_UINT first, W32_UINT last,
                                int32_t *buf) {
    return GetCharWidthW(hdc, first, last, buf);
}

W32ABI W32_BOOL GetCharABCWidthsFloatA(W32_HDC hdc, W32_UINT first, W32_UINT last,
                                       W32_ABCFLOAT *buf) {
    (void)hdc;
    const gdi_font_face_t *f = gdi_face();
    if (!f || !buf || last < first || last - first > 255) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (W32_UINT c = first; c <= last; c++) {
        buf[c - first].abcfA = 0.0f;
        buf[c - first].abcfB = (c < f->glyphs) ? (float)f->w : 0.0f;
        buf[c - first].abcfC = 0.0f;
    }
    return 1;
}

W32ABI W32_UINT GetOutlineTextMetricsA(W32_HDC hdc, W32_UINT cb,
                                       W32_OUTLINETEXTMETRICA *otm) {
    const gdi_font_face_t *f = gdi_face();
    if (!f) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    char face_a[32];
    int fn = 0;
    while (face_name_w[fn] && fn < 31) { face_a[fn] = (char)face_name_w[fn]; fn++; }
    face_a[fn] = 0;
    char style_a[8] = "Regular";
    /* names layout after the fixed part: face, style, family, full --
     * the size MUST cover all four (the full name is last and longest) */
    uint32_t off_face = (uint32_t)sizeof(W32_OUTLINETEXTMETRICA);
    uint32_t off_style = off_face + (uint32_t)strlen(face_a) + 1;
    uint32_t off_family = off_style + (uint32_t)strlen(style_a) + 1;
    uint32_t off_full = off_family + (uint32_t)strlen(face_a) + 1;
    uint32_t need = off_full + (uint32_t)strlen(face_a) + 1;
    if (!otm || cb < need) return need;     /* size query contract */
    memset(otm, 0, cb);
    otm->otmSize = need;
    fill_tm_common(&otm->otmTextMetrics.tmHeight,
                   &otm->otmTextMetrics.tmAscent,
                   &otm->otmTextMetrics.tmDescent,
                   &otm->otmTextMetrics.tmInternalLeading,
                   &otm->otmTextMetrics.tmAveCharWidth,
                   &otm->otmTextMetrics.tmMaxCharWidth,
                   &otm->otmTextMetrics.tmFirstChar,
                   &otm->otmTextMetrics.tmLastChar,
                   &otm->otmTextMetrics.tmDefaultChar,
                   &otm->otmTextMetrics.tmBreakChar);
    otm->otmTextMetrics.tmWeight = 400;
    otm->otmTextMetrics.tmDigitizedAspectX = (int32_t)GetDeviceCaps(hdc, W32_LOGPIXELSX);
    otm->otmTextMetrics.tmDigitizedAspectY = (int32_t)GetDeviceCaps(hdc, W32_LOGPIXELSY);
    otm->otmTextMetrics.tmPitchAndFamily = 0x01;
    otm->otmTextMetrics.tmCharSet = W32_ANSI_CHARSET;
    otm->otmfsSelection = 0;               /* regular */
    otm->otmfsType = 0;
    otm->otmsCharSlopeRise = 1; otm->otmsCharSlopeRun = 0;
    otm->otmEMSquare = 2048;
    otm->otmAscent = (int32_t)f->ascent;
    otm->otmDescent = -(int32_t)f->descent;
    otm->otmLineGap = 0;
    otm->otmsCapEmHeight = (int32_t)f->ascent;
    otm->otmsXHeight = 8;
    otm->otmrcFontBox.left = 0;
    otm->otmrcFontBox.top = (int32_t)f->ascent;
    otm->otmrcFontBox.right = (int32_t)f->w;
    otm->otmrcFontBox.bottom = -(int32_t)f->descent;
    otm->otmMacAscent = 1000 * (int32_t)f->ascent / (int32_t)f->h;
    otm->otmMacDescent = -1000 * (int32_t)f->descent / (int32_t)f->h;
    otm->otmMacLineGap = 0;
    otm->otmusMinimumPPEM = 8;
    otm->otmptSubscriptSize.x = 6; otm->otmptSubscriptSize.y = 10;
    otm->otmptSubscriptOffset.x = 0; otm->otmptSubscriptOffset.y = 6;
    otm->otmptSuperscriptSize.x = 6; otm->otmptSuperscriptSize.y = 10;
    otm->otmptSuperscriptOffset.x = 0; otm->otmptSuperscriptOffset.y = 10;
    otm->otmsStrikeoutSize = 1;
    otm->otmsStrikeoutPosition = 11;
    otm->otmsUnderlineSize = 1;
    otm->otmsUnderlinePosition = -2;
    /* names right after the fixed part, offsets from struct start */
    char *p = (char *)otm + sizeof(W32_OUTLINETEXTMETRICA);
    memcpy(p, face_a, strlen(face_a) + 1);
    memcpy((char *)otm + off_style, style_a, strlen(style_a) + 1);
    memcpy((char *)otm + off_family, face_a, strlen(face_a) + 1);
    memcpy((char *)otm + off_full, face_a, strlen(face_a) + 1);
    otm->otmFaceName = off_face;
    otm->otmStyleName = off_style;
    otm->otmFamilyName = off_family;
    otm->otmFullName = off_full;
    return need;
}

typedef int (W32ABI *w32_enumfont_proc_t)(const W32_ENUMLOGFONTEXW *,
                                          const W32_TEXTMETRICW *,
                                          W32_DWORD, W32_LPARAM);

W32ABI int32_t EnumFontFamiliesExW(W32_HDC hdc, const W32_LOGFONTW *lf,
                                   void *proc, W32_LPARAM lparam, W32_DWORD flags) {
    (void)lf; (void)flags;
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d || !proc) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 1; }
    w32_enumfont_proc_t cb = (w32_enumfont_proc_t)proc;
    W32_ENUMLOGFONTEXW elf;
    W32_TEXTMETRICW tm;
    memset(&elf, 0, sizeof elf);
    memset(&tm, 0, sizeof tm);
    fill_tm_common(&tm.tmHeight, &tm.tmAscent, &tm.tmDescent,
                   &tm.tmInternalLeading, &tm.tmAveCharWidth, &tm.tmMaxCharWidth,
                   &tm.tmFirstChar, &tm.tmLastChar, &tm.tmDefaultChar,
                   &tm.tmBreakChar);
    tm.tmWeight = 400;
    tm.tmDigitizedAspectX = (int32_t)GetDeviceCaps(hdc, W32_LOGPIXELSX);
    tm.tmDigitizedAspectY = (int32_t)GetDeviceCaps(hdc, W32_LOGPIXELSY);
    tm.tmPitchAndFamily = 0x01;
    tm.tmCharSet = W32_ANSI_CHARSET;
    elf.elfLogFont.lfHeight = 16;
    elf.elfLogFont.lfWeight = 400;
    elf.elfLogFont.lfCharSet = W32_ANSI_CHARSET;
    elf.elfLogFont.lfPitchAndFamily = 0x01;
    for (int i = 0; face_name_w[i]; i++)
        elf.elfLogFont.lfFaceName[i] = face_name_w[i];
    for (int i = 0; face_name_w[i]; i++)
        elf.elfFullName[i] = face_name_w[i];
    for (int i = 0; face_style_w[i]; i++)
        elf.elfStyle[i] = face_style_w[i];
    /* one face: the shipped one -- real enumeration, one member */
    return cb(&elf, &tm, 0, lparam);
}

W32ABI W32_DWORD GetCharacterPlacementW(W32_HDC hdc, const uint16_t *s,
                                        int32_t n, int32_t max_ext,
                                        W32_GCP_RESULTSW *res, W32_DWORD flags) {
    (void)hdc;
    const gdi_font_face_t *f = gdi_face();
    if (!f || !s || !res) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    if (n < 0) n = 0;
    if (res->lpDx)
        for (int i = 0; i < n; i++) res->lpDx[i] = (int32_t)f->w;
    if (res->lpGlyphs)
        for (int i = 0; i < n && (uint32_t)i < res->nGlyphs; i++)
            res->lpGlyphs[i] = (uint16_t)s[i];
    if (flags & 0x00000008u /* GCP_MAXEXTENT */) {
        int k = 0;
        while (k < n && (int32_t)((uint32_t)(k + 1) * f->w) <= max_ext) k++;
        res->nMaxFit = k;
        n = k;
    } else {
        res->nMaxFit = n;
    }
    res->nGlyphs = (uint32_t)n;
    return ((uint32_t)n << 16) | 0x0001 | 0x0002;   /* glyphs + class set */
}

W32ABI W32_BOOL TranslateCharsetInfo(W32_DWORD *src, W32_CHARSETINFO *cs,
                                     W32_DWORD flags) {
    if (!cs) return 0;
    memset(cs, 0, sizeof *cs);
    uint32_t charset;
    if (flags == W32_TCI_SRCCHARSET) {
        if (!src) return 0;
        charset = *src;
    } else if (flags == W32_TCI_SRCCODEPAGE) {
        if (!src) return 0;
        charset = (*src == 1252u) ? W32_ANSI_CHARSET : 0xFFu;
    } else {
        charset = W32_ANSI_CHARSET;
    }
    switch (charset) {
    case W32_ANSI_CHARSET:
    case W32_DEFAULT_CHARSET:
        cs->ciCharset = W32_ANSI_CHARSET;
        cs->ciACP = 1252;
        cs->fsCsb[0] = 0x0001u;
        break;
    case W32_OEM_CHARSET:
        cs->ciCharset = W32_OEM_CHARSET;
        cs->ciACP = 437;
        cs->fsCsb[0] = 0x0002u;
        break;
    default:
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
}

/* ===================================================================== *
 * Palettes                                                              *
 * ===================================================================== */

static gdi_pal_t *pal_from_h(W32_HPALETTE h) {
    gdi_obj_t *o = obj_from_h(h, GOBJ_PALETTE);
    return o ? &o->u.pal : 0;
}

W32ABI W32_HPALETTE CreatePalette(const W32_LOGPALETTE *lp) {
    if (!lp || lp->palNumEntries == 0 || lp->palNumEntries > GDI_CT_MAX) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    gdi_obj_t *o;
    W32_HPALETTE h = (W32_HPALETTE)gdi_alloc_obj(GOBJ_PALETTE, &o);
    if (!h) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    o->u.pal.n = lp->palNumEntries;
    memcpy(o->u.pal.e, lp->palPalEntry,
           (size_t)lp->palNumEntries * sizeof(W32_PALETTEENTRY));
    return h;
}

W32ABI W32_HPALETTE SelectPalette(W32_HDC hdc, W32_HPALETTE pal, W32_BOOL bkgnd) {
    (void)bkgnd;
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d || !pal_from_h(pal)) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return 0;
    }
    W32_HPALETTE old = d->pal ? (W32_HPALETTE)(uintptr_t)d->pal
                              : (W32_HPALETTE)GetStockObject(W32_DEFAULT_PALETTE);
    d->pal = (uint32_t)(uintptr_t)pal;
    return old;
}

W32ABI W32_UINT RealizePalette(W32_HDC hdc) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    gdi_pal_t *p = d->pal ? pal_from_h((W32_HPALETTE)(uintptr_t)d->pal) : 0;
    if (!p) return 0;
    if (p->realized) return 0;             /* already current: no churn */
    p->realized = 1;
    /* On a memory DC with an 8bpp bitmap selected, realising rewrites
     * the bitmap's colour table -- the honest 8-bit path. */
    gdi_bitmap_t *b = dc_bmp(d);
    if (b && b->bpp == 8) {
        int n = p->n < GDI_CT_MAX ? p->n : GDI_CT_MAX;
        for (int i = 0; i < n; i++) {
            b->ct[i].red = p->e[i].peRed;
            b->ct[i].green = p->e[i].peGreen;
            b->ct[i].blue = p->e[i].peBlue;
            b->ct[i].reserved = 0;
        }
        b->ct_n = n;
        return (W32_UINT)n;
    }
    /* 32bpp surfaces: the palette maps to nothing; count is reported
     * but no remap happens (documented, D9). */
    return (W32_UINT)p->n;
}

W32ABI W32_UINT SetPaletteEntries(W32_HPALETTE pal, W32_UINT start,
                                  W32_UINT count, const W32_PALETTEENTRY *ent) {
    gdi_pal_t *p = pal_from_h(pal);
    if (!p || !ent || start + count > (W32_UINT)p->n || count == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    memcpy(&p->e[start], ent, (size_t)count * sizeof(W32_PALETTEENTRY));
    p->realized = 0;
    return count;
}

W32ABI W32_BOOL UpdateColors(W32_HDC hdc) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    /* no palette animation exists to flush; the call is a success that
     * changes nothing, which is the true statement */
    return 1;
}

/* ===================================================================== *
 * Printing: fail-clean refusals (AuraLite has no printers)              *
 * ===================================================================== */

W32ABI int32_t StartDocW(W32_HDC hdc, const W32_DOCINFOW *di) {
    (void)hdc; (void)di;
    w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
    return W32_SP_ERROR;
}
W32ABI int32_t EndDoc(W32_HDC hdc) {
    (void)hdc;
    w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
    return W32_SP_ERROR;
}
W32ABI int32_t StartPage(W32_HDC hdc) {
    (void)hdc;
    w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
    return W32_SP_ERROR;
}
W32ABI int32_t EndPage(W32_HDC hdc) {
    (void)hdc;
    w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
    return W32_SP_ERROR;
}

/* ===================================================================== *
 * Icons: decode + draw (the W32A-6 loader contract)                     *
 * ===================================================================== *
 * LoadIcon* (w32_rsrc.c) still hand back the raw resource blob pointer;
 * that contract survives A-7.  DrawIconEx decodes it into an ARGB icon
 * object through a tiny pointer-keyed cache, then rasterises with the
 * same surf path as everything else -- icons on memory DCs and window
 * DCs agree by construction, and a repeated draw costs a lookup. */

#define GDI_ICON_CACHE 8
static struct {
    const uint8_t *blob;
    size_t len;
    W32_HICON obj;
} icon_cache[GDI_ICON_CACHE];

static W32_HICON icon_obj_alloc(int32_t w, int32_t hgt, gdi_obj_t **out) {
    size_t alloc = (size_t)w * (size_t)hgt * 4u;
    uint32_t *argb = (uint32_t *)pool_alloc(alloc);
    if (!argb) return 0;
    memset(argb, 0, alloc);
    gdi_obj_t *o;
    W32_HICON hi = (W32_HICON)gdi_alloc_obj(GOBJ_ICON, &o);
    if (!hi) { pool_free(argb, alloc); return 0; }
    o->u.icon.w = w; o->u.icon.h = hgt;
    o->u.icon.argb = argb; o->u.icon.alloc = alloc;
    if (out) *out = o;
    return hi;
}

/* W32A-8: mint an icon object straight from ARGB pixels -- the reverse
 * of the decode, what ImageList_GetIcon needs to hand a caller an HICON
 * for one image-list cell.  The pixels are copied into the object's own
 * pool block, so the caller's buffer may die immediately. */
W32_HICON w32_gdi_icon_from_argb(int32_t w, int32_t hgt, const uint32_t *argb) {
    if (w <= 0 || hgt <= 0 || !argb) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    gdi_obj_t *o;
    W32_HICON hi = icon_obj_alloc(w, hgt, &o);
    if (!hi) return 0;
    memcpy(o->u.icon.argb, argb, (size_t)w * (size_t)hgt * 4u);
    return hi;
}

/* W32A-8: the read side of that contract -- what ImageList_ReplaceIcon
 * needs to pull one icon's pixels into an image-list cell.  The pixels
 * stay owned by the icon object; callers copy out immediately. */
const uint32_t *w32_gdi_icon_pixels(W32_HICON hicon, int32_t *w, int32_t *hgt) {
    gdi_obj_t *o = obj_from_h(hicon, GOBJ_ICON);
    if (!o || !w || !hgt) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    *w = o->u.icon.w; *hgt = o->u.icon.h;
    return o->u.icon.argb;
}

W32_HICON w32_gdi_icon_decode(const uint8_t *bytes, size_t len) {
    if (!bytes || len < 40) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    /* already decoded?  the blob address is the identity */
    for (int i = 0; i < GDI_ICON_CACHE; i++)
        if (icon_cache[i].blob == bytes && icon_cache[i].len == len)
            return icon_cache[i].obj;
    /* Follow the two blob shapes the loader produces:
     *  (a) an icon directory (ICONDIR + entries), or
     *  (b) a bare icon image (BITMAPINFOHEADER with biHeight = 2*h). */
    const uint8_t *img = bytes;
    size_t img_len = len;
    if (len >= 6 && bytes[0] == 0 && bytes[1] == 0 &&
        bytes[2] == 1 && bytes[3] == 0) {          /* type 1 = icon */
        uint16_t count = (uint16_t)(bytes[4] | (bytes[5] << 8));
        if (count == 0 || len < (size_t)6 + (size_t)count * 16u) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return 0;
        }
        int best = 0;
        uint32_t best_area = 0;
        for (uint16_t i = 0; i < count; i++) {
            const uint8_t *e = bytes + 6 + (size_t)i * 16u;
            uint32_t w = e[0] ? e[0] : 256u;
            uint32_t h = e[1] ? e[1] : 256u;
            uint32_t area = w * h;
            if (area > best_area) { best_area = area; best = (int)i; }
        }
        /* ICONDIRENTRY: bWidth, bHeight, bColorCount, bReserved (4),
         * wPlanes, wBitCount (4), dwBytesInRes (e[8..11]),
         * dwImageOffset (e[12..15]). */
        const uint8_t *e = bytes + 6 + (size_t)best * 16u;
        uint32_t sz  = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                       ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t off = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                       ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        if (off >= len || sz == 0 || off + (uint64_t)sz > (uint64_t)len) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return 0;
        }
        img = bytes + off;
        img_len = sz;
    }
    if (img_len < 40) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    uint32_t hdr_size = (uint32_t)img[0] | ((uint32_t)img[1] << 8) |
                        ((uint32_t)img[2] << 16) | ((uint32_t)img[3] << 24);
    if (hdr_size < 40) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    int32_t w = (int32_t)((uint32_t)img[4] | ((uint32_t)img[5] << 8) |
                          ((uint32_t)img[6] << 16) | ((uint32_t)img[7] << 24));
    int32_t h2 = (int32_t)((uint32_t)img[8] | ((uint32_t)img[9] << 8) |
                           ((uint32_t)img[10] << 16) | ((uint32_t)img[11] << 24));
    if (w <= 0 || h2 == 0) { w32_set_last_error(W32_ERROR_INVALID_PARAMETER); return 0; }
    int32_t h = h2 / 2;                     /* XOR + AND halves */
    if (h <= 0 || w > 256 || h > 256) {
        w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    uint32_t bpp = (uint32_t)img[14] | ((uint32_t)img[15] << 8);
    uint32_t clr_used = (uint32_t)img[32] | ((uint32_t)img[33] << 8) |
                        ((uint32_t)img[34] << 16) | ((uint32_t)img[35] << 24);
    uint32_t ct_n = bpp <= 8 ? (clr_used ? clr_used : (1u << bpp)) : 0;
    const uint8_t *ct = img + hdr_size;
    uint32_t xor_stride = (((uint32_t)w * bpp + 31u) / 32u) * 4u;
    const uint8_t *xor_bits = ct + ct_n * 4u;
    uint32_t and_stride = (((uint32_t)w + 31u) / 32u) * 4u;
    const uint8_t *and_bits = xor_bits + xor_stride * (uint32_t)h;
    if ((size_t)(and_bits - img) + (size_t)and_stride * (size_t)h > img_len) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (bpp != 32 && bpp != 24 && bpp != 8 && bpp != 1) {
        w32_set_last_error(W32_ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    gdi_obj_t *o;
    W32_HICON hicon = icon_obj_alloc(w, h, &o);
    if (!hicon) { w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    uint32_t *dst = o->u.icon.argb;
    for (int32_t y = 0; y < h; y++) {
        int32_t dy = h - 1 - y;             /* DIB rows are bottom-up */
        const uint8_t *xrow = xor_bits + (uint32_t)y * xor_stride;
        const uint8_t *arow = and_bits + (uint32_t)y * and_stride;
        for (int32_t x = 0; x < w; x++) {
            uint8_t r = 0, g = 0, b = 0, a = 255;
            if (bpp == 32) {
                b = xrow[4u * (uint32_t)x + 0];
                g = xrow[4u * (uint32_t)x + 1];
                r = xrow[4u * (uint32_t)x + 2];
                a = xrow[4u * (uint32_t)x + 3];
            } else if (bpp == 24) {
                b = xrow[3u * (uint32_t)x + 0];
                g = xrow[3u * (uint32_t)x + 1];
                r = xrow[3u * (uint32_t)x + 2];
            } else if (bpp == 8) {
                int i = xrow[x];
                if (i < (int)ct_n) {
                    b = ct[4u * (uint32_t)i + 0];
                    g = ct[4u * (uint32_t)i + 1];
                    r = ct[4u * (uint32_t)i + 2];
                }
            } else {                        /* bpp == 1 */
                if (xrow[x >> 3] & (0x80u >> (x & 7))) { r = 255; g = 255; b = 255; }
            }
            if ((arow[x >> 3] & (0x80u >> (x & 7))) && bpp != 32) a = 0;
            dst[dy * w + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                              ((uint32_t)g << 8) | b;
        }
    }
    /* cache the decode: the blob address is the loader's identity for
     * the icon, and DrawIconEx only ever gets that pointer */
    int slot = 0;
    for (int i = 0; i < GDI_ICON_CACHE; i++)
        if (icon_cache[i].blob == bytes) { slot = -1; break; }
        else if (icon_cache[i].blob == 0) { slot = i; break; }
    if (slot >= 0) {
        icon_cache[slot].blob = bytes;
        icon_cache[slot].len = len;
        icon_cache[slot].obj = hicon;
    }
    return hicon;
}

/* The loader (w32_rsrc.c) registers every icon/cursor blob it hands out
 * -- it is the one side that knows the resource size, and registering
 * here is what lets DrawIconEx size a blob it only got a pointer to. */
void w32_gdi_icon_cache_add(const uint8_t *blob, size_t len) {
    if (!blob || !len) return;
    for (int i = 0; i < GDI_ICON_CACHE; i++)
        if (icon_cache[i].blob == blob) return;
    int slot = 0;
    for (int i = 0; i < GDI_ICON_CACHE; i++)
        if (icon_cache[i].blob == 0) { slot = i; break; }
    (void)slot;
    W32_HICON obj = w32_gdi_icon_decode(blob, len);
    if (!obj) return;                       /* decode refused: cache nothing,
                                             * DrawIconEx will refuse too */
}

typedef struct {
    gdi_obj_t *icon;
    int32_t x, y, cx, cy;
} icon_ctx_t;

static void icon_body(surf_t *s, void *user) {
    icon_ctx_t *c = (icon_ctx_t *)user;
    gdi_icon_t *ic = &c->icon->u.icon;
    for (int32_t y = 0; y < c->cy; y++)
        for (int32_t x = 0; x < c->cx; x++) {
            int32_t sx = ic->w > 0 ? x * ic->w / c->cx : 0;
            int32_t sy = ic->h > 0 ? y * ic->h / c->cy : 0;
            uint32_t px = ic->argb[sy * ic->w + sx];
            uint32_t a = (px >> 24) & 0xFFu;
            if (!a) continue;
            if (a == 255) {
                surf_put(s, c->x + x, c->y + y, px & 0x00FFFFFFu);
            } else {
                /* alpha against the destination: one read, one write */
                uint32_t d = surf_get(s, c->x + x, c->y + y);
                uint32_t out = 0;
                for (int ch = 0; ch < 3; ch++) {
                    uint32_t sc = (px >> (ch * 8)) & 0xFFu;
                    uint32_t dc = (d >> (ch * 8)) & 0xFFu;
                    out |= ((sc * a + dc * (255 - a)) / 255) << (ch * 8);
                }
                surf_put(s, c->x + x, c->y + y, out);
            }
        }
}

/* user32.c's DrawIconEx calls here (it stays a user32 export). */
W32_BOOL w32_gdi_draw_icon(W32_HDC hdc, int32_t x, int32_t y, W32_HICON icon,
                           int32_t cx, int32_t cy) {
    if (!icon) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    gdi_obj_t *o = 0;
    int i = idx_from_h(icon);
    if (i >= 0 && gobjs[i].type == GOBJ_ICON) {
        o = &gobjs[i];                      /* table handle */
    } else {
        /* raw resource pointer: the cache is the only source of truth
         * for its length, because the loader registered it there */
        for (int k = 0; k < GDI_ICON_CACHE; k++)
            if (icon_cache[k].blob == (const uint8_t *)icon) {
                int j = idx_from_h(icon_cache[k].obj);
                if (j >= 0 && gobjs[j].type == GOBJ_ICON) o = &gobjs[j];
                break;
            }
        if (!o) {
            w32_set_last_error(W32_ERROR_INVALID_HANDLE);
            return 0;
        }
    }
    if (cx <= 0) cx = o->u.icon.w;
    if (cy <= 0) cy = o->u.icon.h;
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    icon_ctx_t c;
    c.icon = o; c.x = x; c.y = y; c.cx = cx; c.cy = cy;
    if (d->kind == 1) {
        surf_t s;
        if (surf_of_dc(d, &s) != 0) return 0;
        icon_body(&s, &c);
        return 1;
    }
    return win_raster_blit(d, x, y, cx, cy, icon_body, &c);
}

/* ===================================================================== *
 * The A-5 GDI calls, moved from user32.c: now real DC state             *
 * ===================================================================== */

W32ABI W32_BOOL MoveToEx(W32_HDC hdc, int32_t x, int32_t y, W32_POINT *old) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    if (old) { old->x = d->cur_x; old->y = d->cur_y; }
    d->cur_x = x; d->cur_y = y;
    return W32_TRUE;
}

typedef struct { int32_t x1, y1; } line_ctx_t;

static void line_body(surf_t *s, void *user) {
    line_ctx_t *c = (line_ctx_t *)user;
    surf_line_pen(s, s->dc->cur_x, s->dc->cur_y, c->x1, c->y1);
}

W32ABI W32_BOOL LineTo(W32_HDC hdc, int32_t x, int32_t y) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return W32_FALSE; }
    gdi_pen_t *p = dc_pen(d);
    int w = p->width > 0 ? p->width : 1;
    int32_t x0 = d->cur_x, y0 = d->cur_y;
    int32_t minx = x0 < x ? x0 : x, maxx = x0 > x ? x0 : x;
    int32_t miny = y0 < y ? y0 : y, maxy = y0 > y ? y0 : y;
    line_ctx_t c = { x, y };
    int ok;
    if (d->kind == 1) {
        surf_t s;
        if (surf_of_dc(d, &s) != 0) return 0;
        line_body(&s, &c);
        ok = 1;
    } else {
        ok = win_raster_blit(d, minx - w, miny - w,
                             (maxx - minx) + 2 * w + 1,
                             (maxy - miny) + 2 * w + 1, line_body, &c);
    }
    d->cur_x = x; d->cur_y = y;
    return ok;
}

typedef struct { int32_t l, t, r, b; gdi_brush_t *br; } fill_ctx_t;

static void fill_body(surf_t *s, void *user) {
    fill_ctx_t *c = (fill_ctx_t *)user;
    for (int32_t y = c->t; y < c->b; y++)
        for (int32_t x = c->l; x < c->r; x++) {
            uint32_t col = brush_color_at(s->dc, c->br, x, y);
            if (col == GDI_BRUSH_SKIP) continue;
            surf_put(s, x, y, col);
        }
}

/* user32.c's FillRect calls here (it stays a user32 export). */
W32_BOOL w32_gdi_fill_rect(W32_HDC hdc, const W32_RECT *r, W32_HBRUSH brush) {
    gdi_dc_t *d = dc_from_h(hdc);
    if (!d || !r) { w32_set_last_error(W32_ERROR_INVALID_HANDLE); return 0; }
    if (r->right <= r->left || r->bottom <= r->top) return 1;
    gdi_brush_t *br = dc_brush(d);
    if (brush && (uintptr_t)brush >= GDI_HANDLE_BIAS) {
        gdi_obj_t *o = obj_from_h(brush, GOBJ_BRUSH);
        if (o) br = &o->u.brush;
    } else if (brush) {
        static gdi_brush_t legacy;          /* A-5 raw-colour convention */
        legacy.style = GDI_BS_SOLID;
        legacy.color = (W32_DWORD)(uintptr_t)brush;
        br = &legacy;
    }
    fill_ctx_t c;
    c.l = r->left; c.t = r->top; c.r = r->right; c.b = r->bottom;
    c.br = br;
    if (d->kind == 1) {
        surf_t s;
        if (surf_of_dc(d, &s) != 0) return 0;
        fill_body(&s, &c);
        return 1;
    }
    return win_raster_blit(d, r->left, r->top,
                           r->right - r->left, r->bottom - r->top,
                           fill_body, &c);
}

/* Debug hook used by the host unit test to reach the object table. */
int w32_gdi_obj_type(void *h) {
    int i = idx_from_h(h);
    return i < 0 ? 0 : gobjs[i].type;
}

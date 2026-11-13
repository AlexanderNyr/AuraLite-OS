/* userspace/office/office/aoslide.c — AuraLite Slide (OFFICE_PLAN.md OF6).
 *
 * A fixed-buffer deck editor and presenter.  The document model, strict XML
 * codec, z-order/slide operations, snapping and snapshot history live in
 * libaodoc/aodeck; text objects use the same ao_model run model and OF3 layout
 * engine as Writer.  This file is the GUI consumer and receipt printer.
 *
 * Editor keys: F5 show; Ctrl+S save; N new slide; D duplicate slide;
 * Delete removes the selected object; T/R/O/E/L/P/I add text/rect/rounded/
 * ellipse/line/polyline/image; G toggles guides; # toggles master page numbers;
 * Ctrl+M toggles master theme; Shift+N edits speaker notes; Ctrl+Z/Y undo/redo;
 * arrows move a selected object; Ctrl+Up/Down reorders slides; Alt+Up/Down
 * changes object z-order.  Double-click a text object to edit it.  During the
 * show, click, Space, Right/Down advances and Esc exits.  No transitions.
 */

#define _POSIX_C_SOURCE 200809L

#include "aodeck.h"
#include "aolayout.h"
#include "aoglyph.h"
#include "auragui.h"
#include "wv_image.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_DECK_PATH "/documents/Untitled.aoslide"
#define DECK_XML_CAP       (1024u * 1024u)
#define IO_CAP             (4u * 1024u * 1024u)
#define FONT_FILE_CAP      (128u * 1024u)
#define FONT_ARENA_CAP     (192u * 1024u)
#define APP_W_MAX          2560u
#define APP_H_MAX          1600u
#define TEXT_LAYOUT_LINES  128u
#define TEXT_LAYOUT_PAGES  16u
#define IMG_CACHE_N        3u
#define UI_BAR_H           46
#define THUMB_X            10
#define THUMB_W            138
#define THUMB_TOP          68
#define THUMB_STEP         96
#define VIEW_LEFT          164
#define VIEW_RIGHT         14
#define VIEW_TOP           62
#define VIEW_BOTTOM        62
#define GLYPH_SCRATCH_W    128
#define GLYPH_SCRATCH_H    128
#define GLYPH_BASE_X       8
#define GLYPH_BASELINE     88
#define FP_ONE             65536LL

#define K_LEFT   0x100
#define K_RIGHT  0x101
#define K_UP     0x102
#define K_DOWN   0x103
#define K_HOME   0x104
#define K_END    0x105
#define K_PGUP   0x106
#define K_PGDN   0x107
#define K_DEL    0x109
#define K_F5     0x114
#define M_SHIFT  0x01
#define M_CTRL   0x02
#define M_ALT    0x04

#define C_UI_BG       0x00191F2B
#define C_UI_PANEL    0x00252D3A
#define C_UI_PANEL2   0x00323B4A
#define C_UI_TEXT     0x00E8EDF4
#define C_UI_MUTED    0x009CA8B8
#define C_UI_ACCENT   0x003E80C8
#define C_UI_BORDER   0x00515D6E
#define C_UI_SELECT   0x00FFD45A
#define C_SLIDE_SHADOW 0x000B1018

static ao_deck_t deck;
static ao_deck_t check_deck;
static char xml_buf[DECK_XML_CAP];
static uint8_t io_buf[IO_CAP];
static char font_file[FONT_FILE_CAP];
static uint8_t font_mem_sans[FONT_ARENA_CAP];
static uint8_t font_mem_mono[FONT_ARENA_CAP];
static ao_arena_t font_arena_sans, font_arena_mono;
static ao_font_t font_sans, font_mono;
static uint32_t glyph_scratch[GLYPH_SCRATCH_W * GLYPH_SCRATCH_H];
static ao_layout_line_t text_lines[TEXT_LAYOUT_LINES];
static ao_layout_page_t text_pages[TEXT_LAYOUT_PAGES];

static uint32_t *paint_px;
static uint32_t paint_w, paint_h, paint_stride;
static int clip_on, clip_l, clip_t, clip_r, clip_b;
static uint32_t *editor_frame;
static uint32_t *show_frame;
static uint32_t editor_w, editor_h;
static uint32_t show_w, show_h;
static uint32_t screen_w, screen_h;
static int editor_wid = -1;
static int show_wid = -1;
static int running = 1;
static int show_slide;
static uint32_t active_slide;
static int selected_obj = -1;
static int thumb_scroll;
static int guides_visible = 1;
static int editing_text;
static int editing_notes;
static int editing_image_path;
static uint32_t text_caret;
static char current_path[AO_DECK_PATH_MAX] = DEFAULT_DECK_PATH;
static char image_path_edit[AO_DECK_PATH_MAX];
static int save_dirty;
static char status_msg[128] = "Ready";
static uint32_t status_ticks;

static struct {
    char path[AO_DECK_PATH_MAX];
    uint32_t *pixels;
    int w, h;
    uint32_t last_use;
} img_cache[IMG_CACHE_N];
static uint32_t img_clock;

static const int32_t VGUIDES[] = { 0, 160, 320, 480, 640, 800, 960 };
static const int32_t HGUIDES[] = { 0, 90, 180, 270, 360, 450, 540 };

static void set_status(const char *s) {
    if (!s) s = "";
    snprintf(status_msg, sizeof status_msg, "%s", s);
    status_ticks = 0;
}

static void set_target(uint32_t *px, uint32_t w, uint32_t h) {
    paint_px = px;
    paint_w = w;
    paint_h = h;
    paint_stride = w;
    clip_on = 0;
}

static int inside_clip(int x, int y) {
    return !clip_on || (x >= clip_l && x < clip_r && y >= clip_t && y < clip_b);
}

static void pixel(int x, int y, uint32_t color) {
    if (!paint_px || x < 0 || y < 0 || (uint32_t)x >= paint_w ||
        (uint32_t)y >= paint_h || !inside_clip(x, y)) return;
    paint_px[(size_t)y * paint_stride + (uint32_t)x] = color & 0x00FFFFFFu;
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!paint_px || w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)paint_w) x1 = (int)paint_w;
    if (y1 > (int)paint_h) y1 = (int)paint_h;
    if (clip_on) {
        if (x0 < clip_l) x0 = clip_l;
        if (y0 < clip_t) y0 = clip_t;
        if (x1 > clip_r) x1 = clip_r;
        if (y1 > clip_b) y1 = clip_b;
    }
    if (x1 <= x0 || y1 <= y0) return;
    color &= 0x00FFFFFFu;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = paint_px + (size_t)yy * paint_stride + (uint32_t)x0;
        for (int xx = x0; xx < x1; xx++) *row++ = color;
    }
}

static void outline_rect(int x, int y, int w, int h, uint32_t color, int thick) {
    if (w <= 0 || h <= 0) return;
    if (thick < 1) thick = 1;
    if (thick * 2 > w) thick = (w + 1) / 2;
    if (thick * 2 > h) thick = (h + 1) / 2;
    fill_rect(x, y, w, thick, color);
    fill_rect(x, y + h - thick, w, thick, color);
    fill_rect(x, y + thick, thick, h - 2 * thick, color);
    fill_rect(x + w - thick, y + thick, thick, h - 2 * thick, color);
}

static void line_draw(int x0, int y0, int x1, int y1, uint32_t color, int thick) {
    int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 >= y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    if (thick < 1) thick = 1;
    for (;;) {
        int r = thick / 2;
        fill_rect(x0 - r, y0 - r, thick, thick, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static int rounded_inside(int x, int y, int w, int h, int r) {
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x >= w || y >= h) return 0;
    int cx = x, cy = y;
    if (x < r) cx = r;
    else if (x >= w - r) cx = w - r - 1;
    if (y < r) cy = r;
    else if (y >= h - r) cy = h - r - 1;
    int dx = x - cx, dy = y - cy;
    return (int64_t)dx * dx + (int64_t)dy * dy <= (int64_t)r * r;
}

static void rounded_rect(int x, int y, int w, int h, uint32_t fill,
                         int fill_on, uint32_t stroke, int stroke_on, int sw) {
    if (w <= 0 || h <= 0) return;
    int r = w < h ? w / 7 : h / 7;
    if (r < 3) r = 3;
    if (r > 24) r = 24;
    if (sw < 1) sw = 1;
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            if (!rounded_inside(xx, yy, w, h, r)) continue;
            int inner_w = w - 2 * sw, inner_h = h - 2 * sw;
            int edge = inner_w <= 0 || inner_h <= 0 ||
                       !rounded_inside(xx - sw, yy - sw, inner_w, inner_h,
                                       r > sw ? r - sw : 1);
            if (stroke_on && edge) pixel(x + xx, y + yy, stroke);
            else if (fill_on) pixel(x + xx, y + yy, fill);
        }
    }
}

static void ellipse_draw(int x, int y, int w, int h, uint32_t fill,
                         int fill_on, uint32_t stroke, int stroke_on, int sw) {
    if (w <= 0 || h <= 0) return;
    int64_t ww = (int64_t)w * w;
    int64_t hh = (int64_t)h * h;
    int64_t lim = ww * hh;
    int inner_w = w - 2 * sw, inner_h = h - 2 * sw;
    int64_t iww = (int64_t)inner_w * inner_w;
    int64_t ihh = (int64_t)inner_h * inner_h;
    int64_t ilim = (inner_w > 0 && inner_h > 0) ? iww * ihh : 0;
    if (sw < 1) sw = 1;
    for (int yy = 0; yy < h; yy++) {
        int64_t dy = 2LL * yy + 1 - h;
        for (int xx = 0; xx < w; xx++) {
            int64_t dx = 2LL * xx + 1 - w;
            int64_t v = dx * dx * hh + dy * dy * ww;
            if (v > lim) continue;
            int edge = 0;
            if (stroke_on) {
                if (inner_w <= 0 || inner_h <= 0) edge = 1;
                else {
                    int64_t iv = dx * dx * ihh + dy * dy * iww;
                    edge = iv >= ilim;
                }
            }
            if (edge) pixel(x + xx, y + yy, stroke);
            else if (fill_on) pixel(x + xx, y + yy, fill);
            else if (stroke_on) pixel(x + xx, y + yy, stroke);
        }
    }
}

static int32_t fp_floor(int64_t v) {
    if (v >= 0) return (int32_t)(v / FP_ONE);
    return (int32_t)(-(((-v) + FP_ONE - 1) / FP_ONE));
}

static int32_t map_coord(int32_t origin, int32_t logical, int64_t scale_fp) {
    return origin + fp_floor((int64_t)logical * scale_fp);
}

static int32_t map_size(int32_t logical, int64_t scale_fp) {
    if (logical <= 0) return 0;
    int32_t n = fp_floor((int64_t)logical * scale_fp + FP_ONE / 2);
    return n < 1 ? 1 : n;
}

static void clip_push(int l, int t, int r, int b,
                      int *old_on, int *old_l, int *old_t, int *old_r, int *old_b) {
    *old_on = clip_on; *old_l = clip_l; *old_t = clip_t;
    *old_r = clip_r; *old_b = clip_b;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > (int)paint_w) r = (int)paint_w;
    if (b > (int)paint_h) b = (int)paint_h;
    if (clip_on) {
        if (l < clip_l) l = clip_l;
        if (t < clip_t) t = clip_t;
        if (r > clip_r) r = clip_r;
        if (b > clip_b) b = clip_b;
    }
    clip_on = 1; clip_l = l; clip_t = t; clip_r = r; clip_b = b;
}

static void clip_pop(int old_on, int old_l, int old_t, int old_r, int old_b) {
    clip_on = old_on; clip_l = old_l; clip_t = old_t; clip_r = old_r; clip_b = old_b;
}

static void ui_text(int x, int baseline, const char *s, uint32_t color) {
    if (!s || !font_sans.n || !paint_px) return;
    ao_text_draw(&font_sans, s, strlen(s), AO_STYLE_NONE, paint_px,
                 (int)paint_stride, (int)paint_w, (int)paint_h,
                 x, baseline, color, NULL);
}


static int load_face(const char *path, ao_font_t *font, ao_arena_t *arena,
                     uint8_t *arena_mem) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(font_file, 1, sizeof font_file, f);
    int over = n == sizeof font_file && !feof(f);
    fclose(f);
    if (over || n == 0) return -1;
    ao_arena_init(arena, arena_mem, FONT_ARENA_CAP);
    return ao_font_load_bdf(font, arena, font_file, n);
}

static void fonts_init(void) {
    memset(&font_sans, 0, sizeof font_sans);
    memset(&font_mono, 0, sizeof font_mono);
    (void)load_face("/lib/fonts/ao_sans_11.bdf", &font_sans,
                    &font_arena_sans, font_mem_sans);
    (void)load_face("/lib/fonts/ao_mono_16.bdf", &font_mono,
                    &font_arena_mono, font_mem_mono);
}

static const ao_font_t *font_for(uint8_t id) {
    if (id == AO_FONT_MONO && font_mono.n) return &font_mono;
    if (font_sans.n) return &font_sans;
    if (font_mono.n) return &font_mono;
    return NULL;
}

static int measure_text_px(const ao_font_t *font, const char *s, size_t n,
                           uint8_t size) {
    if (!font || !font->n) return (int)n * (size ? size : 11) / 2;
    int base = 11;
    int w = ao_text_width(font, s, n);
    return (w * (size ? size : 11) + base / 2) / base;
}

typedef struct { const ao_font_t *font; } measure_ctx_t;

static int layout_measure(void *ctx, const char *s, size_t n, uint8_t size) {
    measure_ctx_t *m = (measure_ctx_t *)ctx;
    return measure_text_px(m ? m->font : NULL, s, n, size);
}

static void paint_scaled_text(int x, int baseline, const char *s, size_t n,
                              int style, uint8_t size, uint8_t font_id,
                              uint32_t color, int64_t view_scale_fp) {
    const ao_font_t *font = font_for(font_id);
    if (!font || !s || n == 0) return;
    int64_t glyph_scale_fp = view_scale_fp * (size ? size : 11) / 11;
    if (glyph_scale_fp <= 0) glyph_scale_fp = 1;
    int64_t pen_fp = 0;
    size_t i = 0;
    while (i < n) {
        uint32_t cp = 0;
        int bytes = ao_utf8_decode(s + i, n - i, &cp);
        if (bytes <= 0) { cp = '?'; bytes = 1; }
        memset(glyph_scratch, 0, sizeof glyph_scratch);
        (void)ao_glyph_draw(font, cp, style, glyph_scratch,
                            GLYPH_SCRATCH_W, GLYPH_SCRATCH_W,
                            GLYPH_SCRATCH_H, GLYPH_BASE_X,
                            GLYPH_BASELINE, 0x00FFFFFFu);
        for (int sy = 0; sy < GLYPH_SCRATCH_H; sy++) {
            for (int sx = 0; sx < GLYPH_SCRATCH_W; sx++) {
                if (!glyph_scratch[(size_t)sy * GLYPH_SCRATCH_W + (uint32_t)sx])
                    continue;
                int relx = sx - GLYPH_BASE_X;
                int rely = sy - GLYPH_BASELINE;
                int32_t dx0 = x + fp_floor(pen_fp + (int64_t)relx * glyph_scale_fp);
                int32_t dx1 = x + fp_floor(pen_fp + (int64_t)(relx + 1) * glyph_scale_fp);
                int32_t dy0 = baseline + fp_floor((int64_t)rely * glyph_scale_fp);
                int32_t dy1 = baseline + fp_floor((int64_t)(rely + 1) * glyph_scale_fp);
                if (dx1 <= dx0) dx1 = dx0 + 1;
                if (dy1 <= dy0) dy1 = dy0 + 1;
                fill_rect(dx0, dy0, dx1 - dx0, dy1 - dy0, color);
            }
        }
        int adv = ao_font_advance(font, cp);
        pen_fp += (int64_t)adv * glyph_scale_fp;
        i += (size_t)bytes;
    }
}

static int read_image(const char *path, uint32_t **px, int *w, int *h) {
    if (!path || !path[0]) return -1;
    for (uint32_t i = 0; i < IMG_CACHE_N; i++) {
        if (img_cache[i].pixels && strcmp(img_cache[i].path, path) == 0) {
            img_cache[i].last_use = ++img_clock;
            *px = img_cache[i].pixels; *w = img_cache[i].w; *h = img_cache[i].h;
            return 0;
        }
    }
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(io_buf, 1, sizeof io_buf, f);
    int over = n == sizeof io_buf && !feof(f);
    fclose(f);
    if (over || n < 16) return -1;
    uint32_t *decoded = NULL;
    int iw = 0, ih = 0;
    if (wv_image_decode(io_buf, n, &decoded, &iw, &ih) != 0 ||
        !decoded || iw <= 0 || ih <= 0) {
        free(decoded);
        return -1;
    }
    uint32_t slot = 0;
    for (uint32_t i = 0; i < IMG_CACHE_N; i++) {
        if (!img_cache[i].pixels) { slot = i; break; }
        if (img_cache[i].last_use < img_cache[slot].last_use) slot = i;
    }
    free(img_cache[slot].pixels);
    memset(&img_cache[slot], 0, sizeof img_cache[slot]);
    img_cache[slot].pixels = decoded;
    img_cache[slot].w = iw; img_cache[slot].h = ih;
    snprintf(img_cache[slot].path, sizeof img_cache[slot].path, "%s", path);
    img_cache[slot].last_use = ++img_clock;
    *px = decoded; *w = iw; *h = ih;
    return 0;
}

static void render_image_box(const ao_deck_obj_t *o, int x, int y, int w, int h) {
    uint32_t *src = NULL;
    int iw = 0, ih = 0;
    if (read_image(o->img_path, &src, &iw, &ih) != 0) {
        fill_rect(x, y, w, h, 0x00DDE3EC);
        for (int yy = 0; yy < h; yy += 12)
            for (int xx = 0; xx < w; xx += 12)
                if (((xx / 12) + (yy / 12)) & 1)
                    fill_rect(x + xx, y + yy, 12, 12, 0x00CBD3DF);
        outline_rect(x, y, w, h, 0x006B7788, 1);
        if (font_sans.n) {
            const char *msg = o->img_path[0] ? "Image unavailable" : "Set image path";
            int tw = ao_text_width(&font_sans, msg, strlen(msg));
            ui_text(x + (w - tw) / 2, y + h / 2, msg, 0x00404A59);
        }
        return;
    }
    if (w <= 0 || h <= 0) return;
    int old_on, old_l, old_t, old_r, old_b;
    clip_push(x, y, x + w, y + h, &old_on, &old_l, &old_t, &old_r, &old_b);
    for (int yy = 0; yy < h; yy++) {
        int sy = (int)((int64_t)yy * ih / h);
        for (int xx = 0; xx < w; xx++) {
            int sx = (int)((int64_t)xx * iw / w);
            pixel(x + xx, y + yy, src[(size_t)sy * (uint32_t)iw + (uint32_t)sx]);
        }
    }
    clip_pop(old_on, old_l, old_t, old_r, old_b);
    if (o->stroke_on) outline_rect(x, y, w, h, o->stroke, o->stroke_w ? o->stroke_w : 1);
}

typedef struct {
    int x, y, w, h, slide_x, slide_y;
    int64_t scale_fp;
} view_t;

static view_t make_view(int x, int y, int w, int h) {
    view_t v;
    memset(&v, 0, sizeof v);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int64_t sx = ((int64_t)w * FP_ONE) / AO_SLIDE_W;
    int64_t sy = ((int64_t)h * FP_ONE) / AO_SLIDE_H;
    v.scale_fp = sx < sy ? sx : sy;
    if (v.scale_fp < 1) v.scale_fp = 1;
    v.w = map_size(AO_SLIDE_W, v.scale_fp);
    v.h = map_size(AO_SLIDE_H, v.scale_fp);
    v.x = x + (w - v.w) / 2;
    v.y = y + (h - v.h) / 2;
    v.slide_x = v.x; v.slide_y = v.y;
    return v;
}

static void object_bounds(const ao_deck_obj_t *o, int32_t *x, int32_t *y,
                          int32_t *w, int32_t *h) {
    int32_t x0 = o->x, y0 = o->y, x1 = o->x + o->w, y1 = o->y + o->h;
    if (o->kind == AO_OBJ_LINE) {
        x0 = o->x < o->w ? o->x : o->w;
        y0 = o->y < o->h ? o->y : o->h;
        x1 = o->x > o->w ? o->x : o->w;
        y1 = o->y > o->h ? o->y : o->h;
    } else if (o->kind == AO_OBJ_POLYLINE && o->npts) {
        x0 = x1 = o->pts_x[0]; y0 = y1 = o->pts_y[0];
        for (uint8_t i = 1; i < o->npts; i++) {
            if (o->pts_x[i] < x0) x0 = o->pts_x[i];
            if (o->pts_x[i] > x1) x1 = o->pts_x[i];
            if (o->pts_y[i] < y0) y0 = o->pts_y[i];
            if (o->pts_y[i] > y1) y1 = o->pts_y[i];
        }
    }
    if (x1 < x0) { int32_t t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int32_t t = y0; y0 = y1; y1 = t; }
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}

static void render_text_object(const ao_deck_obj_t *o, const view_t *v) {
    if (!o->md.para_len) return;
    ao_page_geom_t g;
    g.page_w = o->w > 0 ? (uint32_t)o->w : 1u;
    g.page_h = o->h > 0 ? (uint32_t)o->h : 1u;
    g.margin_l = g.margin_r = g.margin_t = g.margin_b = 0;
    measure_ctx_t mctx = { font_for(AO_FONT_SANS) };
    ao_layout_t l;
    ao_layout_init(&l, text_lines, TEXT_LAYOUT_LINES, text_pages, TEXT_LAYOUT_PAGES);
    if (ao_layout_build(&l, &o->md, &g, layout_measure, &mctx, NULL, NULL) != AO_OK ||
        l.line_len == 0 || l.page_len == 0) {
        return;
    }
    uint32_t first = l.pages[0].first_line;
    uint32_t last = first + l.pages[0].line_count;
    if (last > l.line_len) last = l.line_len;
    int32_t bx, by, bw, bh;
    object_bounds(o, &bx, &by, &bw, &bh);
    (void)bx; (void)by; (void)bw; (void)bh;
    int box_x = map_coord(v->slide_x, o->x, v->scale_fp);
    int box_y = map_coord(v->slide_y, o->y, v->scale_fp);
    int box_w = map_size(o->w, v->scale_fp);
    int box_h = map_size(o->h, v->scale_fp);
    int old_on, old_l, old_t, old_r, old_b;
    clip_push(box_x, box_y, box_x + box_w, box_y + box_h,
              &old_on, &old_l, &old_t, &old_r, &old_b);

    int content_h = 0;
    if (last > first) {
        ao_layout_line_t *ln = &l.lines[last - 1];
        content_h = ln->y + ln->h;
    }
    int valign_off = 0;
    if (o->t_valign == 1 && (int)o->h > content_h) valign_off = ((int)o->h - content_h) / 2;
    else if (o->t_valign == 2 && (int)o->h > content_h) valign_off = (int)o->h - content_h;

    for (uint32_t li = first; li < last; li++) {
        const ao_layout_line_t *ln = &l.lines[li];
        if (ln->para >= o->md.para_len) continue;
        const ao_para_t *p = &o->md.paras[ln->para];
        uint32_t abs = p->off + ln->start;
        uint32_t len = ln->len;
        if (abs > o->md.text_len) continue;
        if (len > o->md.text_len - abs) len = o->md.text_len - abs;
        int para_x = 0;
        int text_w = measure_text_px(mctx.font, o->md.text + abs, len, ln->size_px);
        if (p->align == AO_ALIGN_CENTER && (int)o->w > text_w)
            para_x = ((int)o->w - text_w) / 2;
        else if (p->align == AO_ALIGN_RIGHT && (int)o->w > text_w)
            para_x = (int)o->w - text_w;
        if (ln->marker[0]) para_x += 24;
        int baseline_logical = ln->y + valign_off + ln->h - 3;
        int base_x = map_coord(v->slide_x, o->x + para_x, v->scale_fp);
        int base_y = map_coord(v->slide_y, o->y + baseline_logical, v->scale_fp);
        int64_t pen_fp = 0;
        uint32_t consumed = 0;
        while (consumed < len) {
            uint32_t pos = abs + consumed;
            uint32_t ri = ao_model_run_of(&o->md, pos);
            if (ri >= o->md.run_len) break;
            const ao_run_t *run = &o->md.runs[ri];
            uint32_t run_end = run->off + run->len;
            if (run_end <= pos) { consumed++; continue; }
            uint32_t n = run_end - pos;
            if (n > len - consumed) n = len - consumed;
            const ao_font_t *face = font_for(run->font);
            uint8_t size = run->size ? run->size : (o->t_size ? o->t_size : 11);
            int style = run->style & (AO_STYLE_BOLD | AO_STYLE_ITALIC | AO_STYLE_UNDERLINE);
            if (o->t_bold) style |= AO_STYLE_BOLD;
            if (!face) { consumed += n; continue; }
            int run_x = base_x + fp_floor(pen_fp);
            paint_scaled_text(run_x, base_y, o->md.text + pos, n, style,
                              size, run->font, o->t_color, v->scale_fp);
            int adv = measure_text_px(face, o->md.text + pos, n, size);
            pen_fp += (int64_t)adv * v->scale_fp;
            consumed += n;
        }
        if (ln->marker[0]) {
            if (ln->kind == AO_PARA_BULLET) {
                int r = map_size(4, v->scale_fp);
                int bx = map_coord(v->slide_x, o->x + 8, v->scale_fp) - r / 2;
                int by = base_y - map_size(6, v->scale_fp);
                ellipse_draw(bx, by, r, r, o->t_color, 1, 0, 0, 1);
            } else {
                int mark_size = ln->size_px ? ln->size_px : 11;
                paint_scaled_text(map_coord(v->slide_x, o->x, v->scale_fp), base_y,
                                  ln->marker, strlen(ln->marker), AO_STYLE_NONE,
                                  (uint8_t)mark_size, AO_FONT_SANS,
                                  o->t_color, v->scale_fp);
            }
        }
    }
    if (o->md.text_len == 0 && font_sans.n) {
        static const char empty_label[] = "Type here";
        int bx0 = box_x + 4;
        int by0 = box_y + 15;
        int64_t s = v->scale_fp;
        paint_scaled_text(bx0, by0, empty_label, sizeof empty_label - 1,
                          AO_STYLE_ITALIC, 11, AO_FONT_SANS,
                          0x00798798, s);
    }
    clip_pop(old_on, old_l, old_t, old_r, old_b);
}

static void render_object(const ao_deck_obj_t *o, const view_t *v) {
    if (!o || o->kind == AO_OBJ_NONE) return;
    if (o->kind == AO_OBJ_LINE) {
        int x0 = map_coord(v->slide_x, o->x, v->scale_fp);
        int y0 = map_coord(v->slide_y, o->y, v->scale_fp);
        int x1 = map_coord(v->slide_x, o->w, v->scale_fp);
        int y1 = map_coord(v->slide_y, o->h, v->scale_fp);
        int sw = map_size(o->stroke_w ? o->stroke_w : 1, v->scale_fp);
        line_draw(x0, y0, x1, y1, o->stroke_on ? o->stroke : 0x00101010, sw);
        return;
    }
    if (o->kind == AO_OBJ_POLYLINE) {
        int sw = map_size(o->stroke_w ? o->stroke_w : 1, v->scale_fp);
        for (uint8_t i = 0; i + 1 < o->npts; i++) {
            line_draw(map_coord(v->slide_x, o->pts_x[i], v->scale_fp),
                      map_coord(v->slide_y, o->pts_y[i], v->scale_fp),
                      map_coord(v->slide_x, o->pts_x[i + 1], v->scale_fp),
                      map_coord(v->slide_y, o->pts_y[i + 1], v->scale_fp),
                      o->stroke_on ? o->stroke : 0x00101010, sw);
        }
        return;
    }
    int x = map_coord(v->slide_x, o->x, v->scale_fp);
    int y = map_coord(v->slide_y, o->y, v->scale_fp);
    int w = map_size(o->w, v->scale_fp);
    int h = map_size(o->h, v->scale_fp);
    int sw = map_size(o->stroke_w ? o->stroke_w : 1, v->scale_fp);
    switch (o->kind) {
    case AO_OBJ_TEXT:
        render_text_object(o, v);
        break;
    case AO_OBJ_RECT:
        if (o->fill_on) fill_rect(x, y, w, h, o->fill);
        if (o->stroke_on) outline_rect(x, y, w, h, o->stroke, sw);
        break;
    case AO_OBJ_ROUNDED:
        rounded_rect(x, y, w, h, o->fill, o->fill_on,
                     o->stroke, o->stroke_on, sw);
        break;
    case AO_OBJ_ELLIPSE:
        ellipse_draw(x, y, w, h, o->fill, o->fill_on,
                     o->stroke, o->stroke_on, sw);
        break;
    case AO_OBJ_IMAGE:
        render_image_box(o, x, y, w, h);
        break;
    default:
        break;
    }
}

static void draw_guides(const view_t *v) {
    uint32_t col = 0x0076A9CF;
    for (uint32_t i = 0; i < sizeof VGUIDES / sizeof VGUIDES[0]; i++) {
        int x = map_coord(v->slide_x, VGUIDES[i], v->scale_fp);
        for (int y = v->y; y < v->y + v->h; y += 8) pixel(x, y, col);
    }
    for (uint32_t i = 0; i < sizeof HGUIDES / sizeof HGUIDES[0]; i++) {
        int y = map_coord(v->slide_y, HGUIDES[i], v->scale_fp);
        for (int x = v->x; x < v->x + v->w; x += 8) pixel(x, y, col);
    }
}

static void draw_selected(const ao_deck_obj_t *o, const view_t *v) {
    if (!o) return;
    int32_t lx, ly, lw, lh;
    object_bounds(o, &lx, &ly, &lw, &lh);
    int x = map_coord(v->slide_x, lx, v->scale_fp);
    int y = map_coord(v->slide_y, ly, v->scale_fp);
    int w = map_size(lw, v->scale_fp);
    int h = map_size(lh, v->scale_fp);
    uint32_t col = C_UI_SELECT;
    for (int xx = x; xx < x + w; xx += 7) {
        fill_rect(xx, y, 4, 2, col);
        fill_rect(xx, y + h - 2, 4, 2, col);
    }
    for (int yy = y; yy < y + h; yy += 7) {
        fill_rect(x, yy, 2, 4, col);
        fill_rect(x + w - 2, yy, 2, 4, col);
    }
    fill_rect(x + w - 4, y + h - 4, 8, 8, C_UI_SELECT);
}

static void render_slide(const ao_deck_t *d, uint32_t si,
                         int x, int y, int w, int h,
                         int use_guides, int selection) {
    if (!d || si >= d->slide_len) return;
    view_t v = make_view(x, y, w, h);
    fill_rect(v.x - 4, v.y - 4, v.w + 8, v.h + 8, C_SLIDE_SHADOW);
    fill_rect(v.x, v.y, v.w, v.h, d->master_bg);
    for (uint32_t i = 0; i < d->slides[si].obj_len; i++)
        render_object(&d->slides[si].objs[i], &v);
    if (d->master_page_numbers) {
        char num[32];
        snprintf(num, sizeof num, "%u / %u", (unsigned)(si + 1),
                 (unsigned)d->slide_len);
        uint32_t fg = (d->master_bg & 0x00FFFFFFu) < 0x00808080u
                          ? 0x00E7EDF5 : 0x006A7584;
        int tw = font_sans.n ? ao_text_width(&font_sans, num, strlen(num)) : 30;
        ui_text(v.x + v.w - tw - 12, v.y + v.h - 10, num, fg);
    }
    if (use_guides && d->guides_on) draw_guides(&v);
    if (selection >= 0 && (uint32_t)selection < d->slides[si].obj_len)
        draw_selected(&d->slides[si].objs[selection], &v);
    outline_rect(v.x, v.y, v.w, v.h, 0x00838E9C, 1);
}

static void rebind_obj(ao_deck_obj_t *o) {
    o->md.text = o->md_text;
    o->md.runs = o->md_runs;
    o->md.paras = o->md_paras;
}

static void add_background(uint32_t slide, uint32_t color) {
    uint32_t idx = 0;
    if (ao_deck_obj_add(&deck, slide, AO_OBJ_RECT, &idx) != AO_DECK_OK) return;
    ao_deck_obj_t *o = &deck.slides[slide].objs[idx];
    o->x = 0; o->y = 0; o->w = AO_SLIDE_W; o->h = AO_SLIDE_H;
    o->fill = color; o->fill_on = 1; o->stroke_on = 0;
    if (idx != 0) {
        ao_deck_obj_t tmp;
        memcpy(&tmp, o, sizeof tmp);
        memmove(&deck.slides[slide].objs[1], &deck.slides[slide].objs[0],
                idx * sizeof deck.slides[slide].objs[0]);
        memcpy(&deck.slides[slide].objs[0], &tmp, sizeof tmp);
        rebind_obj(&deck.slides[slide].objs[0]);
        for (uint32_t i = 1; i <= idx; i++) rebind_obj(&deck.slides[slide].objs[i]);
    }
}

static ao_deck_obj_t *add_text_obj(uint32_t slide, int x, int y, int w, int h,
                                   const char *text, uint8_t para_style,
                                   uint8_t size, uint32_t color, int bold) {
    uint32_t idx = 0;
    if (ao_deck_obj_add(&deck, slide, AO_OBJ_TEXT, &idx) != AO_DECK_OK) return NULL;
    ao_deck_obj_t *o = &deck.slides[slide].objs[idx];
    o->x = x; o->y = y; o->w = w; o->h = h;
    o->t_color = color; o->t_size = size; o->t_bold = (uint8_t)(bold != 0);
    if (text && text[0]) {
        uint32_t n = (uint32_t)strlen(text);
        if (ao_model_insert(&o->md, 0, text, n) != AO_OK) return o;
        for (uint32_t p = 0; p < o->md.para_len; p++) {
            uint32_t pos = o->md.paras[p].off;
            (void)ao_model_set_para_style(&o->md, pos, para_style);
        }
        (void)ao_model_set_size_font(&o->md, 0, o->md.text_len,
                                     size, AO_FONT_SANS);
        if (bold) (void)ao_model_set_style(&o->md, 0, o->md.text_len,
                                           AO_STYLE_BOLD, 1);
    }
    return o;
}

static void demo_shape(uint32_t s, uint32_t kind, int x, int y, int w, int h,
                       uint32_t fill, uint32_t stroke, int fill_on, int stroke_on) {
    uint32_t idx = 0;
    if (ao_deck_obj_add(&deck, s, kind, &idx) != AO_DECK_OK) return;
    ao_deck_obj_t *o = &deck.slides[s].objs[idx];
    o->x = x; o->y = y; o->w = w; o->h = h;
    o->fill = fill; o->stroke = stroke;
    o->fill_on = (uint8_t)fill_on; o->stroke_on = (uint8_t)stroke_on;
    o->stroke_w = 3;
}

static void build_demo(void) {
    ao_deck_init(&deck);
    deck.master_bg = 0x00FFFFFF;
    deck.master_color = 0x00101826;
    deck.master_size = 20;
    deck.master_bold = 0;
    deck.master_page_numbers = 1;
    deck.guides_on = 1;

    add_background(0, 0x0017273A);
    demo_shape(0, AO_OBJ_RECT, 68, 70, 14, 400, 0x00F0B843, 0, 1, 0);
    demo_shape(0, AO_OBJ_ROUNDED, 112, 340, 365, 86, 0x00274258, 0x005B768B, 1, 1);
    (void)add_text_obj(0, 112, 142, 740, 88, "AuraLite Office",
                       AO_PARA_H1, 32, 0x00FFFFFF, 1);
    (void)add_text_obj(0, 116, 240, 730, 60, "Clear ideas. Beautiful slides.",
                       AO_PARA_NORMAL, 19, 0x00B7C9D8, 0);
    (void)add_text_obj(0, 142, 373, 310, 44, "A presentation by AuraLite",
                       AO_PARA_NORMAL, 14, 0x00F5F7FA, 0);
    snprintf(deck.slides[0].notes, sizeof deck.slides[0].notes,
             "Welcome. Introduce AuraLite Office and the design goals.");

    uint32_t s1 = 0;
    if (ao_deck_slide_add(&deck, 0, &s1) == AO_DECK_OK) {
        add_background(s1, 0x00F3F6FA);
        demo_shape(s1, AO_OBJ_RECT, 88, 165, 10, 252, 0x003D9B75, 0, 1, 0);
        demo_shape(s1, AO_OBJ_ELLIPSE, 760, 78, 110, 110, 0x00DDEEE8, 0, 1, 0);
        (void)add_text_obj(s1, 120, 78, 700, 64, "Agenda",
                           AO_PARA_H1, 30, 0x001C2C3E, 1);
        ao_deck_obj_t *list = add_text_obj(s1, 126, 172, 650, 252,
            "One model for every slide\nGuides, shapes and images\nA clear, focused show",
            AO_PARA_BULLET, 14, 0x00334353, 0);
        if (list) {
            for (uint32_t p = 0; p < list->md.para_len; p++)
                (void)ao_model_set_para_style(&list->md, list->md.paras[p].off,
                                              AO_PARA_BULLET);
        }
        snprintf(deck.slides[s1].notes, sizeof deck.slides[s1].notes,
                 "Walk through the model, the editor tools, and the full-screen presenter.");
    }

    uint32_t s2 = 0;
    if (ao_deck_slide_add(&deck, 1, &s2) == AO_DECK_OK) {
        add_background(s2, 0x001B2541);
        demo_shape(s2, AO_OBJ_ELLIPSE, 626, 98, 224, 224, 0x00F0B843, 0, 1, 0);
        demo_shape(s2, AO_OBJ_ELLIPSE, 680, 152, 116, 116, 0x002B3E62, 0, 1, 0);
        demo_shape(s2, AO_OBJ_LINE, 110, 384, 820, 384, 0x00000000, 0x004E7395, 0, 1);
        (void)add_text_obj(s2, 112, 146, 560, 100, "Questions?",
                           AO_PARA_H1, 34, 0x00FFFFFF, 1);
        (void)add_text_obj(s2, 116, 264, 520, 66, "Thank you for listening.",
                           AO_PARA_NORMAL, 18, 0x00C2CEDB, 0);
        (void)add_text_obj(s2, 116, 408, 520, 40, "AuraLite Office  |  Slide",
                           AO_PARA_NORMAL, 13, 0x008DA4B9, 0);
        snprintf(deck.slides[s2].notes, sizeof deck.slides[s2].notes,
                 "Pause for questions, then close with thanks.");
    }
}

static int deck_check(const ao_deck_t *d, const char **why) {
    return ao_deck_valid(d, why) == AO_DECK_OK;
}

static int save_deck(const char *path) {
    const char *why = NULL;
    if (!deck_check(&deck, &why)) {
        printf("[slide] save refused: invalid deck (%s)\n", why ? why : "unknown");
        return -1;
    }
    size_t n = 0;
    int rc = ao_deck_serialize(&deck, xml_buf, sizeof xml_buf, &n);
    if (rc != AO_DECK_OK) {
        printf("[slide] save refused: serialize rc=%d\n", rc);
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("[slide] save failed: cannot open %s\n", path);
        return -1;
    }
    size_t wrote = fwrite(xml_buf, 1, n, f);
    int close_rc = fclose(f);
    int bad = wrote != n || close_rc != 0;
    if (bad) {
        printf("[slide] save failed: short write %s\n", path);
        return -1;
    }
    snprintf(current_path, sizeof current_path, "%s", path);
    save_dirty = 0;
    printf("[slide] saved %s (%lu bytes)\n", path, (unsigned long)n);
    set_status("Saved");
    return 0;
}

static int load_deck(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(io_buf, 1, sizeof io_buf, f);
    int over = n == sizeof io_buf && !feof(f);
    fclose(f);
    if (over || n == 0) return -1;
    if (ao_deck_parse(&deck, (const char *)io_buf, n) != AO_DECK_OK) return -1;
    const char *why = NULL;
    if (!deck_check(&deck, &why)) return -1;
    snprintf(current_path, sizeof current_path, "%s", path);
    return 0;
}

static int demo_receipt(const char *path) {
    build_demo();
    const char *why = NULL;
    if (!deck_check(&deck, &why)) {
        printf("SLIDE DEMO: FAIL (invalid deck: %s)\n", why ? why : "unknown");
        return 1;
    }
    size_t n = 0;
    if (ao_deck_serialize(&deck, xml_buf, sizeof xml_buf, &n) != AO_DECK_OK) {
        printf("SLIDE DEMO: FAIL (serialize)\n");
        return 1;
    }
    ao_deck_init(&check_deck);
    if (ao_deck_parse(&check_deck, xml_buf, n) != AO_DECK_OK ||
        !deck_check(&check_deck, &why) || check_deck.slide_len != 3) {
        printf("SLIDE DEMO: FAIL (self-check)\n");
        return 1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("SLIDE DEMO: FAIL (cannot write %s)\n", path);
        return 1;
    }
    size_t wrote = fwrite(xml_buf, 1, n, f);
    int close_rc = fclose(f);
    int bad = wrote != n || close_rc != 0;
    if (bad) {
        printf("SLIDE DEMO: FAIL (short write)\n");
        return 1;
    }
    printf("SLIDE DEMO: wrote %s\n", path);
    printf("SLIDE DEMO: self-check OK\n");
    return 0;
}

static int show_receipt(const char *path) {
    if (load_deck(path) != 0) {
        printf("SLIDE-SHOW: FAIL (cannot load %s)\n", path);
        return 1;
    }
    for (uint32_t i = 0; i < deck.slide_len; i++)
        printf("SLIDE-SHOW: slide %u/%u\n", (unsigned)(i + 1),
               (unsigned)deck.slide_len);
    printf("SLIDE-SHOW: end\n");
    printf("SLIDE-SHOW: PASS (%u slides, validated)\n", (unsigned)deck.slide_len);
    return 0;
}

static view_t editor_view(void) {
    int area_x = VIEW_LEFT;
    int area_y = VIEW_TOP;
    int area_w = (int)editor_w - VIEW_LEFT - VIEW_RIGHT;
    int area_h = (int)editor_h - VIEW_TOP - VIEW_BOTTOM;
    return make_view(area_x, area_y, area_w, area_h);
}

static int toolbar_button_w(void) {
    int avail = (int)editor_w - 10;
    int width = (avail - 14 * 2) / 15;
    return width < 40 ? 40 : width;
}

static int toolbar_button_x(int i) {
    return 5 + i * (toolbar_button_w() + 2);
}

static void draw_button(int i, const char *label, int enabled) {
    int x = toolbar_button_x(i);
    int w = toolbar_button_w();
    int y = 6;
    uint32_t bg = enabled ? C_UI_PANEL2 : C_UI_PANEL;
    fill_rect(x, y, w, 32, bg);
    outline_rect(x, y, w, 32, enabled ? C_UI_ACCENT : C_UI_BORDER, 1);
    int tw = font_sans.n ? ao_text_width(&font_sans, label, strlen(label)) : 30;
    ui_text(x + (w - tw) / 2, y + 21, label, enabled ? C_UI_TEXT : C_UI_MUTED);
}

static const char *obj_label(uint32_t kind) {
    switch (kind) {
    case AO_OBJ_TEXT: return "Text";
    case AO_OBJ_RECT: return "Rectangle";
    case AO_OBJ_ROUNDED: return "Rounded rectangle";
    case AO_OBJ_ELLIPSE: return "Ellipse";
    case AO_OBJ_LINE: return "Line";
    case AO_OBJ_POLYLINE: return "Polyline";
    case AO_OBJ_IMAGE: return "Image";
    default: return "Object";
    }
}

static void draw_thumbnail(uint32_t si, int y) {
    int w = THUMB_W, h = 77;
    uint32_t bg = (si == active_slide) ? C_UI_ACCENT : C_UI_BORDER;
    fill_rect(THUMB_X - 3, y - 3, w + 6, h + 25, bg);
    render_slide(&deck, si, THUMB_X, y, w, h, 0, -1);
    char label[32];
    snprintf(label, sizeof label, "%02u  Slide %u", (unsigned)(si + 1),
             (unsigned)(si + 1));
    ui_text(THUMB_X + 3, y + h + 17, label, C_UI_TEXT);
}

static void paint_editor(void) {
    if (!editor_frame || editor_w == 0 || editor_h == 0) return;
    set_target(editor_frame, editor_w, editor_h);
    fill_rect(0, 0, (int)editor_w, (int)editor_h, C_UI_BG);
    fill_rect(0, 0, (int)editor_w, UI_BAR_H, C_UI_PANEL);
    static const char *const labels[] = {
        "+Slide", "Dup", "Del", "Text", "Rect", "Oval", "Line", "Poly",
        "Image", "Front", "Guides", "Notes", "Master", "Save", "Show F5"
    };
    for (int i = 0; i < (int)(sizeof labels / sizeof labels[0]); i++)
        draw_button(i, labels[i], i != 10 || deck.guides_on);

    /* Slide navigator. */
    fill_rect(0, UI_BAR_H, 154, (int)editor_h - UI_BAR_H, C_UI_PANEL);
    ui_text(THUMB_X, 59, "Slides", C_UI_MUTED);
    uint32_t visible = (editor_h > 180) ? (editor_h - 178) / THUMB_STEP + 1 : 1;
    if (visible > 7) visible = 7;
    uint32_t end = (uint32_t)thumb_scroll + visible;
    if (end > deck.slide_len) end = deck.slide_len;
    for (uint32_t s = (uint32_t)thumb_scroll; s < end; s++)
        draw_thumbnail(s, THUMB_TOP + (int)(s - (uint32_t)thumb_scroll) * THUMB_STEP);
    if (deck.slide_len > visible) {
        char scroll[32];
        snprintf(scroll, sizeof scroll, "%u-%u of %u",
                 (unsigned)(thumb_scroll + 1), (unsigned)end,
                 (unsigned)deck.slide_len);
        ui_text(THUMB_X, (int)editor_h - 62, scroll, C_UI_MUTED);
    }

    /* Main slide and object selection. */
    view_t view = editor_view();
    render_slide(&deck, active_slide, view.x, view.y, view.w, view.h,
                 guides_visible, selected_obj);

    /* The bottom rails keep notes visible without making presenter view. */
    int note_y = (int)editor_h - 51;
    fill_rect(160, note_y - 9, (int)editor_w - 170, 24, C_UI_PANEL);
    ao_deck_slide_t *sd = &deck.slides[active_slide];
    if (editing_notes) {
        ui_text(168, note_y + 7, "Speaker notes (Esc to finish):", C_UI_SELECT);
        ui_text(362, note_y + 7, sd->notes, C_UI_TEXT);
    } else if (editing_image_path) {
        ui_text(168, note_y + 7, "Image path:", C_UI_SELECT);
        ui_text(238, note_y + 7, image_path_edit, C_UI_TEXT);
    } else if (editing_text) {
        ui_text(168, note_y + 7, "Text editing  |  Enter adds a line  |  Esc finishes",
                C_UI_SELECT);
    } else {
        char info[180];
        const char *sel = (selected_obj >= 0 &&
                           (uint32_t)selected_obj < sd->obj_len)
                              ? obj_label(sd->objs[selected_obj].kind) : "No object";
        snprintf(info, sizeof info, "Slide %u/%u  |  %s  |  %s  |  Ctrl+S Save  F5 Show",
                 (unsigned)(active_slide + 1), (unsigned)deck.slide_len,
                 sel, deck.guides_on ? "Guides on" : "Guides off");
        ui_text(168, note_y + 7, info, C_UI_MUTED);
    }
    fill_rect(0, (int)editor_h - 24, (int)editor_w, 24, C_UI_PANEL);
    ui_text(10, (int)editor_h - 8, current_path, C_UI_MUTED);
    ui_text((int)editor_w - 208, (int)editor_h - 8,
            status_msg[0] ? status_msg : "Ready", C_UI_TEXT);
    (void)ag_blit(editor_wid, 0, 0, editor_w, editor_h,
                  editor_frame, editor_w);
}

static void paint_show(void) {
    if (!show_frame || show_wid < 0) return;
    set_target(show_frame, show_w, show_h);
    fill_rect(0, 0, (int)show_w, (int)show_h, 0x00000000);
    render_slide(&deck, (uint32_t)show_slide, 0, 0,
                 (int)show_w, (int)show_h, 0, -1);
    (void)ag_blit(show_wid, 0, 0, show_w, show_h, show_frame, show_w);
}

static int ensure_frame(uint32_t **frame, uint32_t w, uint32_t h) {
    if (!w || !h || w > APP_W_MAX || h > APP_H_MAX ||
        (uint64_t)w * h > SIZE_MAX / sizeof(uint32_t)) return -1;
    uint32_t *p = (uint32_t *)calloc((size_t)w * h, sizeof(uint32_t));
    if (!p) return -1;
    free(*frame);
    *frame = p;
    return 0;
}

static void begin_show(void) {
    if (show_wid >= 0) return;
    if (ag_screen_size(&screen_w, &screen_h) != 0 || !screen_w || !screen_h ||
        screen_w > APP_W_MAX || screen_h > APP_H_MAX) {
        set_status("Cannot query display size");
        return;
    }
    if (ensure_frame(&show_frame, screen_w, screen_h) != 0) {
        set_status("Cannot allocate presenter surface");
        return;
    }
    show_w = screen_w; show_h = screen_h; show_slide = 0;
    show_wid = ag_window_create(0, 0, show_w, show_h, "AuraLite Slide Show",
                                AG_WIN_PRESENTATION);
    if (show_wid < 0) {
        free(show_frame); show_frame = NULL; show_wid = -1;
        set_status("Cannot create full-screen show window");
        return;
    }
    (void)ag_window_show(show_wid);
    (void)ag_window_focus(show_wid);
    paint_show();
    printf("SLIDE-SHOW: slide 1/%u\n", (unsigned)deck.slide_len);
}

static void end_show(const char *reason) {
    if (show_wid < 0) return;
    if (reason && reason[0]) printf("SLIDE-SHOW: end (%s)\n", reason);
    else printf("SLIDE-SHOW: end\n");
    int old = show_wid;
    show_wid = -1;
    (void)ag_window_destroy(old);
    free(show_frame); show_frame = NULL;
    show_w = show_h = 0;
    if (editor_wid >= 0) (void)ag_window_focus(editor_wid);
    paint_editor();
}

static void advance_show(void) {
    if (show_wid < 0) return;
    if ((uint32_t)(show_slide + 1) >= deck.slide_len) {
        end_show("");
        return;
    }
    show_slide++;
    paint_show();
    printf("SLIDE-SHOW: slide %u/%u\n", (unsigned)(show_slide + 1),
           (unsigned)deck.slide_len);
}

static uint32_t key_char(const ag_event_t *e) {
    if (e->data) return e->data;
    if (e->key < 0x100) return e->key;
    return 0;
}

static int utf8_encode(uint32_t cp, char out[4]) {
    if (cp <= 0x7F) { out[0] = (char)cp; return 1; }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F)); return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
    }
    return 0;
}

static void push_undo(void) {
    ao_deck_undo_push(&deck);
    save_dirty = 1;
}

static void insert_text_bytes(ao_deck_obj_t *o, uint32_t *caret,
                              const char *s, uint32_t n) {
    if (!o || !caret || !s || !n) return;
    push_undo();
    int rc = ao_model_insert(&o->md, *caret, s, n);
    if (rc != AO_OK) {
        set_status("Text box full or invalid UTF-8");
        return;
    }
    (void)ao_model_set_size_font(&o->md, *caret, n,
                                 o->t_size ? o->t_size : 18, o->t_font);
    if (o->t_bold) (void)ao_model_set_style(&o->md, *caret, n, AO_STYLE_BOLD, 1);
    *caret += n;
}

static void set_current_slide(uint32_t s) {
    if (s >= deck.slide_len) return;
    active_slide = s;
    selected_obj = -1;
    editing_text = editing_notes = editing_image_path = 0;
    if (active_slide < (uint32_t)thumb_scroll) thumb_scroll = (int)active_slide;
    paint_editor();
}

static void add_new_slide(void) {
    uint32_t new_s = 0;
    push_undo();
    if (ao_deck_slide_add(&deck, active_slide, &new_s) != AO_DECK_OK) {
        set_status("Slide limit reached"); return;
    }
    active_slide = new_s; selected_obj = -1;
    uint32_t idx = 0;
    if (ao_deck_obj_add(&deck, active_slide, AO_OBJ_TEXT, &idx) == AO_DECK_OK) {
        ao_deck_obj_t *o = &deck.slides[active_slide].objs[idx];
        o->x = 80; o->y = 72; o->w = 800; o->h = 100;
        o->t_size = 30; o->t_bold = 1; o->t_color = 0x001D2C3D;
        (void)ao_model_set_size_font(&o->md, 0, 0, 30, AO_FONT_SANS);
        selected_obj = (int)idx; editing_text = 1; text_caret = 0;
    }
    if ((int)active_slide >= thumb_scroll + 6) thumb_scroll = (int)active_slide - 5;
    set_status("New slide");
    paint_editor();
}

static void duplicate_slide(void) {
    uint32_t idx = 0;
    push_undo();
    if (ao_deck_slide_dup(&deck, active_slide, &idx) != AO_DECK_OK) {
        set_status("Slide limit reached"); return;
    }
    active_slide = idx; selected_obj = -1;
    if ((int)active_slide >= thumb_scroll + 6) thumb_scroll = (int)active_slide - 5;
    set_status("Slide duplicated");
    paint_editor();
}

static void delete_slide(void) {
    if (deck.slide_len <= 1) { set_status("A deck needs one slide"); return; }
    push_undo();
    (void)ao_deck_slide_remove(&deck, active_slide);
    if (active_slide >= deck.slide_len) active_slide = deck.slide_len - 1;
    selected_obj = -1;
    if (thumb_scroll >= (int)deck.slide_len) thumb_scroll = (int)deck.slide_len - 1;
    set_status("Slide deleted");
    paint_editor();
}

static void move_slide(int delta) {
    int32_t to = (int32_t)active_slide + delta;
    if (to < 0 || (uint32_t)to >= deck.slide_len) return;
    push_undo();
    (void)ao_deck_slide_move(&deck, active_slide, (uint32_t)to);
    active_slide = (uint32_t)to;
    selected_obj = -1;
    set_status("Slide reordered");
    paint_editor();
}

static void add_object(uint32_t kind) {
    uint32_t idx = 0;
    push_undo();
    if (ao_deck_obj_add(&deck, active_slide, kind, &idx) != AO_DECK_OK) {
        set_status("Object limit reached"); return;
    }
    selected_obj = (int)idx;
    ao_deck_obj_t *o = &deck.slides[active_slide].objs[idx];
    if (kind == AO_OBJ_TEXT) {
        o->t_size = 24; o->t_color = deck.master_color;
        editing_text = 1; text_caret = 0;
    } else if (kind == AO_OBJ_POLYLINE) {
        o->npts = 4;
        o->pts_x[0] = 140; o->pts_y[0] = 390;
        o->pts_x[1] = 270; o->pts_y[1] = 250;
        o->pts_x[2] = 400; o->pts_y[2] = 365;
        o->pts_x[3] = 530; o->pts_y[3] = 220;
        o->stroke = 0x00D95D48; o->stroke_on = 1;
    } else if (kind == AO_OBJ_IMAGE) {
        image_path_edit[0] = 0;
        editing_image_path = 1;
    }
    set_status("Object added");
    paint_editor();
}

static void duplicate_object(void) {
    ao_deck_slide_t *sd = &deck.slides[active_slide];
    if (selected_obj < 0 || (uint32_t)selected_obj >= sd->obj_len ||
        sd->obj_len >= AO_DECK_MAX_OBJECTS) return;
    uint32_t old = (uint32_t)selected_obj, idx = 0;
    ao_deck_obj_t *src = &sd->objs[old];
    push_undo();
    if (ao_deck_obj_add(&deck, active_slide, src->kind, &idx) != AO_DECK_OK) return;
    ao_deck_obj_t *dst = &sd->objs[idx];
    memcpy(dst, src, sizeof *dst);
    rebind_obj(dst);
    dst->x += 22; dst->y += 22;
    if (dst->kind == AO_OBJ_LINE) { dst->w += 22; dst->h += 22; }
    if (dst->kind == AO_OBJ_POLYLINE) {
        for (uint8_t i = 0; i < dst->npts; i++) {
            dst->pts_x[i] += 22; dst->pts_y[i] += 22;
        }
    }
    selected_obj = (int)idx;
    set_status("Object duplicated");
    paint_editor();
}

static void remove_selected_object(void) {
    ao_deck_slide_t *sd = &deck.slides[active_slide];
    if (selected_obj < 0 || (uint32_t)selected_obj >= sd->obj_len) return;
    push_undo();
    (void)ao_deck_obj_remove(&deck, active_slide, (uint32_t)selected_obj);
    selected_obj = -1;
    editing_text = 0;
    set_status("Object removed");
    paint_editor();
}

static void move_selected_object(int dx, int dy) {
    ao_deck_slide_t *sd = &deck.slides[active_slide];
    if (selected_obj < 0 || (uint32_t)selected_obj >= sd->obj_len) return;
    ao_deck_obj_t *o = &sd->objs[selected_obj];
    push_undo();
    o->x += dx; o->y += dy;
    if (o->kind == AO_OBJ_LINE) { o->w += dx; o->h += dy; }
    if (o->kind == AO_OBJ_POLYLINE)
        for (uint8_t i = 0; i < o->npts; i++) {
            o->pts_x[i] += (int16_t)dx; o->pts_y[i] += (int16_t)dy;
        }
    set_status("Object moved");
    paint_editor();
}

static void set_z_order(int front) {
    if (selected_obj < 0) return;
    push_undo();
    if (front) {
        (void)ao_deck_obj_to_front(&deck, active_slide, (uint32_t)selected_obj);
        selected_obj = (int)deck.slides[active_slide].obj_len - 1;
    } else {
        (void)ao_deck_obj_to_back(&deck, active_slide, (uint32_t)selected_obj);
        selected_obj = 0;
    }
    set_status(front ? "Brought to front" : "Sent to back");
    paint_editor();
}

static void toggle_guides(void) {
    push_undo(); deck.guides_on ^= 1u;
    set_status(deck.guides_on ? "Guides on" : "Guides off");
    paint_editor();
}

static void toggle_page_numbers(void) {
    push_undo(); deck.master_page_numbers ^= 1u;
    set_status(deck.master_page_numbers ? "Page numbers on" : "Page numbers off");
    paint_editor();
}

static void toggle_master_theme(void) {
    push_undo();
    if ((deck.master_bg & 0x00FFFFFFu) > 0x007F7F7Fu) {
        deck.master_bg = 0x001B2541; deck.master_color = 0x00FFFFFF;
    } else {
        deck.master_bg = 0x00FFFFFF; deck.master_color = 0x00101826;
    }
    deck.master_bold ^= 1u;
    set_status("Master theme changed");
    paint_editor();
}

static void object_text_toggle_bold(void) {
    ao_deck_slide_t *sd = &deck.slides[active_slide];
    if (selected_obj < 0 || (uint32_t)selected_obj >= sd->obj_len) return;
    ao_deck_obj_t *o = &sd->objs[selected_obj];
    if (o->kind != AO_OBJ_TEXT || o->md.text_len == 0) return;
    push_undo();
    uint8_t old = o->md.runs[0].style & AO_STYLE_BOLD;
    (void)ao_model_set_style(&o->md, 0, o->md.text_len, AO_STYLE_BOLD, old == 0);
    set_status(old ? "Bold off" : "Bold on");
    paint_editor();
}

static void undo_redo(int redo) {
    int rc = redo ? ao_deck_redo(&deck) : ao_deck_undo(&deck);
    if (rc) {
        if (active_slide >= deck.slide_len) active_slide = deck.slide_len - 1;
        selected_obj = -1; editing_text = editing_notes = editing_image_path = 0;
        save_dirty = 1;
        set_status(redo ? "Redo" : "Undo");
        paint_editor();
    }
}

static void set_image_path(const char *path) {
    if (selected_obj < 0 || (uint32_t)selected_obj >= deck.slides[active_slide].obj_len)
        return;
    ao_deck_obj_t *o = &deck.slides[active_slide].objs[selected_obj];
    snprintf(o->img_path, sizeof o->img_path, "%s", path);
    editing_image_path = 0;
    save_dirty = 1;
    set_status(o->img_path[0] ? "Image path set" : "Image path cleared");
    paint_editor();
}

static void edit_notes_key(const ag_event_t *e) {
    ao_deck_slide_t *sd = &deck.slides[active_slide];
    uint32_t n = (uint32_t)strlen(sd->notes);
    uint32_t cp = key_char(e);
    if (e->key == 27) { editing_notes = 0; paint_editor(); return; }
    if ((e->mods & M_CTRL) && (cp == 's' || cp == 'S')) {
        (void)save_deck(current_path); paint_editor(); return;
    }
    if (cp == 8 || cp == 127 || e->key == K_DEL) {
        if (n) {
            push_undo();
            uint32_t start = (uint32_t)ao_utf8_prev(sd->notes, n);
            sd->notes[start] = 0;
        }
        paint_editor(); return;
    }
    if (cp == 13) cp = '\n';
    if (cp >= 32 || cp == '\n' || cp == '\t') {
        char enc[4]; int k = utf8_encode(cp, enc);
        if (k > 0 && n + (uint32_t)k < sizeof sd->notes) {
            push_undo(); memcpy(sd->notes + n, enc, (size_t)k);
            sd->notes[n + (uint32_t)k] = 0;
        }
        paint_editor();
    }
}

static void edit_image_key(const ag_event_t *e) {
    uint32_t cp = key_char(e);
    size_t n = strlen(image_path_edit);
    if (e->key == 27) { editing_image_path = 0; paint_editor(); return; }
    if (cp == 13) { set_image_path(image_path_edit); return; }
    if (cp == 8 || cp == 127 || e->key == K_DEL) {
        if (n) image_path_edit[(uint32_t)ao_utf8_prev(image_path_edit, (uint32_t)n)] = 0;
        paint_editor(); return;
    }
    if (cp >= 32) {
        char enc[4]; int k = utf8_encode(cp, enc);
        if (k > 0 && n + (size_t)k < sizeof image_path_edit) {
            memcpy(image_path_edit + n, enc, (size_t)k);
            image_path_edit[n + (size_t)k] = 0;
        }
        paint_editor();
    }
}

static void edit_text_key(const ag_event_t *e) {
    ao_deck_slide_t *sd = &deck.slides[active_slide];
    if (selected_obj < 0 || (uint32_t)selected_obj >= sd->obj_len) {
        editing_text = 0; return;
    }
    ao_deck_obj_t *o = &sd->objs[selected_obj];
    if (o->kind != AO_OBJ_TEXT) { editing_text = 0; return; }
    uint32_t cp = key_char(e);
    if (e->key == 27) { editing_text = 0; set_status("Text edit finished"); paint_editor(); return; }
    if ((e->mods & M_CTRL) && (cp == 's' || cp == 'S')) {
        (void)save_deck(current_path); paint_editor(); return;
    }
    if ((e->mods & M_CTRL) && (cp == 'b' || cp == 'B')) {
        object_text_toggle_bold(); return;
    }
    if (e->key == K_LEFT) { text_caret = ao_model_prev_cp(&o->md, text_caret); paint_editor(); return; }
    if (e->key == K_RIGHT) { text_caret = ao_model_next_cp(&o->md, text_caret); paint_editor(); return; }
    if (e->key == K_HOME) { text_caret = 0; paint_editor(); return; }
    if (e->key == K_END) { text_caret = o->md.text_len; paint_editor(); return; }
    if (cp == 8 || cp == 127) {
        if (text_caret) {
            uint32_t prev = ao_model_prev_cp(&o->md, text_caret);
            push_undo();
            if (ao_model_delete(&o->md, prev, text_caret - prev) == AO_OK)
                text_caret = prev;
        }
        paint_editor(); return;
    }
    if (e->key == K_DEL) {
        if (text_caret < o->md.text_len) {
            uint32_t next = ao_model_next_cp(&o->md, text_caret);
            push_undo(); (void)ao_model_delete(&o->md, text_caret, next - text_caret);
        }
        paint_editor(); return;
    }
    if (cp == 13) cp = '\n';
    if (cp >= 32 || cp == '\n' || cp == '\t') {
        char enc[4]; int n = utf8_encode(cp, enc);
        if (n > 0) insert_text_bytes(o, &text_caret, enc, (uint32_t)n);
        paint_editor();
    }
}

static void add_text_from_toolbar(void) { add_object(AO_OBJ_TEXT); }

static void toolbar_action(int idx) {
    switch (idx) {
    case 0: add_new_slide(); break;
    case 1: duplicate_slide(); break;
    case 2: delete_slide(); break;
    case 3: add_text_from_toolbar(); break;
    case 4: add_object(AO_OBJ_RECT); break;
    case 5: add_object(AO_OBJ_ELLIPSE); break;
    case 6: add_object(AO_OBJ_LINE); break;
    case 7: add_object(AO_OBJ_POLYLINE); break;
    case 8: add_object(AO_OBJ_IMAGE); break;
    case 9: set_z_order(1); break;
    case 10: toggle_guides(); break;
    case 11:
        editing_notes = !editing_notes;
        editing_text = editing_image_path = 0;
        paint_editor(); break;
    case 12: toggle_master_theme(); break;
    case 13: (void)save_deck(current_path); paint_editor(); break;
    case 14: begin_show(); break;
    default: break;
    }
}

static int editor_to_slide(int px, int py, int32_t *sx, int32_t *sy) {
    view_t v = editor_view();
    if (px < v.x || py < v.y || px >= v.x + v.w || py >= v.y + v.h ||
        v.scale_fp <= 0) return 0;
    int64_t xx = ((int64_t)(px - v.slide_x) * FP_ONE) / v.scale_fp;
    int64_t yy = ((int64_t)(py - v.slide_y) * FP_ONE) / v.scale_fp;
    if (xx < 0 || yy < 0 || xx >= AO_SLIDE_W || yy >= AO_SLIDE_H) return 0;
    *sx = (int32_t)xx; *sy = (int32_t)yy;
    return 1;
}

static int drag_active;
static int drag_resize;
static int drag_committed;
static int drag_obj;
static int32_t drag_start_x, drag_start_y;
static int32_t drag_orig_x, drag_orig_y, drag_orig_w, drag_orig_h;

static int point_near_handle(const ao_deck_obj_t *o, int px, int py) {
    view_t v = editor_view();
    int32_t lx, ly, lw, lh;
    object_bounds(o, &lx, &ly, &lw, &lh);
    int x = map_coord(v.slide_x, lx + lw, v.scale_fp);
    int y = map_coord(v.slide_y, ly + lh, v.scale_fp);
    return px >= x - 10 && px <= x + 10 && py >= y - 10 && py <= y + 10;
}

static void mouse_down(const ag_event_t *e) {
    if (e->y < UI_BAR_H) {
        int step = toolbar_button_w() + 2;
        int idx = (e->x - 5) / step;
        if (e->x >= 5 && idx >= 0 && idx < 15 &&
            e->x < toolbar_button_x(idx) + toolbar_button_w())
            toolbar_action(idx);
        return;
    }
    if (e->x < 154 && e->y >= THUMB_TOP) {
        int row = (e->y - THUMB_TOP) / THUMB_STEP;
        int s = thumb_scroll + row;
        if (row >= 0 && s >= 0 && (uint32_t)s < deck.slide_len) set_current_slide((uint32_t)s);
        return;
    }
    if (e->y >= (int)editor_h - 54 && e->x >= 160) {
        editing_notes = 1; editing_text = editing_image_path = 0;
        paint_editor(); return;
    }
    if (editing_text || editing_notes || editing_image_path) {
        editing_text = editing_notes = editing_image_path = 0;
    }
    int32_t sx, sy;
    if (!editor_to_slide(e->x, e->y, &sx, &sy)) {
        selected_obj = -1; paint_editor(); return;
    }
    int32_t hit = ao_deck_obj_at(&deck, active_slide, sx, sy);
    selected_obj = (int)hit;
    if (hit >= 0) {
        ao_deck_obj_t *o = &deck.slides[active_slide].objs[hit];
        drag_active = 1; drag_committed = 0; drag_obj = (int)hit;
        drag_resize = point_near_handle(o, e->x, e->y);
        drag_start_x = sx; drag_start_y = sy;
        drag_orig_x = o->x; drag_orig_y = o->y;
        drag_orig_w = o->w; drag_orig_h = o->h;
        (void)ag_window_capture(editor_wid);
    }
    paint_editor();
}

static void mouse_move(const ag_event_t *e) {
    if (!drag_active || !(e->buttons & 1) || drag_obj < 0 ||
        (uint32_t)drag_obj >= deck.slides[active_slide].obj_len) return;
    int32_t sx, sy;
    view_t v = editor_view();
    if (v.scale_fp <= 0) return;
    sx = (int32_t)(((int64_t)(e->x - v.slide_x) * FP_ONE) / v.scale_fp);
    sy = (int32_t)(((int64_t)(e->y - v.slide_y) * FP_ONE) / v.scale_fp);
    int32_t dx = sx - drag_start_x, dy = sy - drag_start_y;
    if (!dx && !dy) return;
    ao_deck_obj_t *o = &deck.slides[active_slide].objs[drag_obj];
    if (!drag_committed) { push_undo(); drag_committed = 1; }
    if (drag_resize) {
        o->x = drag_orig_x; o->y = drag_orig_y;
        o->w = drag_orig_w + dx; o->h = drag_orig_h + dy;
        if (o->w < 16) o->w = 16;
        if (o->h < 16) o->h = 16;
    } else {
        int32_t nx = drag_orig_x + dx, ny = drag_orig_y + dy;
        if (deck.guides_on) {
            int32_t sxout = nx, syout = ny;
            uint32_t n = ao_snap_box(nx, ny, o->w, o->h,
                VGUIDES, (uint32_t)(sizeof VGUIDES / sizeof VGUIDES[0]),
                HGUIDES, (uint32_t)(sizeof HGUIDES / sizeof HGUIDES[0]),
                12, &sxout, &syout);
            (void)n;
            nx = sxout; ny = syout;
        }
        o->x = nx; o->y = ny;
        if (o->kind == AO_OBJ_LINE) { o->w = drag_orig_w + dx; o->h = drag_orig_h + dy; }
        if (o->kind == AO_OBJ_POLYLINE) {
            int16_t ddx = (int16_t)dx, ddy = (int16_t)dy;
            for (uint8_t i = 0; i < o->npts; i++) {
                o->pts_x[i] += ddx; o->pts_y[i] += ddy;
            }
        }
    }
    paint_editor();
}

static void mouse_up(void) {
    if (!drag_active) return;
    drag_active = 0; drag_obj = -1; drag_committed = 0;
    (void)ag_window_capture(-1);
    if (save_dirty) set_status("Modified");
    paint_editor();
}

static void handle_editor_key(const ag_event_t *e) {
    if (editing_text) { edit_text_key(e); return; }
    if (editing_notes) { edit_notes_key(e); return; }
    if (editing_image_path) { edit_image_key(e); return; }
    uint32_t cp = key_char(e);
    int ctrl = (e->mods & M_CTRL) != 0;
    int shift = (e->mods & M_SHIFT) != 0;
    int alt = (e->mods & M_ALT) != 0;
    if (e->key == K_F5) { begin_show(); return; }
    if (e->key == 27) { running = 0; return; }
    if (ctrl && (cp == 's' || cp == 'S')) { (void)save_deck(current_path); paint_editor(); return; }
    if (ctrl && (cp == 'z' || cp == 'Z')) { undo_redo(0); return; }
    if (ctrl && (cp == 'y' || cp == 'Y')) { undo_redo(1); return; }
    if (ctrl && (cp == 'm' || cp == 'M')) { toggle_master_theme(); return; }
    if (ctrl && (cp == 'd' || cp == 'D')) { duplicate_object(); return; }
    if (ctrl && e->key == K_UP) { move_slide(-1); return; }
    if (ctrl && e->key == K_DOWN) { move_slide(1); return; }
    if (alt && e->key == K_UP) { set_z_order(1); return; }
    if (alt && e->key == K_DOWN) { set_z_order(0); return; }
    if (e->key == K_LEFT) {
        if (selected_obj >= 0) move_selected_object(-2, 0);
        else if (active_slide > 0) set_current_slide(active_slide - 1);
        return;
    }
    if (e->key == K_RIGHT) {
        if (selected_obj >= 0) move_selected_object(2, 0);
        else if (active_slide + 1 < deck.slide_len) set_current_slide(active_slide + 1);
        return;
    }
    if (e->key == K_UP) {
        if (selected_obj >= 0) move_selected_object(0, -2);
        else if (thumb_scroll > 0) { thumb_scroll--; paint_editor(); }
        return;
    }
    if (e->key == K_DOWN) {
        if (selected_obj >= 0) move_selected_object(0, 2);
        else if ((uint32_t)(thumb_scroll + 1) < deck.slide_len) { thumb_scroll++; paint_editor(); }
        return;
    }
    if (e->key == K_PGUP) { if (active_slide) set_current_slide(active_slide - 1); return; }
    if (e->key == K_PGDN) { if (active_slide + 1 < deck.slide_len) set_current_slide(active_slide + 1); return; }
    if (e->key == K_DEL || cp == 127) { remove_selected_object(); return; }
    if (shift && (cp == 'n' || cp == 'N')) {
        editing_notes = 1; paint_editor(); return;
    }
    if (cp == 'n' || cp == 'N') { add_new_slide(); return; }
    if (cp == 'd' || cp == 'D') { duplicate_slide(); return; }
    if (cp == 't' || cp == 'T') { add_object(AO_OBJ_TEXT); return; }
    if (cp == 'r' || cp == 'R') { add_object(AO_OBJ_RECT); return; }
    if (cp == 'o' || cp == 'O') { add_object(AO_OBJ_ROUNDED); return; }
    if (cp == 'e' || cp == 'E') { add_object(AO_OBJ_ELLIPSE); return; }
    if (cp == 'l' || cp == 'L') { add_object(AO_OBJ_LINE); return; }
    if (cp == 'p' || cp == 'P') { add_object(AO_OBJ_POLYLINE); return; }
    if (cp == 'i' || cp == 'I') { add_object(AO_OBJ_IMAGE); return; }
    if (cp == 'g' || cp == 'G') { toggle_guides(); return; }
    if (cp == '#' || (shift && cp == '3')) { toggle_page_numbers(); return; }
    if (cp == 'f' || cp == 'F') { set_z_order(1); return; }
    if (cp == 'b' || cp == 'B') { object_text_toggle_bold(); return; }
    if (e->type == AG_EVT_MOUSE_WHEEL) {
        if (e->key > 0 && thumb_scroll > 0) thumb_scroll--;
        else if ((int32_t)e->key < 0 && (uint32_t)(thumb_scroll + 1) < deck.slide_len) thumb_scroll++;
        paint_editor();
    }
}

static int start_editor(void) {
    /* GET_SCREEN is intentionally limited to an existing GUI participant, so
     * create a modest bootstrap window first, then fit it to the real display. */
    uint32_t outer_w = 900, outer_h = 620;
    editor_wid = ag_window_create(32, 28, outer_w, outer_h,
                                  "AuraLite Slide", AG_WIN_DEFAULT);
    if (editor_wid < 0) return -1;
    if (ag_screen_size(&screen_w, &screen_h) == 0 && screen_w && screen_h) {
        uint32_t fit_w = screen_w > 1000 ? 960 : (screen_w > 40 ? screen_w - 40 : screen_w);
        uint32_t fit_h = screen_h > 730 ? 690 : (screen_h > 70 ? screen_h - 70 : screen_h);
        if (fit_w > 100 && fit_h > 80 &&
            ag_window_resize(editor_wid, fit_w, fit_h) == 0) {
            outer_w = fit_w; outer_h = fit_h;
        }
        (void)ag_window_move(editor_wid,
            (int32_t)(screen_w > outer_w ? (screen_w - outer_w) / 2 : 0),
            (int32_t)(screen_h > outer_h ? (screen_h - outer_h) / 2 : 0));
    } else {
        screen_w = outer_w; screen_h = outer_h;
    }
    (void)ag_window_get_size(editor_wid, &editor_w, &editor_h);
    if (!editor_w || !editor_h || editor_w > APP_W_MAX || editor_h > APP_H_MAX ||
        ensure_frame(&editor_frame, editor_w, editor_h) != 0) {
        (void)ag_window_destroy(editor_wid); editor_wid = -1; return -1;
    }
    (void)ag_window_show(editor_wid);
    (void)ag_window_focus(editor_wid);
    printf("[slide] gui: deck ready (%u slides)\n", (unsigned)deck.slide_len);
    paint_editor();
    return 0;
}

static void handle_show_event(const ag_event_t *e) {
    if (e->type == AG_EVT_CLOSE_REQ ||
        (e->type == AG_EVT_KEY_DOWN && e->key == 27)) {
        end_show("Esc");
        return;
    }
    if (e->type == AG_EVT_MOUSE_DOWN) { advance_show(); return; }
    if (e->type != AG_EVT_KEY_DOWN) return;
    if (e->key == 32 || e->key == K_RIGHT || e->key == K_DOWN ||
        e->key == K_PGDN || e->key == 13 || e->data == ' ') {
        advance_show(); return;
    }
    if (e->key == K_LEFT || e->key == K_UP || e->key == K_PGUP) {
        if (show_slide > 0) {
            show_slide--; paint_show();
            printf("SLIDE-SHOW: slide %u/%u\n", (unsigned)(show_slide + 1),
                   (unsigned)deck.slide_len);
        }
    }
}

static void app_loop(void) {
    while (running) {
        ag_event_t e;
        int target = show_wid >= 0 ? show_wid : editor_wid;
        if (target < 0 || !ag_poll_event(target, &e)) {
            for (volatile int i = 0; i < 100000; i++) {}
            continue;
        }
        if (show_wid >= 0) {
            handle_show_event(&e);
            continue;
        }
        if (e.type == AG_EVT_CLOSE_REQ) { running = 0; continue; }
        if (e.type == AG_EVT_RESIZE) {
            uint32_t nw = 0, nh = 0;
            (void)ag_window_get_size(editor_wid, &nw, &nh);
            if (ensure_frame(&editor_frame, nw, nh) == 0) {
                editor_w = nw; editor_h = nh;
            } else {
                set_status("Resize refused");
            }
            paint_editor(); continue;
        }
        if (e.type == AG_EVT_MOUSE_DOWN) { mouse_down(&e); continue; }
        if (e.type == AG_EVT_MOUSE_MOVE) { mouse_move(&e); continue; }
        if (e.type == AG_EVT_MOUSE_UP) { mouse_up(); continue; }
        if (e.type == AG_EVT_MOUSE_WHEEL) { handle_editor_key(&e); continue; }
        if (e.type == AG_EVT_KEY_DOWN) { handle_editor_key(&e); continue; }
    }
}

static void cleanup(void) {
    if (show_wid >= 0) {
        int old = show_wid; show_wid = -1;
        (void)ag_window_destroy(old);
    }
    if (editor_wid >= 0) {
        int old = editor_wid; editor_wid = -1;
        (void)ag_window_destroy(old);
    }
    free(editor_frame); editor_frame = NULL;
    free(show_frame); show_frame = NULL;
    for (uint32_t i = 0; i < IMG_CACHE_N; i++) {
        free(img_cache[i].pixels); img_cache[i].pixels = NULL;
    }
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--demo") == 0) {
        const char *path = argc >= 3 ? argv[2] : DEFAULT_DECK_PATH;
        return demo_receipt(path);
    }
    if (argc >= 2 && strcmp(argv[1], "--show") == 0) {
        if (argc < 3) {
            printf("usage: aoslide --show <file.aoslide>\n");
            return 1;
        }
        return show_receipt(argv[2]);
    }
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 ||
                      strcmp(argv[1], "-h") == 0)) {
        printf("usage: aoslide [file.aoslide] | --demo [file] | --show <file>\n");
        return 0;
    }

    ao_deck_init(&deck);
    if (argc >= 2) {
        if (load_deck(argv[1]) != 0) {
            printf("[slide] open failed: %s\n", argv[1]);
            return 1;
        }
    } else if (load_deck(DEFAULT_DECK_PATH) != 0) {
        /* A fresh deck starts with a useful title placeholder. */
        ao_deck_init(&deck);
        (void)add_text_obj(0, 100, 150, 760, 100, "Untitled presentation",
                           AO_PARA_H1, 28, deck.master_color, 1);
        (void)add_text_obj(0, 104, 260, 720, 60, "Press F5 to present",
                           AO_PARA_NORMAL, 16, 0x00546576, 0);
        snprintf(current_path, sizeof current_path, "%s", DEFAULT_DECK_PATH);
    }

    fonts_init();
    if (start_editor() != 0) {
        printf("[slide] gui: cannot create editor window\n");
        return 1;
    }
    app_loop();
    cleanup();
    return 0;
}

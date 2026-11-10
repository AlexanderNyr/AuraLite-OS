/* userspace/office/libaoglyph/aoglyph.c — see aoglyph.h for what this is for.
 *
 * Pure C: no syscalls, no globals, no libc allocation.  The only external
 * dependency is libaodoc (the arena and the UTF-8 decoder), which is itself
 * pure — that is what lets tests/unit/test_ao_glyph.c compile this exact file
 * against the host and grade the loaders, the fallback path and the style
 * synthesis that the shipped binaries run.
 */

#include "aoglyph.h"

#include <string.h>

/* ------------------------------------------------------------------ byte IO */

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Iterate lines of a non-NUL-terminated buffer. */
static int next_line(const char **p, const char *end,
                     const char **ls, size_t *ll) {
    if (*p >= end) return 0;
    const char *s = *p;
    const char *nl = (const char *)memchr(s, '\n', (size_t)(end - s));
    const char *e = nl ? nl : end;
    *ls = s;
    *ll = (size_t)(e - s);
    *p = nl ? nl + 1 : end;
    /* Trim CR (a BDF authored on Windows) and trailing blanks. */
    while (*ll && (ls[0][*ll - 1] == '\r' || ls[0][*ll - 1] == ' ')) (*ll)--;
    return 1;
}

/* Keyword match: the word, then end of line or whitespace.
 *
 * line_has() alone is not enough and the difference is not academic: the
 * property line "CHARSET_REGISTRY" starts with "CHARS", so a prefix match read
 * a registry name as a glyph count and refused every real BDF this generator
 * produces.  A parser that says "no" to well-formed input is still wrong. */
static int line_kw(const char *ls, size_t ll, const char *word) {
    size_t n = strlen(word);
    if (ll < n || memcmp(ls, word, n) != 0) return 0;
    return ll == n || ls[n] == ' ' || ls[n] == '\t';
}

/* Decimal integer from a line, skipping `skip` leading bytes. */
static int dec(const char *ls, size_t ll, size_t skip, int *out) {
    size_t i = skip;
    int sign = 1, v = 0, any = 0;
    while (i < ll && (ls[i] == ' ' || ls[i] == '\t')) i++;
    if (i < ll && (ls[i] == '-' || ls[i] == '+')) {
        if (ls[i] == '-') sign = -1;
        i++;
    }
    while (i < ll && ls[i] >= '0' && ls[i] <= '9') {
        v = v * 10 + (ls[i] - '0');
        if (v > 1000000) return -1;
        i++;
        any = 1;
    }
    if (!any) return -1;
    *out = v * sign;
    return 0;
}

/* ---------------------------------------------------------------- coverage */

static int find_cp(const ao_font_t *f, uint32_t cp) {
    int lo = 0, hi = (int)f->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->g[mid].cp == cp) return mid;
        if (f->g[mid].cp < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

/* The blank glyph a face without '?' falls back to.  Static const: read-only,
 * no allocation, and shared by every font, which is fine because it carries no
 * state — the caller supplies the advance. */
static const ao_glyph_t blank_glyph = { 0, NULL, 0, 0, 0, 0, 0 };

const ao_glyph_t *ao_font_glyph(const ao_font_t *f, uint32_t cp) {
    if (!f || f->n == 0) return &blank_glyph;
    int i = find_cp(f, cp);
    if (i >= 0) return &f->g[i];
    if (f->fallback != 0xFFFF) return &f->g[f->fallback];
    return &blank_glyph;
}

int ao_font_has(const ao_font_t *f, uint32_t cp) {
    return f && find_cp(f, cp) >= 0;
}

int ao_font_advance(const ao_font_t *f, uint32_t cp) {
    if (!f || f->n == 0) return 0;
    int i = find_cp(f, cp);
    if (i >= 0) return f->g[i].adv;
    if (f->fallback != 0xFFFF) return f->g[f->fallback].adv;
    return f->cell_w;
}

int ao_font_covers(const ao_font_t *f, uint32_t lo, uint32_t hi) {
    int n = 0;
    for (uint32_t cp = lo; cp <= hi; cp++)
        if (ao_font_has(f, cp)) n++;
    return n;
}

size_t ao_font_used_bytes(const ao_font_t *f) {
    return f && f->arena ? ao_arena_used(f->arena) : 0;
}

/* ------------------------------------------------------------------- BDF */

static void sort_glyphs(ao_font_t *f) {
    /* Insertion sort: n <= 512 and the input is usually already ordered, so
     * this is O(n) in practice and keeps the loader dependency-free. */
    for (int i = 1; i < (int)f->n; i++) {
        ao_glyph_t key = f->g[i];
        int j = i - 1;
        while (j >= 0 && f->g[j].cp > key.cp) {
            f->g[j + 1] = f->g[j];
            j--;
        }
        f->g[j + 1] = key;
    }
    /* Drop duplicates, keeping the first: a BDF that lists a codepoint twice is
     * damaged, and picking deterministically beats picking "whatever the
     * search lands on". */
    int out = 0;
    for (int i = 0; i < (int)f->n; i++) {
        if (out > 0 && f->g[out - 1].cp == f->g[i].cp) continue;
        f->g[out++] = f->g[i];
    }
    f->n = (uint16_t)out;
}

static int bdf_load_impl(ao_font_t *f, ao_arena_t *arena,
                         const char *data, size_t len) {
    if (!f || !arena || !data || len == 0) return AO_FONT_E_ARG;
    memset(f, 0, sizeof(*f));
    f->arena = arena;
    f->fallback = 0xFFFF;
    f->cell_w = 8;
    f->cell_h = 16;
    f->ascent = 12;
    f->descent = 4;

    const char *p = data;
    const char *end = data + len;
    const char *ls;
    size_t ll;

    int saw_start = 0, saw_end = 0;
    int declared = -1;

    /* Current glyph being assembled. */
    uint32_t cp = 0;
    int have_enc = 0;
    int gw = 0, gh = 0, gxo = 0, gyo = 0, gadv = 0;
    int have_bbx = 0;
    int in_bitmap = 0;
    int keep = 0;
    int rows_read = 0;
    uint32_t cp_of_question = 0xFFFFFFFFu;

    while (next_line(&p, end, &ls, &ll)) {
        if (ll == 0) continue;

        if (line_kw(ls, ll, "STARTFONT")) { saw_start = 1; continue; }
        if (line_kw(ls, ll, "ENDFONT")) { saw_end = 1; break; }

        if (line_kw(ls, ll, "FONT_ASCENT")) {
            int v;
            if (dec(ls, ll, 11, &v) == 0 && v > 0 && v < 256) f->ascent = (int16_t)v;
            continue;
        }
        if (line_kw(ls, ll, "FONT_DESCENT")) {
            int v;
            if (dec(ls, ll, 12, &v) == 0 && v >= 0 && v < 256) f->descent = (int16_t)v;
            continue;
        }
        if (line_kw(ls, ll, "FONTBOUNDINGBOX")) {
            /* FONTBOUNDINGBOX w h xoff yoff */
            const char *q = ls + 15;
            const char *lim = ls + ll;
            int vals[4], got = 0;
            while (got < 4 && q < lim) {
                while (q < lim && (*q == ' ' || *q == '\t')) q++;
                if (q >= lim) break;
                int sign = 1, v = 0, any = 0;
                if (*q == '-' || *q == '+') { if (*q == '-') sign = -1; q++; }
                while (q < lim && *q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; any = 1; }
                if (!any) break;
                vals[got++] = v * sign;
            }
            if (got >= 2 && vals[0] > 0 && vals[0] < 256) f->cell_w = (uint16_t)vals[0];
            if (got >= 2 && vals[1] > 0 && vals[1] < 256) f->cell_h = (uint16_t)vals[1];
            continue;
        }
        if (line_kw(ls, ll, "SPACING")) {
            const char *q = ls + 7;
            while (q < ls + ll && (*q == ' ' || *q == '"')) q++;
            f->proportional = (q < ls + ll && *q == 'P') ? 1u : 0u;
            continue;
        }
        if (line_kw(ls, ll, "CHARS")) {
            int v;
            if (dec(ls, ll, 5, &v) != 0 || v < 0) return AO_FONT_E_FORMAT;
            if (v > AO_GLYPH_MAX) return AO_FONT_E_CAPACITY;
            declared = v;
            continue;
        }
        if (line_kw(ls, ll, "STARTCHAR")) {
            if (f->n >= AO_GLYPH_MAX) return AO_FONT_E_CAPACITY;
            cp = 0; have_enc = 0; have_bbx = 0;
            gw = gh = gxo = gyo = 0; gadv = f->cell_w;
            in_bitmap = 0; keep = 0; rows_read = 0;
            continue;
        }
        if (line_kw(ls, ll, "ENCODING")) {
            int v;
            if (dec(ls, ll, 8, &v) != 0) return AO_FONT_E_FORMAT;
            if (v < 0) { have_enc = 0; }        /* unencoded glyph: skip it */
            else { cp = (uint32_t)v; have_enc = 1; }
            continue;
        }
        if (line_kw(ls, ll, "DWIDTH")) {
            int v;
            if (dec(ls, ll, 6, &v) == 0 && v >= 0 && v < 256) gadv = v;
            continue;
        }
        if (line_kw(ls, ll, "BBX")) {
            const char *q = ls + 3;
            const char *lim = ls + ll;
            int vals[4], got = 0;
            while (got < 4 && q < lim) {
                while (q < lim && (*q == ' ' || *q == '\t')) q++;
                if (q >= lim) break;
                int sign = 1, v = 0, any = 0;
                if (*q == '-' || *q == '+') { if (*q == '-') sign = -1; q++; }
                while (q < lim && *q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; any = 1; }
                if (!any) break;
                vals[got++] = v * sign;
            }
            if (got < 2) return AO_FONT_E_FORMAT;
            gw = vals[0]; gh = vals[1];
            gxo = got > 2 ? vals[2] : 0;
            gyo = got > 3 ? vals[3] : 0;
            if (gw < 0 || gw > 128 || gh < 0 || gh > 128) return AO_FONT_E_FORMAT;
            if (gxo < -128 || gxo > 128 || gyo < -128 || gyo > 128) return AO_FONT_E_FORMAT;
            have_bbx = 1;
            continue;
        }
        if (line_kw(ls, ll, "BITMAP")) {
            in_bitmap = 1;               /* consume rows whether we keep them */
            rows_read = 0;
            keep = (have_enc && have_bbx);
            if (!keep) continue;
            ao_glyph_t *g = &f->g[f->n];  /* provisional: committed at ENDCHAR */
            g->cp = cp;
            g->rows = NULL;
            g->w = (uint16_t)gw;
            g->h = (uint16_t)gh;
            g->xoff = (int16_t)gxo;
            g->yoff = (int16_t)gyo;
            g->adv = (uint16_t)gadv;
            if (gw > 0 && gh > 0) {
                size_t stride = (size_t)((gw + 7) / 8);
                size_t bytes = stride * (size_t)gh;
                uint8_t *bits = (uint8_t *)ao_arena_calloc(arena, 1, bytes);
                if (!bits) return AO_FONT_E_STORAGE;
                g->rows = bits;
            }
            continue;
        }
        if (line_kw(ls, ll, "ENDCHAR")) {
            if (in_bitmap && keep) {
                if ((int)rows_read != gh) return AO_FONT_E_TRUNCATED;
                if (cp == (uint32_t)'?') cp_of_question = cp;
                f->n++;
            }
            in_bitmap = 0;
            keep = 0;
            continue;
        }
        if (in_bitmap && keep && gw > 0 && gh > 0) {
            /* One hex row of the current glyph: ceil(gw/8) bytes, MSB leftmost,
             * the last byte padded on the right with zeros. */
            if ((int)rows_read >= gh) return AO_FONT_E_TRUNCATED;
            ao_glyph_t *g = &f->g[f->n];
            size_t stride = (size_t)((gw + 7) / 8);
            uint8_t *row = (uint8_t *)g->rows + (size_t)rows_read * stride;
            size_t nbytes = stride;
            size_t i = 0;
            for (size_t b = 0; b < nbytes; b++) {
                if (i + 1 >= ll) return AO_FONT_E_TRUNCATED;
                int hi = hexval(ls[i]);
                int lo = hexval(ls[i + 1]);
                if (hi < 0 || lo < 0) return AO_FONT_E_FORMAT;
                row[b] = (uint8_t)((hi << 4) | lo);
                i += 2;
            }
            rows_read++;
            continue;
        }
        if (in_bitmap && !keep) {
            /* Skipping an unencoded glyph: its hex rows are not code. */
            rows_read++;
            continue;
        }
    }

    if (!saw_start) return AO_FONT_E_FORMAT;
    if (!saw_end) return AO_FONT_E_TRUNCATED;    /* font ends mid-glyph */
    if (f->n == 0) return AO_FONT_E_FORMAT;
    if (declared >= 0 && (int)f->n > declared) return AO_FONT_E_FORMAT;

    sort_glyphs(f);

    if (cp_of_question != 0xFFFFFFFFu) {
        int i = find_cp(f, cp_of_question);
        if (i >= 0) f->fallback = (uint16_t)i;
    }
    if (f->ascent <= 0) f->ascent = (int16_t)(f->cell_h - f->descent);
    return AO_FONT_OK;
}

/* Every loader leaves the font EMPTY on failure.  A caller that ignores the
 * return code therefore draws nothing rather than a half-parsed face whose
 * glyph table disagrees with the file it came from. */
int ao_font_load_bdf(ao_font_t *f, ao_arena_t *arena,
                     const char *data, size_t len) {
    int rc = bdf_load_impl(f, arena, data, len);
    if (rc != AO_FONT_OK && f) f->n = 0;
    return rc;
}

/* ------------------------------------------------------------------ PSF2 */

#define PSF2_MAGIC 0x864AB572u

int ao_font_load_psf2(ao_font_t *f, ao_arena_t *arena,
                      const uint8_t *data, size_t len, int ascent) {
    if (!f || !arena || !data) return AO_FONT_E_ARG;
    memset(f, 0, sizeof(*f));
    f->arena = arena;
    f->fallback = 0xFFFF;
    if (len < 32) return AO_FONT_E_TRUNCATED;
    if (rd32le(data) != PSF2_MAGIC) return AO_FONT_E_FORMAT;
    uint32_t headersize = rd32le(data + 8);
    uint32_t flags = rd32le(data + 12);
    uint32_t numglyph = rd32le(data + 16);
    uint32_t bytesperglyph = rd32le(data + 20);
    uint32_t height = rd32le(data + 24);
    uint32_t width = rd32le(data + 28);

    if (headersize < 32 || headersize > len) return AO_FONT_E_TRUNCATED;
    if (numglyph == 0 || numglyph > AO_GLYPH_MAX) return AO_FONT_E_CAPACITY;
    if (width == 0 || width > 128 || height == 0 || height > 128)
        return AO_FONT_E_FORMAT;
    size_t stride = (size_t)((width + 7) / 8);
    if (bytesperglyph < stride * height) return AO_FONT_E_FORMAT;
    size_t glyph_bytes = (size_t)numglyph * bytesperglyph;
    if (headersize + glyph_bytes > len) return AO_FONT_E_TRUNCATED;

    f->cell_w = (uint16_t)width;
    f->cell_h = (uint16_t)height;
    f->image = data + headersize;
    f->image_len = (uint32_t)glyph_bytes;
    f->bytesperglyph = bytesperglyph;
    f->nglyph_raw = (uint16_t)numglyph;
    f->from_psf2 = 1;
    f->proportional = 0;

    int asc = (ascent > 0 && ascent <= (int)height) ? ascent
                                                   : (int)height - (int)height / 4;
    f->ascent = (int16_t)asc;
    f->descent = (int16_t)(height - (uint32_t)asc);

    /* Codepoint map.  With flags bit 0 the container carries a Unicode table
     * after the glyphs: runs of uint16 LE codepoints, each run introduced by
     * 0xFFFE (next glyph index) and terminated by 0xFFFF.  Without it, glyph i
     * is codepoint i, which is what the kernel's console does. */
    const uint8_t *uni = data + headersize + glyph_bytes;
    size_t uni_len = len - (size_t)(uni - data);

    if ((flags & 1u) && uni_len >= 2) {
        uint32_t gi = 0;
        size_t i = 0;
        while (i + 1 < uni_len) {
            uint16_t v = (uint16_t)(uni[i] | ((uint16_t)uni[i + 1] << 8));
            i += 2;
            if (v == 0xFFFEu) {
                if (i + 1 >= uni_len) break;
                gi = (uint32_t)(uni[i] | ((uint16_t)uni[i + 1] << 8));
                i += 2;
                continue;
            }
            if (v == 0xFFFFu) { gi++; continue; }
            if (gi < numglyph && f->n < AO_GLYPH_MAX) {
                ao_glyph_t *g = &f->g[f->n++];
                g->cp = v;
                g->rows = f->image + (size_t)gi * bytesperglyph;
                g->w = (uint16_t)width;
                g->h = (uint16_t)height;
                g->xoff = 0;
                g->yoff = (int16_t)(-(int)f->descent);
                g->adv = (uint16_t)width;
            }
            gi++;
        }
    } else {
        for (uint32_t i = 0; i < numglyph && f->n < AO_GLYPH_MAX; i++) {
            ao_glyph_t *g = &f->g[f->n++];
            g->cp = i;
            g->rows = f->image + (size_t)i * bytesperglyph;
            g->w = (uint16_t)width;
            g->h = (uint16_t)height;
            g->xoff = 0;
            g->yoff = (int16_t)(-(int)f->descent);
            g->adv = (uint16_t)width;
        }
    }
    if (f->n == 0) return AO_FONT_E_FORMAT;
    sort_glyphs(f);
    int qi = find_cp(f, (uint32_t)'?');
    if (qi >= 0) f->fallback = (uint16_t)qi;
    return AO_FONT_OK;
}

/* ----------------------------------------------------------------- drawing */

/* Style synthesis, in pixels, so the tests can assert it:
 *
 *   bold   — every set pixel is smeared one column to the RIGHT.  A 1-pixel
 *            smear, not a re-render at a heavier weight: it thickens stems
 *            without moving the advance, which keeps a bold word the same
 *            width as the regular one.
 *   italic — every pixel above the baseline moves right by
 *            floor((baseline - y) / 4): one pixel of lean per four rows of
 *            height.  Below the baseline nothing moves, so descenders stay
 *            vertical and the glyph keeps its footing.
 */
#define MASK_SX 4          /* slack left of the glyph for the sheared rows */
#define MASK_SY 2          /* slack above and below */
/* A glyph wider or taller than this is refused rather than allowed to overflow
 * the mask: 128 rows is far past any text face, and a 128x128 mask is 16 KiB of
 * stack, which every caller in this tree can afford. */
#define MASK_MAX 128

static void build_mask(const ao_glyph_t *g, int style,
                       uint8_t *mask, int mw, int mh, int *dy_top) {
    memset(mask, 0, (size_t)mw * (size_t)mh);
    if (!g || !g->rows || g->w == 0 || g->h == 0) return;
    int top = g->yoff + (int)g->h - 1;         /* top row, in dy (up = +) */
    *dy_top = top + MASK_SY;
    int stride_bytes = (g->w + 7) / 8;
    for (int py = 0; py < (int)g->h; py++) {
        const uint8_t *row = g->rows + (size_t)py * (size_t)stride_bytes;
        int dy = g->yoff + (int)g->h - 1 - py;
        int shear = 0;
        if ((style & AO_STYLE_ITALIC) && dy > 0) shear = dy / 4;
        int r = *dy_top - dy;
        if (r < 0 || r >= mh) continue;
        for (int px = 0; px < (int)g->w; px++) {
            uint8_t bit = (uint8_t)(row[px / 8] >> (7 - (px % 8))) & 1u;
            if (!bit) continue;
            int c = g->xoff + px + MASK_SX + shear;
            if (c < 0 || c >= mw) continue;
            mask[(size_t)r * (size_t)mw + (size_t)c] = 1;
            if ((style & AO_STYLE_BOLD) && c + 1 < mw)
                mask[(size_t)r * (size_t)mw + (size_t)(c + 1)] = 1;
        }
    }
}

int ao_glyph_draw(const ao_font_t *f, uint32_t cp, int style,
                  uint32_t *dst, int stride, int w, int h,
                  int x, int baseline_y, uint32_t color) {
    if (!dst || w <= 0 || h <= 0) return 0;
    const ao_glyph_t *g = ao_font_glyph(f, cp);
    if (!g || g->w == 0 || g->h == 0) return 0;

    int mw = (int)g->w + 2 * MASK_SX;
    int mh = (int)g->h + 2 * MASK_SY;
    if (mw > MASK_MAX || mh > MASK_MAX) return 0;   /* not a text face */
    uint8_t mask[MASK_MAX * MASK_MAX];

    int dy_top = 0;
    build_mask(g, style, mask, mw, mh, &dy_top);

    int painted = 0;
    for (int r = 0; r < mh; r++) {
        int dy = dy_top - r;
        int sy = baseline_y - dy;
        if (sy < 0 || sy >= h) continue;
        for (int c = 0; c < mw; c++) {
            if (!mask[(size_t)r * (size_t)mw + (size_t)c]) continue;
            int sx = x + c - MASK_SX;
            if (sx < 0 || sx >= w) continue;
            dst[(size_t)sy * (size_t)stride + (size_t)sx] = color;
            painted++;
        }
    }
    return painted;
}

int ao_text_draw(const ao_font_t *f, const char *s, size_t len, int style,
                 uint32_t *dst, int stride, int w, int h,
                 int x, int baseline_y, uint32_t color, int *width_out) {
    if (!f || !s) { if (width_out) *width_out = 0; return 0; }
    int pen = x, painted = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp = 0;
        int n = ao_utf8_decode(s + i, len - i, &cp);
        if (n < 0) { cp = (uint32_t)'?'; n = 1; }   /* resync one byte */
        painted += ao_glyph_draw(f, cp, style, dst, stride, w, h, pen,
                                 baseline_y, color);
        pen += ao_font_advance(f, cp);
        i += (size_t)n;
    }
    if (width_out) *width_out = pen - x;
    return painted;
}

int ao_text_width(const ao_font_t *f, const char *s, size_t len) {
    if (!f || !s) return 0;
    int pen = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp = 0;
        int n = ao_utf8_decode(s + i, len - i, &cp);
        if (n < 0) { cp = (uint32_t)'?'; n = 1; }
        pen += ao_font_advance(f, cp);
        i += (size_t)n;
    }
    return pen;
}

/* ----------------------------------------------------------- line breaking */

int ao_text_is_break(uint32_t cp) {
    switch (cp) {
    case 0x09:          /* tab */
    case 0x20:          /* space */
    case 0x2D:          /* hyphen-minus */
    case 0x2F:          /* solidus */
    case 0xAD:          /* soft hyphen */
    case 0x2010:        /* hyphen */
    case 0x2013:        /* en dash */
    case 0x2014:        /* em dash */
        return 1;
    case 0x00A0:        /* NO-BREAK space: the whole point of the character */
    case 0x202F:        /* narrow no-break space */
        return 0;
    default:
        return 0;
    }
}

size_t ao_text_fit(const ao_font_t *f, const char *s, size_t len, int max_w,
                   int *width_out) {
    if (width_out) *width_out = 0;
    if (!f || !s || len == 0 || max_w <= 0) return 0;

    int pen = 0;
    size_t i = 0;
    size_t last_break = 0;      /* byte offset just past the last break char */
    int    last_break_w = 0;
    size_t consumed = 0;        /* how much we could take ignoring breaks */
    int    consumed_w = 0;

    while (i < len) {
        uint32_t cp = 0;
        int n = ao_utf8_decode(s + i, len - i, &cp);
        if (n < 0) { cp = (uint32_t)'?'; n = 1; }
        int adv = ao_font_advance(f, cp);
        if (pen + adv > max_w) break;
        pen += adv;
        i += (size_t)n;
        consumed = i;
        consumed_w = pen;
        if (ao_text_is_break(cp)) {
            last_break = i;
            last_break_w = pen;
        }
    }

    /* Prefer the last break opportunity; when a single word is wider than the
     * line, take what fits so the caller makes progress instead of looping. */
    if (last_break > 0 && last_break < len && i < len) {
        if (width_out) *width_out = last_break_w;
        return last_break;
    }
    if (width_out) *width_out = consumed_w;
    return consumed;
}

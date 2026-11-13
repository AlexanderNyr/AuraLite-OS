/* userspace/office/libaodoc/aolayout.c — see aolayout.h. */

#include "aolayout.h"

#include <string.h>

/* ------------------------------------------------------------ styles ---- */

void ao_para_metrics(uint8_t para_style, ao_para_metrics_t *out) {
    memset(out, 0, sizeof(*out));
    out->size_px = 11;
    switch (para_style & AO_PARA_KIND_MASK) {
        case AO_PARA_H1:
            out->size_px = 20; out->space_before = 18; out->space_after = 8;
            out->bold = 1;
            break;
        case AO_PARA_H2:
            out->size_px = 16; out->space_before = 14; out->space_after = 6;
            out->bold = 1;
            break;
        case AO_PARA_H3:
            out->size_px = 13; out->space_before = 10; out->space_after = 4;
            out->bold = 1;
            break;
        case AO_PARA_QUOTE:
            out->size_px = 11; out->space_before = 6; out->space_after = 6;
            out->indent = 24; out->italic = 1;
            break;
        case AO_PARA_BULLET:
        case AO_PARA_NUMBER:
            out->space_before = 2; out->space_after = 2; out->indent = 18;
            break;
        case AO_PARA_TABLE:
            out->space_before = 0; out->space_after = 0;
            break;
        case AO_PARA_IMAGE:
            out->space_before = 6; out->space_after = 6;
            break;
        default:
            out->space_before = 4; out->space_after = 4;
            break;
    }
}

/* Line height for a text size: the suite's faces want ~1.45em of leading. */
static uint32_t line_h_for(uint8_t size_px) { return (uint32_t)size_px + 5; }

/* ------------------------------------------------------------ layout ---- */

void ao_layout_init(ao_layout_t *l, ao_layout_line_t *lines, uint32_t line_cap,
                    ao_layout_page_t *pages, uint32_t page_cap) {
    l->lines = lines; l->line_cap = line_cap; l->line_len = 0;
    l->pages = pages; l->page_cap = page_cap; l->page_len = 0;
}

typedef struct {
    ao_layout_t *l;
    const ao_page_geom_t *g;
    int32_t y;              /* cursor, page units from THIS page's top */
    uint32_t page_first;    /* first line index of the open page */
    int overflow;           /* a caller array ran out */
} lay_t;

static void lay_new_page(lay_t *t) {
    if (t->l->page_len < t->l->page_cap) {
        t->l->pages[t->l->page_len].first_line = t->page_first;
        t->l->pages[t->l->page_len].line_count =
            t->l->line_len - t->page_first;
    }
    t->l->page_len++;
    t->page_first = t->l->line_len;
    t->y = (int32_t)t->g->margin_t;
}

/* Emit one line; starts a new page first when it would cross the bottom
 * margin and the page already holds something.  A line taller than the
 * whole content area is placed anyway (and the NEXT line pages): layout
 * must never loop waiting for a page big enough. */
static void lay_emit(lay_t *t, const ao_layout_line_t *ln) {
    int32_t bottom = (int32_t)(t->g->page_h - t->g->margin_b);
    if (t->y + (int32_t)ln->h > bottom && t->l->line_len > t->page_first)
        lay_new_page(t);
    if (t->l->line_len < t->l->line_cap) {
        ao_layout_line_t *dst = &t->l->lines[t->l->line_len];
        *dst = *ln;
        dst->y = t->y;
    } else {
        t->overflow = 1;
    }
    t->l->line_len++;
    t->y += (int32_t)ln->h;
}

/* Next word boundary at or after `i`: a word is a run of non-space
 * codepoints; the returned slice [i, wend) is one word, and the spaces
 * after it are reported through `next`. */
static void next_word(const char *text, uint32_t len, uint32_t i,
                      uint32_t *wend, uint32_t *next) {
    uint32_t e = i;
    while (e < len) {
        uint32_t cp_end = e + 1;
        while (cp_end < len && ((unsigned char)text[cp_end] & 0xC0) == 0x80)
            cp_end++;
        if (text[e] == ' ' || text[e] == '\t') break;
        e = cp_end;
    }
    uint32_t nx = e;
    while (nx < len && (text[nx] == ' ')) nx++;
    *wend = e;
    *next = nx;
}

int ao_layout_build(ao_layout_t *l, const ao_model_t *m,
                    const ao_page_geom_t *g,
                    ao_measure_fn measure, void *measure_ctx,
                    ao_image_size_fn image_size, void *image_ctx) {
    if (!l || !m || !g || !measure) return AO_E_ARG;
    l->line_len = 0;
    l->page_len = 0;
    lay_t t = { l, g, (int32_t)g->margin_t, 0, 0 };
    uint32_t content_w = ao_page_content_w(g);
    uint32_t number_run = 0;     /* numbered-list continuation counter */

    for (uint32_t pi = 0; pi < m->para_len; pi++) {
        const ao_para_t *p = &m->paras[pi];
        uint8_t kind = p->style & AO_PARA_KIND_MASK;
        ao_para_metrics_t met;
        ao_para_metrics(p->style, &met);

        if ((p->style & AO_PARA_PAGEBREAK) && l->line_len > t.page_first)
            lay_new_page(&t);

        /* The paragraph's text without its terminating newline. */
        uint32_t plen = p->len;
        if (plen && m->text[p->off + plen - 1] == '\n') plen--;
        const char *ptext = m->text + p->off;

        if (kind == AO_PARA_IMAGE) {
            uint32_t iw = 0, ih = 0, dw = content_w, dh = 120;
            if (image_size && plen &&
                image_size(image_ctx, ptext, plen, &iw, &ih) == 0 && iw && ih)
                ao_image_fit(g, iw, ih, &dw, &dh);
            ao_layout_line_t ln;
            memset(&ln, 0, sizeof ln);
            ln.para = pi; ln.start = 0; ln.len = plen;
            ln.h = (uint16_t)(dh + (uint32_t)met.space_before +
                              (uint32_t)met.space_after);
            ln.size_px = met.size_px; ln.kind = kind;
            lay_emit(&t, &ln);
            number_run = 0;
            continue;
        }

        if (kind == AO_PARA_TABLE) {
            /* One row: height is one text line (cells do not wrap in OF3's
             * grid; a cell wider than its column is clipped by the painter,
             * and the grid algorithm in ao_table_layout owns the rects). */
            ao_layout_line_t ln;
            memset(&ln, 0, sizeof ln);
            ln.para = pi; ln.start = 0; ln.len = plen;
            ln.h = (uint16_t)line_h_for(met.size_px);
            ln.size_px = met.size_px; ln.kind = kind;
            lay_emit(&t, &ln);
            number_run = 0;
            continue;
        }

        /* List marker + continuation numbering. */
        char marker[AO_LAYOUT_MAX_LINE_MARKER] = "";
        if (kind == AO_PARA_BULLET) {
            /* U+2022 BULLET as literal UTF-8. */
            marker[0] = (char)0xE2; marker[1] = (char)0x80;
            marker[2] = (char)0xA2; marker[3] = 0;
            number_run = 0;
        } else if (kind == AO_PARA_NUMBER) {
            number_run++;
            /* "N." built by hand: no snprintf in the layout core. */
            char rev[8]; int ri = 0; uint32_t v = number_run;
            if (!v) rev[ri++] = '0';
            while (v) { rev[ri++] = (char)('0' + (v % 10)); v /= 10; }
            int mi = 0;
            while (ri > 0 && mi < (int)sizeof marker - 2) marker[mi++] = rev[--ri];
            marker[mi++] = '.';
            marker[mi] = 0;
        } else {
            number_run = 0;
        }

        /* Indent: style indent + the paragraph's own (list levels ride on
         * the paragraph indent, 18 units per level). */
        int32_t indent = (int32_t)met.indent + (int32_t)p->indent;
        if (indent < 0) indent = 0;
        uint32_t avail = content_w > (uint32_t)indent
                             ? content_w - (uint32_t)indent : content_w / 2;
        int32_t  marker_w = marker[0]
            ? measure(measure_ctx, marker, strlen(marker), met.size_px) + 6 : 0;

        t.y += met.space_before;
        if (plen == 0) {
            ao_layout_line_t ln;
            memset(&ln, 0, sizeof ln);
            ln.para = pi; ln.start = 0; ln.len = 0;
            ln.h = (uint16_t)line_h_for(met.size_px);
            ln.size_px = met.size_px; ln.kind = kind;
            memcpy(ln.marker, marker, sizeof ln.marker);
            lay_emit(&t, &ln);
            t.y += met.space_after;
            continue;
        }

        /* Greedy wrap: take words while they fit; a single word wider than
         * the line is hard-split by codepoints so layout always advances. */
        uint32_t i = 0;
        int first_line = 1;
        while (i < plen) {
            uint32_t line_w = (uint32_t)(first_line ? marker_w : 0);
            uint32_t j = i, last_fit_end = i, last_fit_next = i;
            while (j < plen) {
                uint32_t wend, nx;
                next_word(ptext, plen, j, &wend, &nx);
                if (wend == j) { j = nx; continue; }   /* leading spaces */
                int ww = measure(measure_ctx, ptext + j, wend - j, met.size_px);
                uint32_t add = (uint32_t)ww + (j > i ? 4u : 0u);
                if (line_w + add > avail && j > i) break;
                if (line_w + add > avail && j == i) {
                    /* One word too wide: take as many codepoints as fit. */
                    uint32_t k = j;
                    line_w = first_line ? (uint32_t)marker_w : 0;
                    while (k < wend) {
                        uint32_t ce = k + 1;
                        while (ce < plen &&
                               ((unsigned char)ptext[ce] & 0xC0) == 0x80) ce++;
                        int cw = measure(measure_ctx, ptext + k, ce - k,
                                         met.size_px);
                        if (line_w + (uint32_t)cw > avail && k > j) break;
                        line_w += (uint32_t)cw;
                        k = ce;
                    }
                    last_fit_end = k;
                    last_fit_next = k;
                    break;
                }
                line_w += add;
                last_fit_end = wend;
                last_fit_next = nx;
                j = nx;
            }
            if (last_fit_end == i) {   /* cannot happen; never hang */
                last_fit_end = i + 1;
                last_fit_next = i + 1;
            }
            ao_layout_line_t ln;
            memset(&ln, 0, sizeof ln);
            ln.para = pi; ln.start = i; ln.len = last_fit_end - i;
            ln.h = (uint16_t)line_h_for(met.size_px);
            ln.size_px = met.size_px; ln.kind = kind;
            if (first_line) memcpy(ln.marker, marker, sizeof ln.marker);
            lay_emit(&t, &ln);
            first_line = 0;
            i = last_fit_next;
        }
        t.y += met.space_after;
    }

    /* Close the final page. */
    if (l->line_len > t.page_first || l->page_len == 0) {
        if (l->page_len < l->page_cap) {
            l->pages[l->page_len].first_line = t.page_first;
            l->pages[l->page_len].line_count = l->line_len - t.page_first;
        }
        l->page_len++;
    }
    return t.overflow ? AO_E_FULL : AO_OK;
}

int ao_layout_page_of(const ao_layout_t *l, const ao_model_t *m, uint32_t pos) {
    if (!l || !m) return -1;
    uint32_t pi = ao_model_para_of(m, pos);
    for (uint32_t pg = 0; pg < l->page_len && pg < l->page_cap; pg++) {
        for (uint32_t i = 0; i < l->pages[pg].line_count; i++) {
            if (l->lines[l->pages[pg].first_line + i].para == pi)
                return (int)pg;
        }
    }
    return -1;
}

void ao_layout_header(char *out, size_t cap, const char *title) {
    if (!cap) return;
    {
        size_t i = 0;
        const char *s = title ? title : "";
        while (s[i] && i + 1 < cap) { out[i] = s[i]; i++; }
        out[i] = 0;
    }
}

void ao_layout_footer(char *out, size_t cap, uint32_t page, uint32_t total) {
    if (!cap) return;
    /* "Page N of M" by hand. */
    char tmp[48];
    int ti = 0;
    const char *pre = "Page ";
    while (*pre && ti < (int)sizeof tmp - 16) tmp[ti++] = *pre++;
    char rev[12]; int ri = 0; uint32_t v = page + 1;
    if (!v) rev[ri++] = '0';
    while (v) { rev[ri++] = (char)('0' + (v % 10)); v /= 10; }
    while (ri > 0) tmp[ti++] = rev[--ri];
    const char *mid = " of ";
    while (*mid && ti < (int)sizeof tmp - 12) tmp[ti++] = *mid++;
    ri = 0; v = total;
    if (!v) rev[ri++] = '0';
    while (v) { rev[ri++] = (char)('0' + (v % 10)); v /= 10; }
    while (ri > 0) tmp[ti++] = rev[--ri];
    tmp[ti] = 0;
    size_t i = 0;
    while (tmp[i] && i + 1 < cap) { out[i] = tmp[i]; i++; }
    out[i] = 0;
}

/* ------------------------------------------------------------ images ---- */

int ao_png_size(const uint8_t *data, size_t n, uint32_t *w, uint32_t *h) {
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (!data || n < 24 || !w || !h) return -1;
    if (memcmp(data, sig, 8) != 0) return -1;
    if (memcmp(data + 12, "IHDR", 4) != 0) return -1;
    *w = ((uint32_t)data[16] << 24) | ((uint32_t)data[17] << 16) |
         ((uint32_t)data[18] << 8) | (uint32_t)data[19];
    *h = ((uint32_t)data[20] << 24) | ((uint32_t)data[21] << 16) |
         ((uint32_t)data[22] << 8) | (uint32_t)data[23];
    return (*w && *h) ? 0 : -1;
}

void ao_image_fit(const ao_page_geom_t *g, uint32_t iw, uint32_t ih,
                  uint32_t *dw, uint32_t *dh) {
    uint32_t cw = ao_page_content_w(g), ch = ao_page_content_h(g);
    if (!iw || !ih) { *dw = cw; *dh = 120; return; }
    uint32_t w = iw, h = ih;
    if (w > cw) { h = (uint32_t)((uint64_t)h * cw / w); w = cw; }
    if (h > ch) { w = (uint32_t)((uint64_t)w * ch / h); h = ch; }
    if (!h) h = 1;
    *dw = w; *dh = h;
}

/* ------------------------------------------------------------ tables ---- */

uint32_t ao_table_parse_row(const char *text, uint32_t len,
                            ao_tcell_t *cells, uint32_t cap, int header) {
    uint32_t n = 0, i = 0;
    for (;;) {
        uint32_t start = i;
        while (i < len && text[i] != '\t') i++;
        if (n < cap) {
            cells[n].text = text + start;
            cells[n].len = i - start;
            cells[n].colspan = 1;
            cells[n].shade = (uint8_t)(header != 0);
            /* A leading merge marker makes this cell a continuation of
             * the previous one: fold it into that cell's span. */
            if (n > 0 && cells[n].len && text[start] == AO_TABLE_MERGE_MARK) {
                cells[n - 1].colspan++;
                /* do not count this slot as its own cell */
            } else {
                n++;
            }
        }
        if (i >= len) break;
        i++;   /* the tab */
    }
    return n;
}

int ao_table_layout(const ao_tcell_t *cells, uint32_t nrows, uint32_t ncols,
                    uint32_t width, uint32_t row_h,
                    ao_trect_t *out, uint32_t cap, uint32_t *total_h) {
    if (!cells || !out || !nrows || !ncols) return AO_E_ARG;
    uint32_t base = width / ncols;
    uint32_t n = 0;
    for (uint32_t r = 0; r < nrows; r++) {
        uint32_t col = 0;
        for (uint32_t c = 0; c < ncols && col < ncols; c++) {
            const ao_tcell_t *cell = &cells[r * ncols + c];
            if (n >= cap) return AO_E_FULL;
            uint32_t span = cell->colspan ? cell->colspan : 1;
            if (col + span > ncols) span = ncols - col;
            uint32_t x = col * base;
            uint32_t w = span * base;
            if (col + span == ncols) w = width - x;   /* last takes rest */
            out[n].x = (int32_t)x;
            out[n].y = (int32_t)(r * row_h);
            out[n].w = w;
            out[n].h = row_h;
            out[n].row = r;
            out[n].col = col;
            n++;
            col += span;
            /* merged continuation slots are already folded into span */
            c += span - 1;
        }
    }
    if (total_h) *total_h = nrows * row_h;
    return (int)n;
}

int ao_table_row_insert_col(char *text, uint32_t len, uint32_t cap,
                            uint32_t col) {
    /* Find the byte offset where column `col` starts (its preceding tab,
     * or end of text when appending a column). */
    uint32_t i = 0, c = 0;
    while (c < col && i < len) {
        if (text[i] == '\t') c++;
        i++;
    }
    /* Insert "\t" (+ nothing) at the column start: for col 0 before the
     * first cell, else right after the tab that opens the column... the
     * empty cell is then [i, i). */
    uint32_t at = (col == 0) ? 0 : i;
    if (len + 1 >= cap) return AO_E_FULL;
    memmove(text + at + 1, text + at, len - at);
    text[at] = '\t';
    if (col == 0) {
        /* text now starts with a tab: the first cell is the empty one,
         * which is exactly an inserted leading column. */
    }
    text[len + 1] = 0;
    return (int)(len + 1);
}

int ao_table_row_delete_col(char *text, uint32_t len, uint32_t col) {
    uint32_t start = 0, c = 0, i = 0;
    while (c < col && i < len) {
        if (text[i] == '\t') { c++; start = i + 1; }
        i++;
    }
    if (c < col) return AO_E_RANGE;    /* no such column in this row */
    uint32_t end = start;
    while (end < len && text[end] != '\t') end++;
    /* Delete the cell plus one separator: the following tab when there is
     * one, else the preceding tab when deleting the last cell. */
    uint32_t del_from = start, del_to = end;
    if (end < len) del_to = end + 1;
    else if (start > 0) del_from = start - 1;
    uint32_t dn = del_to - del_from;
    memmove(text + del_from, text + del_to, len - del_to);
    text[len - dn] = 0;
    return (int)(len - dn);
}

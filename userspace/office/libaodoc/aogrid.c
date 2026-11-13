/* aogrid.c — OFFICE_PLAN.md OF5: the virtualised grid behind the Sheet.
 *
 * The grid never allocates per cell: viewport maths is pure arithmetic
 * over the scroll position and per-column widths; the contents come from
 * the OF4 engine on demand.  See aogrid.h for the contract.
 */
#define _POSIX_C_SOURCE 200809L
#include "aogrid.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>

#define AO_ROW_H AO_GRID_ROW_H

/* ---------------------------------------------------------------- refs --- */

/* Column letter(s) -> 0-based index; -1 on garbage. */
static int col_from_letters(const char *p, int n) {
    uint32_t c = 0;
    for (int i = 0; i < n; i++) {
        char ch = (char)toupper((unsigned char)p[i]);
        if (ch < 'A' || ch > 'Z') return -1;
        c = c * 26 + (uint32_t)(ch - 'A' + 1);
    }
    return c ? (int)c - 1 : -1;
}

/* Parse "$A$1" style ref at *p into row/col + $ flags.  Returns
   0 = not a ref (caller copies verbatim),
   1 = ok (*eat set to bytes consumed),
   2 = consumed a ref-shaped token whose coordinates are off-grid
       (caller emits #REF! and skips it). */
static int scan_ref(const char *p, uint32_t *row, uint32_t *col,
                    int *abs_r, int *abs_c, size_t *eat) {
    const char *q = p;
    int ar = 0, ac = 0;
    if (*q == '$') { ac = 1; q++; }
    const char *ls = q;
    int ln = 0;
    while (*q && isalpha((unsigned char)*q) && ln < 4) { q++; ln++; }
    if (ln == 0) return 0;
    int c = col_from_letters(ls, ln);
    if (c < 0) {
        /* Letters only, not a column index: a name (or an out-of-range
           column name).  If it is followed by digits it *looks* like a
           reference to a column beyond XFD — Excel calls that #REF!. */
        const char *d = q;
        int dn = 0;
        while (*d && isdigit((unsigned char)*d)) { d++; dn++; }
        return dn ? 0 : 0;   /* plain name: verbatim */
    }
    if (*q == '$') { ar = 1; q++; }
    const char *ds = q;
    int dn = 0;
    while (*q && isdigit((unsigned char)*q)) { q++; dn++; }
    if (dn == 0) return 0;
    *eat = (size_t)(q - p);
    if (c >= (int)AO_SHEET_MAX_COLS) return 2;
    uint32_t r1 = (uint32_t)strtoul(ds, NULL, 10);
    if (r1 == 0 || r1 > AO_SHEET_MAX_ROWS) return 2;
    *row = r1 - 1; *col = (uint32_t)c; *abs_r = ar; *abs_c = ac;
    return 1;
}

/* Column index -> letters, with optional $ markers, into buf. */
static void emit_ref(uint32_t row, uint32_t col, int abs_r, int abs_c,
                     char *buf, size_t n) {
    ao_ref_format(row, col, abs_r, abs_c, buf, n);
}

static void emit_str(char *out, size_t out_n, size_t *olen,
                     const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (*olen + 1 >= out_n) break;
        out[(*olen)++] = s[i];
    }
}

void ao_formula_translate(const char *in, int drow, int dcol,
                          char *out, size_t out_n) {
    size_t i = 0, olen = 0;
    size_t len = strlen(in);
    if (strncmp(in, "=", 1) != 0) {
        /* Not a formula: verbatim copy. */
        snprintf(out, out_n, "%s", in);
        return;
    }
    out[0] = '='; olen = 1;
    i = 1;
    while (i < len) {
        char c = in[i];
        /* string literal: copy verbatim, "" escapes a quote */
        if (c == '"') {
            size_t start = i++;
            while (i < len) {
                if (in[i] == '"') {
                    if (i + 1 < len && in[i + 1] == '"') { i += 2; continue; }
                    i++; break;
                }
                i++;
            }
            emit_str(out, out_n, &olen, in + start, i - start);
            continue;
        }
        if (isalpha((unsigned char)c) || c == '$') {
            /* Token start.  Refs may carry a leading $ on either axis. */
            size_t start = i;
            uint32_t row = 0, col = 0;
            int ar = 0, ac = 0;
            size_t used = 0;
            int st = scan_ref(in + i, &row, &col, &ar, &ac, &used);
            if (st == 1 && in[start + used] == '(') st = 0;   /* function call */
            if (st == 2) {
                emit_str(out, out_n, &olen, "#REF!", 5);
                i = start + used;
                continue;
            }
            if (st == 1) {
                int bad = 0;
                int64_t nr = (int64_t)row + (ar ? 0 : drow);
                int64_t nc = (int64_t)col + (ac ? 0 : dcol);
                if (nr < 0 || nc < 0 ||
                    (uint64_t)nr >= AO_SHEET_MAX_ROWS ||
                    (uint64_t)nc >= AO_SHEET_MAX_COLS) bad = 1;
                if (bad) {
                    emit_str(out, out_n, &olen, "#REF!", 5);
                } else {
                    char tmp[32];
                    emit_ref((uint32_t)nr, (uint32_t)nc, ar, ac, tmp, sizeof tmp);
                    emit_str(out, out_n, &olen, tmp, strlen(tmp));
                }
                i = start + used;
                continue;
            }
            /* Not a ref: copy the letters/$ run verbatim, then go on. */
            while (i < len && (isalpha((unsigned char)in[i]) || in[i] == '$' ||
                               in[i] == '_' || in[i] == '.')) i++;
            while (i < len && isdigit((unsigned char)in[i])) i++;
            emit_str(out, out_n, &olen, in + start, i - start);
            continue;
        }
        out[olen < out_n - 1 ? olen++ : olen] = c;
        i++;
    }
    out[olen < out_n ? olen : out_n - 1] = '\0';
}

/* ------------------------------------------------------------ viewport --- */

void ao_grid_init(ao_grid_t *g, ao_book_t *book, uint32_t sheet) {
    memset(g, 0, sizeof *g);
    g->book = book;
    g->sheet = sheet;
}

void ao_grid_col_w_set(ao_grid_t *g, uint32_t col, uint32_t px) {
    if (col >= AO_GRID_MAX_COLS) return;
    if (px < 24) px = 24;
    if (px > 512) px = 512;
    g->col_w[col] = (uint16_t)px;
}

uint32_t ao_grid_col_w(const ao_grid_t *g, uint32_t col) {
    if (col >= AO_GRID_MAX_COLS) return AO_GRID_DEFAULT_COL_W;
    uint32_t w = g->col_w[col];
    return w ? w : AO_GRID_DEFAULT_COL_W;
}

/* Prefix sum of column widths up to and excluding `col`. */
static uint32_t prefix_w(const ao_grid_t *g, uint32_t col) {
    uint32_t x = 0;
    for (uint32_t i = 0; i < col; i++) x += ao_grid_col_w(g, i);
    return x;
}

uint32_t ao_grid_col_x(const ao_grid_t *g, uint32_t col) { return prefix_w(g, col); }
uint32_t ao_grid_x_of_col(const ao_grid_t *g, uint32_t col) { return prefix_w(g, col + 1); }

static uint32_t freeze_w(const ao_grid_t *g) { return prefix_w(g, g->freeze_cols); }
static uint32_t freeze_h(const ao_grid_t *g) { return g->freeze_rows * AO_ROW_H; }

uint32_t ao_grid_rows_visible(const ao_grid_t *g, uint32_t height_px) {
    uint32_t frozen = freeze_h(g);
    uint32_t avail = height_px > frozen ? height_px - frozen : 0;
    uint32_t v = avail / AO_ROW_H;
    return v ? v : 1;
}

uint32_t ao_grid_cols_visible(const ao_grid_t *g, uint32_t width_px) {
    uint32_t frozen = freeze_w(g);
    uint32_t avail = width_px > frozen ? width_px - frozen : 0;
    uint32_t x = 0, n = 0;
    for (uint32_t c = g->freeze_cols; c < AO_GRID_MAX_COLS; c++) {
        uint32_t w = ao_grid_col_w(g, c);
        if (x + w > avail) break;
        x += w; n++;
    }
    return n ? n : 1;
}

void ao_grid_ensure_visible(ao_grid_t *g, uint32_t row, uint32_t col,
                            uint32_t height_px, uint32_t width_px) {
    if (row >= g->freeze_rows) {
        uint32_t vis = ao_grid_rows_visible(g, height_px);
        uint32_t first = g->freeze_rows + g->top_row;
        uint32_t last = first + vis - 1;
        if (row < first) {
            g->top_row = row - g->freeze_rows;
        } else if (row > last) {
            g->top_row = row - g->freeze_rows - vis + 1;
        }
    }
    if (col >= g->freeze_cols) {
        uint32_t avail = width_px - freeze_w(g);
        /* Walk candidate windows; cheap: at most a few dozen columns. */
        uint32_t scroll_px = prefix_w(g, g->left_col) - freeze_w(g);
        uint32_t x0 = prefix_w(g, col) - freeze_w(g) - scroll_px;
        uint32_t cw = ao_grid_col_w(g, col);
        while (x0 + cw > avail) {
            /* Not enough room: scroll so this col is flush right. */
            g->left_col++;
            if (g->left_col >= AO_GRID_MAX_COLS) { g->left_col = AO_GRID_MAX_COLS - 1; break; }
            scroll_px = prefix_w(g, g->left_col) - freeze_w(g);
            x0 = prefix_w(g, col) - freeze_w(g) - scroll_px;
        }
        while (col < g->left_col) { g->left_col--; scroll_px = prefix_w(g, g->left_col) - freeze_w(g); x0 = prefix_w(g, col) - freeze_w(g) - scroll_px; }
    }
}

void ao_grid_scroll(ao_grid_t *g, int drows, int dcols) {
    if (drows) {
        int64_t t = (int64_t)g->top_row + drows;
        if (t < 0) t = 0;
        if ((uint64_t)t >= AO_GRID_MAX_ROWS) t = AO_GRID_MAX_ROWS - 1;
        g->top_row = (uint32_t)t;
    }
    if (dcols) {
        int64_t t = (int64_t)g->left_col + dcols;
        if (t < 0) t = 0;
        if ((uint64_t)t >= AO_GRID_MAX_COLS) t = AO_GRID_MAX_COLS - 1;
        g->left_col = (uint32_t)t;
    }
}

/* ------------------------------------------------------------ selection --- */

void ao_grid_move(ao_grid_t *g, int drow, int dcol, int extend) {
    int64_t tr = (int64_t)g->cur_row + drow;
    int64_t tc = (int64_t)g->cur_col + dcol;
    if (tr < 0) tr = 0;
    if (tc < 0) tc = 0;
    if ((uint64_t)tr >= AO_GRID_MAX_ROWS) tr = AO_GRID_MAX_ROWS - 1;
    if ((uint64_t)tc >= AO_GRID_MAX_COLS) tc = AO_GRID_MAX_COLS - 1;
    uint32_t nr = (uint32_t)tr, nc = (uint32_t)tc;
    if (!extend) {
        g->anchor_row = nr;
        g->anchor_col = nc;
    }
    g->cur_row = nr;
    g->cur_col = nc;
    g->extend = extend;
}

static uint32_t used_max_row(ao_book_t *b, ao_sheet_t *s) {
    uint32_t n = ao_sheet_cell_count(s), m = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r, c;
        ao_cell_pos(ao_sheet_cell_sorted(b, s, i), &r, &c);
        if (r > m) m = r;
    }
    return m;
}

static uint32_t used_max_col(ao_book_t *b, ao_sheet_t *s) {
    uint32_t n = ao_sheet_cell_count(s), m = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r, c;
        ao_cell_pos(ao_sheet_cell_sorted(b, s, i), &r, &c);
        if (c > m) m = c;
    }
    return m;
}

void ao_grid_jump(ao_grid_t *g, int dr, int dc, int extend) {
    ao_sheet_t *s = ao_book_sheet_at(g->book, (int)g->sheet);
    uint32_t nr = g->cur_row, nc = g->cur_col;
    if (s) {
        uint32_t n = ao_sheet_cell_count(s);
        if (dr) {
            uint32_t best = dr < 0 ? 0 : AO_GRID_MAX_ROWS - 1;
            int found = 0;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t r, c;
                ao_cell_pos(ao_sheet_cell_sorted(g->book, s, i), &r, &c);
                if (dr < 0) {
                    if (r < g->cur_row && r >= best) { best = r; found = 1; }
                } else {
                    if (r > g->cur_row && r <= best) { best = r; found = 1; }
                }
            }
            if (found) nr = best;
            else nr = dr < 0 ? 0 : used_max_row(g->book, s);
        }
        if (dc) {
            uint32_t best = dc < 0 ? 0 : AO_GRID_MAX_COLS - 1;
            int found = 0;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t r, c;
                ao_cell_pos(ao_sheet_cell_sorted(g->book, s, i), &r, &c);
                if (dc < 0) {
                    if (c < g->cur_col && c >= best) { best = c; found = 1; }
                } else {
                    if (c > g->cur_col && c <= best) { best = c; found = 1; }
                }
            }
            if (found) nc = best;
            else nc = dc < 0 ? 0 : used_max_col(g->book, s);
        }
    }
    if (!extend) { g->anchor_row = nr; g->anchor_col = nc; }
    g->cur_row = nr; g->cur_col = nc; g->extend = extend;
}

void ao_grid_to_edge(ao_grid_t *g, int dr, int dc, int extend) {
    ao_sheet_t *s = ao_book_sheet_at(g->book, (int)g->sheet);
    uint32_t nr = (dr < 0) ? 0 : (s ? used_max_row(g->book, s) : 0);
    uint32_t nc = (dc < 0) ? 0 : (s ? used_max_col(g->book, s) : 0);
    if (!extend) { g->anchor_row = nr; g->anchor_col = nc; }
    g->cur_row = nr;
    g->cur_col = nc;
    g->extend = extend;
}

void ao_grid_select_all(ao_grid_t *g) {
    ao_sheet_t *s = ao_book_sheet_at(g->book, (int)g->sheet);
    g->anchor_row = 0;
    g->anchor_col = 0;
    g->cur_row = s ? used_max_row(g->book, s) : 0;
    g->cur_col = s ? used_max_col(g->book, s) : 0;
    g->extend = 1;
}

void ao_grid_sel(const ao_grid_t *g,
                 uint32_t *r1, uint32_t *c1, uint32_t *r2, uint32_t *c2) {
    uint32_t ar = g->anchor_row, ac = g->anchor_col;
    if (ar <= g->cur_row) { *r1 = ar; *r2 = g->cur_row; } else { *r1 = g->cur_row; *r2 = ar; }
    if (ac <= g->cur_col) { *c1 = ac; *c2 = g->cur_col; } else { *c1 = g->cur_col; *c2 = ac; }
}

uint32_t ao_grid_sel_rows(const ao_grid_t *g) {
    uint32_t r1, c1, r2, c2; ao_grid_sel(g, &r1, &c1, &r2, &c2); return r2 - r1 + 1;
}
uint32_t ao_grid_sel_cols(const ao_grid_t *g) {
    uint32_t r1, c1, r2, c2; ao_grid_sel(g, &r1, &c1, &r2, &c2); return c2 - c1 + 1;
}

int ao_grid_in_sel(const ao_grid_t *g, uint32_t row, uint32_t col) {
    uint32_t r1, c1, r2, c2;
    ao_grid_sel(g, &r1, &c1, &r2, &c2);
    return row >= r1 && row <= r2 && col >= c1 && col <= c2;
}

/* -------------------------------------------------------- copy/paste --- */

/* The user-visible text of a cell: the input, not the computed value —
   copying a formula must copy the formula. */
static void cell_text(ao_book_t *b, ao_sheet_t *s, uint32_t row, uint32_t col,
                      char *buf, size_t n) {
    ao_cell_t *c = ao_sheet_cell(b, s, row, col);
    if (c) snprintf(buf, n, "%s", ao_cell_input(c));
    else buf[0] = 0;
}

void ao_grid_copy_tsv(ao_book_t *b, ao_sheet_t *s,
                      uint32_t r1, uint32_t c1, uint32_t r2, uint32_t c2,
                      ao_grid_emit_fn emit, void *ctx) {
    char buf[2048];
    for (uint32_t r = r1; r <= r2; r++) {
        for (uint32_t c = c1; c <= c2; c++) {
            cell_text(b, s, r, c, buf, sizeof buf);
            emit(ctx, buf, strlen(buf));
            emit(ctx, "\t", 1);
        }
        emit(ctx, "\n", 1);
    }
}

uint32_t ao_grid_paste_tsv(ao_book_t *b, ao_sheet_t *s,
                           const char *tsv, uint32_t row, uint32_t col,
                           int drow, int dcol) {
    /* Empty fields and empty rows round-trip: copy() emits a tab after
       every cell and a newline after every row, so the paste has to
       honour position, not just content. */
    uint32_t written = 0, r = row;
    const char *p = tsv;
    while (*p) {
        const char *nl = strchr(p, '\n');
        const char *line = p;
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (ll && line[ll - 1] == '\r') ll--;
        const char *fend = line + ll;
        const char *f = line;
        uint32_t c = col;
        for (;;) {
            const char *tab = memchr(f, '\t', (size_t)(fend - f));
            size_t flen = tab ? (size_t)(tab - f) : (size_t)(fend - f);
            char field[2048];
            size_t cp = flen < sizeof field - 1 ? flen : sizeof field - 1;
            memcpy(field, f, cp);
            field[cp] = 0;
            if (field[0]) {
                char buf[2048];
                if (field[0] == '=' && (drow || dcol)) {
                    ao_formula_translate(field, drow, dcol, buf, sizeof buf);
                } else {
                    snprintf(buf, sizeof buf, "%s", field);
                }
                ao_sheet_set(b, s, r, c, buf);
                written++;
            }
            if (!tab) break;
            f = tab + 1;
            c++;
        }
        r++;
        if (!nl) break;
        p = nl + 1;
    }
    return written;
}

/* ------------------------------------------------------------- fill --- */

static int is_numstr(const char *s) {
    if (!s || !*s) return 0;
    char *end = NULL;
    double v = strtod(s, &end);
    (void)v;
    return end && *end == 0;
}

void ao_grid_fill(ao_book_t *b, ao_sheet_t *s,
                  uint32_t r1, uint32_t c1, uint32_t r2, uint32_t c2,
                  uint32_t tr2, uint32_t tc2) {
    if (tr2 <= r2 && tc2 <= c2) return;   /* fill only extends, never shrinks */
    uint32_t srclen = r2 - r1 + 1, clen = c2 - c1 + 1;

    if (tr2 > r2) {
        /* Down: series or translated formulas, column by column. */
        for (uint32_t c = c1; c <= c2; c++) {
            char a[2048], b2[2048];
            /* Start from the LAST source row; try the two before it for a
               numeric step. */
            cell_text(b, s, r2, c, b2, sizeof b2);
            cell_text(b, s, r2 > r1 ? r2 - 1 : r1, c, a, sizeof a);
            double v1 = 0, v2 = 0;
            int num = is_numstr(a) && is_numstr(b2) && r2 > r1;
            if (num) { v1 = strtod(a, NULL); v2 = strtod(b2, NULL); }
            double v = v2;
            for (uint32_t t = r2 + 1; t <= tr2; t++) {
                uint32_t offset = t - (r2 + 1);
                uint32_t src = r1 + (offset % srclen);
                char src_in[2048], out[2048];
                cell_text(b, s, src, c, src_in, sizeof src_in);
                if (num) {
                    v += v2 - v1;
                    snprintf(out, sizeof out, "%.17g", v);
                } else if (src_in[0] == '=') {
                    int64_t dr = (int64_t)t - (int64_t)src;
                    ao_formula_translate(src_in, (int)dr, 0, out, sizeof out);
                } else {
                    snprintf(out, sizeof out, "%s", src_in);
                }
                ao_sheet_set(b, s, t, c, out);
            }
        }
        return;
    }

    if (tc2 > c2) {
        /* Right: same, row by row. */
        for (uint32_t r = r1; r <= r2; r++) {
            char a[2048], b2[2048];
            cell_text(b, s, r, c2, b2, sizeof b2);
            cell_text(b, s, r, c2 > c1 ? c2 - 1 : c1, a, sizeof a);
            double v1 = 0, v2 = 0;
            int num = is_numstr(a) && is_numstr(b2) && c2 > c1;
            if (num) { v1 = strtod(a, NULL); v2 = strtod(b2, NULL); }
            double v = v2;
            for (uint32_t t = c2 + 1; t <= tc2; t++) {
                uint32_t offset = t - (c2 + 1);
                uint32_t src = c1 + (offset % clen);
                char src_in[2048], out[2048];
                cell_text(b, s, r, src, src_in, sizeof src_in);
                if (num) {
                    v += v2 - v1;
                    snprintf(out, sizeof out, "%.17g", v);
                } else if (src_in[0] == '=') {
                    int64_t dc = (int64_t)t - (int64_t)src;
                    ao_formula_translate(src_in, 0, (int)dc, out, sizeof out);
                } else {
                    snprintf(out, sizeof out, "%s", src_in);
                }
                ao_sheet_set(b, s, r, t, out);
            }
        }
    }
}

/* ---------------------------------------------------------- sort/filter --- */

static double cell_num(ao_book_t *b, ao_sheet_t *s, uint32_t row, uint32_t col,
                       int *is_num, char *text, size_t text_n, int *is_blank) {
    ao_cell_t *c = ao_sheet_cell(b, s, row, col);
    *is_num = 0;
    *is_blank = c ? 0 : 1;
    if (text) text[0] = 0;
    if (!c) return 0;
    ao_value_t v = ao_sheet_get(b, s, row, col);
    double n = 0;
    if (ao_val_to_num(v, &n) == 0) { *is_num = 1; return n; }
    if (text) ao_val_to_text(b, v, text, text_n);
    return 0;
}

typedef struct { uint32_t idx; } sort_ent_t;

static ao_book_t *g_sort_book;
static ao_sheet_t *g_sort_sheet;
static uint32_t g_sort_by;
static int g_sort_desc;

static int cmp_rows(const void *pa, const void *pb) {
    uint32_t a = ((const sort_ent_t *)pa)->idx;
    uint32_t c = ((const sort_ent_t *)pb)->idx;
    int anum = 0, cnum = 0, ablank = 0, cblank = 0;
    char atext[256] = {0}, ctext[256] = {0};
    double an = cell_num(g_sort_book, g_sort_sheet, a, g_sort_by, &anum, atext, sizeof atext, &ablank);
    double cn = cell_num(g_sort_book, g_sort_sheet, c, g_sort_by, &cnum, ctext, sizeof ctext, &cblank);
    int cmp;
    if (ablank && cblank) cmp = 0;
    else if (ablank) cmp = 1;          /* blanks last */
    else if (cblank) cmp = -1;
    else if (anum && cnum) cmp = (an > cn) - (an < cn);
    else if (anum) cmp = -1;           /* numbers before text */
    else if (cnum) cmp = 1;
    else cmp = strcmp(atext, ctext);   /* case-sensitive: deterministic */
    if (g_sort_desc) cmp = -cmp;
    if (cmp == 0) cmp = (a > c) - (a < c);   /* stable */
    return cmp;
}

void ao_grid_sort(ao_book_t *b, ao_sheet_t *s,
                  uint32_t r1, uint32_t c1, uint32_t r2, uint32_t c2,
                  uint32_t by, int desc) {
    uint32_t nrows = r2 - r1 + 1, ncols = c2 - c1 + 1;
    if (nrows < 2) return;
    /* Snapshot the whole range, then rewrite it in the new order. */
    char **snap = malloc(nrows * ncols * sizeof(char *));
    if (!snap) return;
    for (uint32_t r = 0; r < nrows; r++)
        for (uint32_t c = 0; c < ncols; c++) {
            ao_cell_t *cell = ao_sheet_cell(b, s, r1 + r, c1 + c);
            snap[r * ncols + c] = cell ? strdup(ao_cell_input(cell)) : NULL;
        }
    sort_ent_t *ent = malloc(nrows * sizeof(sort_ent_t));
    if (!ent) {
        for (uint32_t i = 0; i < nrows * ncols; i++) free(snap[i]);
        free(snap);
        return;
    }
    for (uint32_t r = 0; r < nrows; r++) ent[r].idx = r1 + r;
    g_sort_book = b; g_sort_sheet = s; g_sort_by = by; g_sort_desc = desc;
    qsort(ent, nrows, sizeof(sort_ent_t), cmp_rows);
    for (uint32_t r = 0; r < nrows; r++) {
        for (uint32_t c = 0; c < ncols; c++) {
            char *v = snap[(ent[r].idx - r1) * ncols + c];
            ao_sheet_set(b, s, r1 + r, c1 + c, v ? v : "");
        }
    }
    for (uint32_t i = 0; i < nrows * ncols; i++) free(snap[i]);
    free(snap);
    free(ent);
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

int ao_grid_row_hidden(const ao_grid_t *g, uint32_t row) {
    return g->hidden_n && bsearch(&row, g->hidden, g->hidden_n,
                                  sizeof(uint32_t), cmp_u32) != NULL;
}

void ao_grid_filter_clear(ao_grid_t *g) {
    free(g->hidden);
    g->hidden = NULL;
    g->hidden_n = 0;
    g->filter_on = 0;
}

/* Glob match: * matches any run of characters, everything else is
   literal.  Case-insensitive, matching Excel's filter behaviour. */
static int glob_match(const char *glob, const char *text) {
    while (*glob) {
        if (*glob == '*') {
            while (*glob == '*') glob++;
            if (!*glob) return 1;
            for (const char *t = text; *t; t++)
                if (glob_match(glob, t)) return 1;
            return 0;
        }
        if (tolower((unsigned char)*glob) != tolower((unsigned char)*text)) return 0;
        glob++;
        text++;
    }
    return *text == 0;
}

uint32_t ao_grid_filter(ao_grid_t *g, ao_sheet_t *s,
                        uint32_t r1, uint32_t c1, uint32_t r2, uint32_t c2,
                        uint32_t col, const char *criterion) {
    (void)c1; (void)c2;   /* the criterion reads one column of the range */
    ao_grid_filter_clear(g);
    const char *p = criterion;
    while (*p == ' ') p++;
    /* Numeric criteria: op followed by a number. */
    const char *ops[] = {">=", "<=", "<>", ">", "<", "=", NULL};
    int op = -1;
    const char *q = NULL;
    for (int i = 0; ops[i]; i++) {
        size_t n = strlen(ops[i]);
        if (strncmp(p, ops[i], n) == 0) { op = i; q = p + n; break; }
    }
    char *end = NULL;
    double num = 0;
    int num_ok = 0;
    if (op >= 0) {
        while (*q == ' ') q++;
        num = strtod(q, &end);
        num_ok = end != q;
    }
    ao_book_t *b = g->book;
    uint32_t hidden_cap = 0, kept = 0, hn = 0;
    uint32_t *hide = NULL;
    for (uint32_t r = r1; r <= r2; r++) {
        ao_value_t v = ao_sheet_get(b, s, r, col);
        double n = 0;
        int is_num = ao_val_to_num(v, &n) == 0;
        char tbuf[512];
        tbuf[0] = 0;
        if (!is_num) ao_val_to_text(b, v, tbuf, sizeof tbuf);

        int keep;
        if (num_ok) {
            if (!is_num) { keep = 0; }
            else switch (op) {
                case 0: keep = n >= num; break;
                case 1: keep = n <= num; break;
                case 2: keep = n != num; break;
                case 3: keep = n > num;  break;
                case 4: keep = n < num;  break;
                default: keep = n == num; break;
            }
        } else {
            /* text/glob criterion (a leading '*' or any '*' means glob) */
            const char *t = p;
            if (is_num) {
                snprintf(tbuf, sizeof tbuf, "%.17g", n);
            }
            if (strchr(t, '*')) keep = glob_match(t, tbuf);
            else keep = strcasecmp(t, tbuf) == 0;
        }
        if (keep) { kept++; continue; }
        if (hn >= hidden_cap) {
            uint32_t ncap = hidden_cap ? hidden_cap * 2 : 32;
            uint32_t *nh = realloc(hide, ncap * sizeof(uint32_t));
            if (!nh) { free(hide); g->hidden_n = 0; g->filter_on = 0; return kept; }
            hide = nh;
            hidden_cap = ncap;
        }
        hide[hn++] = r;
    }
    g->hidden = hide;
    g->hidden_n = hn;
    g->filter_on = 1;
    return kept;
}

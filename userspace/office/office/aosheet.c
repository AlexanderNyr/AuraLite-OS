/* userspace/office/office/aosheet.c — the Sheet engine, headless.
 *
 * OFFICE_PLAN OF4 gates the engine without a single pixel: this program
 * loads a workbook, recalculates it, and prints the computed values as
 * receipts.  The grid arrives in OF5; nothing here knows it exists.
 *
 *   aosheet --eval <file.aosheet>   load, recalc, print Sheet!A1 = value
 *   aosheet --csv  <file.aosheet>   load, recalc, print sheet 1 as CSV
 *   aosheet --demo                  evaluate the built-in demo workbook
 *
 * The arena is a static 16 MiB: large enough for the practical cell cap
 * on several sheets, and its counters are printed so the gate can see
 * allocation rather than trust it.
 */

#define _POSIX_C_SOURCE 200809L

#include "aocalc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static ao_arena_t arena;
static uint8_t arena_mem[16 * 1024 * 1024];
static char file_buf[4 * 1024 * 1024];

static void emit_file(void *ctx, const char *data, size_t n) {
    fwrite(data, 1, n, (FILE *)ctx);
}

static int print_values(ao_book_t *b) {
    int errors = 0;
    for (int si = 0; si < ao_book_sheet_count(b); si++) {
        ao_sheet_t *s = ao_book_sheet_at(b, si);
        for (uint32_t i = 0; i < ao_sheet_cell_count(s); i++) {
            ao_cell_t *c = ao_sheet_cell_sorted(b, s, i);
            uint32_t r, col;
            ao_cell_pos(c, &r, &col);
            char ref[24], vbuf[1024];
            ao_ref_name(r, col, ref, sizeof ref);
            ao_value_t v = ao_sheet_get(b, s, r, col);
            ao_val_to_text(b, v, vbuf, sizeof vbuf);
            if (v.kind == AO_VAL_ERR) errors++;
            printf("%s!%s = %s\n", ao_sheet_name(s), ref, vbuf);
        }
    }
    return errors;
}

static int finish(ao_book_t *b, const char *what) {
    int rc = ao_book_recalc(b);
    int errors = print_values(b);
    printf("[sheet] %s: %d sheet(s), recalc %ld step(s), arena %llu/%llu bytes\n",
           what, ao_book_sheet_count(b), ao_book_last_steps(b),
           (unsigned long long)ao_arena_used(&arena),
           (unsigned long long)ao_arena_available(&arena));
    if (rc == AO_CALC_ERR_BUDGET) {
        printf("[sheet] recalculation budget exhausted: graph refused, "
               "unsettled cells read #NUM!\n");
        printf("SHEET EVAL: FAIL (budget)\n");
        return 1;
    }
    printf("SHEET EVAL: PASS (%d cell error(s), all computed)\n", errors);
    return 0;
}

static int load_text(const char *path, size_t *out_n) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(file_buf, 1, sizeof file_buf - 1, f);
    fclose(f);
    file_buf[n] = 0;
    *out_n = n;
    return 0;
}

/* The demo workbook: the same shapes the unit tests grade — a chain, a
 * table with a VLOOKUP, a string built by concatenation, and a cycle
 * that must be a message (#CYCLE!), not a hang. */
static const char DEMO[] =
    "# aosheet demo workbook\n"
    "[Data]\n"
    "A1 = 10\n"
    "A2 = 20\n"
    "A3 = =SUM(A1:A2)\n"
    "A4 = =A3*2+5%\n"
    "B1 = \"wid\"\n"
    "B2 = =B1&\"gets: \"&A3\n"
    "C1 = =IF(A3>25,\"big\",\"small\")\n"
    "[Cycle]\n"
    "A1 = =A1+1\n"
    "A2 = =A1*2\n";

static int bench_requested;

int sheet_gui(ao_book_t *b, const char *path);

int main(int argc, char **argv) {
    ao_arena_init(&arena, arena_mem, sizeof arena_mem);
    ao_book_t *b = ao_book_new(&arena);
    if (!b) {
        printf("SHEET EVAL: FAIL (arena)\n");
        return 1;
    }
    /* Wall-clock "now" for TODAY()/NOW(), as an Excel serial. */
    ao_book_set_now(b, (double)time(NULL) / 86400.0 + 25569.0);

    if (argc >= 2 && strcmp(argv[1], "--bench") == 0) {
        bench_requested = 1;
        return sheet_gui(b, NULL);
    }
    if (argc >= 2 && strcmp(argv[1], "--demo") == 0) {
        if (ao_book_load(b, DEMO, strlen(DEMO)) != AO_CALC_OK) {
            printf("SHEET EVAL: FAIL (demo load)\n");
            return 1;
        }
        return finish(b, "demo");
    }
    if (argc >= 3 && strcmp(argv[1], "--eval") == 0) {
        size_t n;
        if (load_text(argv[2], &n) != 0) {
            printf("SHEET EVAL: FAIL (cannot read %s)\n", argv[2]);
            return 1;
        }
        int rc = ao_book_load(b, file_buf, n);
        if (rc != AO_CALC_OK) {
            printf("SHEET EVAL: FAIL (load rc=%d)\n", rc);
            return 1;
        }
        return finish(b, argv[2]);
    }
    if (argc >= 3 && strcmp(argv[1], "--csv") == 0) {
        size_t n;
        if (load_text(argv[2], &n) != 0 ||
            ao_book_load(b, file_buf, n) != AO_CALC_OK) {
            printf("SHEET EVAL: FAIL (cannot load %s)\n", argv[2]);
            return 1;
        }
        if (ao_book_recalc(b) != AO_CALC_OK) {
            printf("SHEET EVAL: FAIL (budget)\n");
            return 1;
        }
        ao_csv_export(b, ao_book_sheet_at(b, 0), ',', emit_file, stdout);
        printf("SHEET EVAL: PASS (csv)\n");
        return 0;
    }
    /* No flag (or a plain file path): the grid. */
    if (argc >= 2 && argv[1][0] != '-') return sheet_gui(b, argv[1]);
    if (argc >= 2) {
        printf("usage: aosheet [--eval <file> | --csv <file> | --demo | --bench] [file.aosheet]\n");
        return 1;
    }
    return sheet_gui(b, NULL);
}

/* ====================================================================
 * OF5: the grid.
 *
 * A virtualised grid over the OF4 engine.  Only the visible rows and
 * columns are drawn, however deep the workbook goes.  Every content
 * mutation is one undo snapshot — the serialised book with the charts
 * prepended as #chart lines, which is also the .aosheet file format,
 * so undo covers cells, formulas and charts by construction.
 *
 * Keys: arrows/Tab/Enter move (Shift extends), Ctrl+arrows jump the
 * used range, Ctrl+A select all, F2 edit, Esc cancel/close, Delete
 * clear, Ctrl+C/X/V copy/cut/paste (formulas translate), Ctrl+Z/Y
 * undo/redo, Ctrl+S save, Ctrl+O reload, F6/F7/F8 bar/line/pie chart
 * from the selection, F9 freeze panes, F11/F12 sort ascending/
 * descending, Ctrl+Shift+F filter by a typed criterion (empty clears).
 * The mouse selects, drag-selects, drags the fill handle (numeric
 * series + translated formulas), resizes columns from the header
 * edge, and drops a cell reference into a formula you are editing.
 * ================================================================== */

#include "aogrid.h"
#include "auragui.h"
#include <ctype.h>

#define WIN_W 760
#define WIN_H 500
#define FB_H 22              /* formula bar height                */
#define COLHDR_H 16          /* column-letter header              */
#define GX 34                /* row-number gutter width           */
#define GY (FB_H + COLHDR_H) /* grid origin y                     */
#define ROWH AO_GRID_ROW_H
#define SY (WIN_H - 22)      /* status bar origin y               */

#define K_LEFT  0x100
#define K_RIGHT 0x101
#define K_UP    0x102
#define K_DOWN  0x103
#define K_HOME  0x104
#define K_END   0x105
#define K_PGUP  0x106
#define K_PGDN  0x107
#define K_DEL   0x109
#define K_F2    0x111
#define K_F6    0x115
#define K_F7    0x116
#define K_F8    0x117
#define K_F9    0x118
#define K_F11   0x11A
#define K_F12   0x11B
#define M_SHIFT 0x01
#define M_CTRL  0x02

#define DOC_SAVE "/documents/Untitled.aosheet"

typedef struct { int type; uint32_t r1, c1, r2, c2, arow, acol; } chart_t;
/* chart types */
enum { CH_BAR = 0, CH_LINE = 1, CH_PIE = 2 };
static const char *const CH_NAME[] = {"bar", "line", "pie"};
static const uint32_t CH_COLORS[] =
    { 0x00C03020, 0x00308040, 0x002F60C0, 0x00E0C040, 0x007030C0, 0x00808080 };

static ao_book_t *book;
static ao_grid_t grid;
static int wid = -1;
static int bench_left = -1;
static long bench_total, bench_max;
static int dirty;

#define DRAG_NONE 0
#define DRAG_SEL  1
#define DRAG_FILL 2
#define DRAG_RESIZE 3
static int drag_mode;
static uint32_t drag_col;
static uint32_t fill_tr = UINT32_MAX, fill_tc = UINT32_MAX;

static chart_t charts[16];
static int chart_n;

static int editing;
static char edit_buf[2048];
static int edit_len;
static int filter_mode;             /* typing the filter criterion        */
static char filter_buf[128];

static char doc_path[512] = DOC_SAVE;
static long copy_r = -1, copy_c = -1;   /* -1 = foreign clipboard origin */

#define UNDO_CAP 32
static char *undo_stack[UNDO_CAP];
static char *redo_stack[UNDO_CAP];
static int undo_n, redo_n;

static long now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long)ts.tv_sec * 1000000L + (long)(ts.tv_nsec / 1000L);
}

static ao_sheet_t *cur_sheet(void) { return ao_book_sheet_at(grid.book, 0); }

/* ---------- serialising book + charts (undo snapshots are also saves) --- */

typedef struct { char *buf; size_t len, cap; } memsink_t;
static void memsink_emit(void *ctx, const char *data, size_t n) {
    memsink_t *ms = ctx;
    if (ms->len + n + 1 > ms->cap) {
        size_t ncap = (ms->len + n + 1) * 2;
        char *nb = realloc(ms->buf, ncap);
        if (!nb) return;
        ms->buf = nb;
        ms->cap = ncap;
    }
    memcpy(ms->buf + ms->len, data, n);
    ms->len += n;
    ms->buf[ms->len] = 0;
}

static char *serialize_all(void) {
    char head[512];
    memsink_t ms = {0};
    ms.buf = malloc(4096);
    if (!ms.buf) return NULL;
    ms.cap = 4096;
    ms.buf[0] = 0;
    for (int i = 0; i < chart_n; i++) {
        chart_t *c = &charts[i];
        int n = snprintf(head, sizeof head, "#chart %s %u %u %u %u %u %u\n",
                         CH_NAME[c->type], c->r1, c->c1, c->r2, c->c2,
                         c->arow, c->acol);
        if (n > 0) memsink_emit(&ms, head, (size_t)n);
    }
    if (ao_book_save(grid.book, memsink_emit, &ms) != AO_CALC_OK) {
        free(ms.buf);
        return NULL;
    }
    return ms.buf;
}

static void parse_chart_line(const char *line, size_t len) {
    (void)len;
    char type[16];
    unsigned r1, c1, r2, c2, arow, acol;
    /* #chart <bar|line|pie> r1 c1 r2 c2 arow acol */
    if (sscanf(line, "#chart %15s %u %u %u %u %u %u",
               type, &r1, &c1, &r2, &c2, &arow, &acol) != 7) return;
    int t = -1;
    if (!strcmp(type, "bar")) t = CH_BAR;
    else if (!strcmp(type, "line")) t = CH_LINE;
    else if (!strcmp(type, "pie")) t = CH_PIE;
    if (t < 0 || chart_n >= 16) return;
    charts[chart_n].type = t;
    charts[chart_n].r1 = r1; charts[chart_n].c1 = c1;
    charts[chart_n].r2 = r2; charts[chart_n].c2 = c2;
    charts[chart_n].arow = arow; charts[chart_n].acol = acol;
    chart_n++;
}

static void restore_from_text(const char *text, size_t len) {
    ao_arena_init(&arena, arena_mem, sizeof arena_mem);
    grid.book = ao_book_new(&arena);
    book = grid.book;
    ao_book_set_now(book, (double)time(NULL) / 86400.0 + 25569.0);
    chart_n = 0;
    const char *p = text;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (ll >= 7 && strncmp(p, "#chart ", 7) == 0) parse_chart_line(p, ll);
        if (!nl) break;
        p = nl + 1;
    }
    ao_book_load(book, text, len);
    ao_sheet_t *sh = ao_book_sheet(book, "Sheet1", 1);
    (void)sh;
    grid.top_row = 0;
    grid.left_col = 0;
    grid.anchor_row = grid.anchor_col = 0;
    grid.cur_row = grid.cur_col = 0;
    grid.freeze_rows = grid.freeze_cols = 0;
    grid.extend = 0;
    ao_grid_filter_clear(&grid);
}

static void push_undo(void) {
    char *snap = serialize_all();
    if (!snap) return;
    if (undo_n == UNDO_CAP) {           /* drop the oldest */
        free(undo_stack[0]);
        memmove(undo_stack, undo_stack + 1, sizeof(char *) * (UNDO_CAP - 1));
        undo_n = UNDO_CAP - 1;
    }
    undo_stack[undo_n++] = snap;
    for (int i = 0; i < redo_n; i++) free(redo_stack[i]);
    redo_n = 0;
}

static void do_undo(void) {
    if (undo_n == 0) { printf("[sheet] undo: nothing\n"); return; }
    char *cur = serialize_all();
    if (cur && redo_n < UNDO_CAP) redo_stack[redo_n++] = cur;
    char *snap = undo_stack[--undo_n];
    restore_from_text(snap, strlen(snap));
    free(snap);
    dirty = 1;
    printf("[sheet] undo (%d left)\n", undo_n);
}

static void do_redo(void) {
    if (redo_n == 0) { printf("[sheet] redo: nothing\n"); return; }
    char *cur = serialize_all();
    if (cur && undo_n < UNDO_CAP) undo_stack[undo_n++] = cur;
    char *snap = redo_stack[--redo_n];
    restore_from_text(snap, strlen(snap));
    free(snap);
    dirty = 1;
    printf("[sheet] redo (%d left)\n", redo_n);
}

static int do_save(void) {
    char *buf = serialize_all();
    if (!buf) return -1;
    FILE *f = fopen(doc_path, "wb");
    if (!f) {
        printf("[sheet] save failed: cannot open %s\n", doc_path);
        free(buf);
        return -1;
    }
    fputs(buf, f);
    fclose(f);
    free(buf);
    uint32_t n = cur_sheet() ? ao_sheet_cell_count(cur_sheet()) : 0;
    dirty = 0;
    printf("[sheet] saved %s (%u cells, %d chart(s))\n", doc_path, n, chart_n);
    return 0;
}

static void do_load(const char *path) {
    size_t n;
    if (load_text(path, &n) != 0) { printf("[sheet] load failed: %s\n", path); return; }
    restore_from_text(file_buf, n);
    snprintf(doc_path, sizeof doc_path, "%s", path);
    printf("[sheet] loaded %s (%u cells, %d chart(s))\n",
           doc_path, ao_sheet_cell_count(cur_sheet()), chart_n);
}

/* ------------------------------------------------------------ view ----- */

static uint32_t prefix_w_all(uint32_t col) {
    uint32_t x = 0;
    for (uint32_t i = 0; i < col && i < AO_GRID_MAX_COLS; i++) x += ao_grid_col_w(&grid, i);
    return x;
}

/* Pixel x of column `col`'s left edge in the current view. */
static uint32_t col_x_view(uint32_t col) {
    if (col < grid.freeze_cols) return prefix_w_all(col);
    uint32_t scroll = prefix_w_all(grid.left_col) - prefix_w_all(grid.freeze_cols);
    return prefix_w_all(col) - scroll;
}

/* Pixel y of row `row`'s top edge, or UINT32_MAX when not visible. */
static uint32_t row_y_view(uint32_t row) {
    if (row < grid.freeze_rows)
        return ao_grid_row_hidden(&grid, row) ? UINT32_MAX : GY + row * ROWH;
    if (row < grid.freeze_rows + grid.top_row) return UINT32_MAX;
    /* walk the window counting visible slots — the walk is bounded by
       the distance into the window, which is small for a viewport */
    uint32_t slot = 0;
    for (uint32_t r = grid.freeze_rows + grid.top_row; r <= row && slot < (SY - GY) / ROWH; r++) {
        if (ao_grid_row_hidden(&grid, r)) continue;
        if (r == row) return GY + (grid.freeze_rows + slot) * ROWH;
        slot++;
    }
    return UINT32_MAX;
}

/* Visible rows in order, honouring the filter. */
static uint32_t visible_rows(uint32_t out[], uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t r = 0; r < grid.freeze_rows && n < cap; r++)
        if (!ao_grid_row_hidden(&grid, r)) out[n++] = r;
    uint32_t r = grid.freeze_rows + grid.top_row;
    uint32_t end = r + 512;             /* bounded walk: 30 visible + hidden */
    while (r < end && n < cap) {
        if (!ao_grid_row_hidden(&grid, r)) out[n++] = r;
        r++;
    }
    return n;
}

static void receipt_cursor(void) {
    char ref[24], v[256];
    ao_sheet_t *s = cur_sheet();
    ao_ref_name(grid.cur_row, grid.cur_col, ref, sizeof ref);
    ao_cell_t *c = s ? ao_sheet_cell(grid.book, s, grid.cur_row, grid.cur_col) : NULL;
    if (c) {
        ao_value_t val = ao_sheet_get(grid.book, s, grid.cur_row, grid.cur_col);
        ao_val_to_text(grid.book, val, v, sizeof v);
        printf("[sheet] %s = %s\n", ref, v);
    } else {
        printf("[sheet] %s = \n", ref);
    }
}

static uint32_t fw_cached = 0;
static uint32_t glyph_width(void) {
    if (!fw_cached) {
        uint32_t w = 8;
        if (ag_font_metrics(&w, NULL, NULL, NULL, NULL) != 0 || !w) w = 8;
        fw_cached = w;
    }
    return fw_cached;
}

/* Scan an in-progress formula and fill the background behind ref-like
   tokens: the reference-highlight while you type. */
static void draw_editor_with_refs(int32_t x, int32_t y, const char *text) {
    uint32_t fw = glyph_width();
    size_t len = strlen(text);
    ag_fill_rect(wid, x, y, (uint32_t)len * fw + 8, FB_H - 4, AG_WHITE);
    if (text[0] == '=') {
        /* highlight pass */
        size_t i = 1;
        while (i < len) {
            char c = text[i];
            if (isalpha((unsigned char)c) || c == '$') {
                /* crude ref scan: $?[A-Za-z]{1,3}$?[0-9]+ */
                size_t start = i;
                const char *q = text + i;
                if (*q == '$') q++;
                const char *ls = q;
                int ln = 0;
                while (*q && isalpha((unsigned char)*q) && ln < 3) { q++; ln++; }
                if (ln && *q == '$') q++;
                const char *ds = q;
                int dn = 0;
                while (*q && isdigit((unsigned char)*q)) { q++; dn++; }
                if (dn) {
                    size_t e2 = (size_t)(q - text);
                    if (text[e2] == '(') { i = e2; continue; }   /* function */
                    (void)ls; (void)ds;
                    ag_fill_rect(wid, x + (int32_t)start * (int32_t)fw, y,
                                 (uint32_t)(e2 - start) * fw, FB_H - 4, AG_YELLOW);
                    i = e2;
                    continue;
                }
                i = i + 1;
                continue;
            }
            i++;
        }
    }
    ag_draw_text(wid, x + 2, y + 3, text, AG_BLACK);
}

static char current_input[2048];
static void editor_text(char *buf, size_t n) {
    if (editing) { snprintf(buf, n, "%s", edit_buf); return; }
    ao_sheet_t *s = cur_sheet();
    ao_cell_t *c = s ? ao_sheet_cell(grid.book, s, grid.cur_row, grid.cur_col) : NULL;
    snprintf(buf, n, "%s", c ? ao_cell_input(c) : "");
}

static void draw_formula_bar(void) {
    char ref[24];
    ao_ref_name(grid.cur_row, grid.cur_col, ref, sizeof ref);
    editor_text(current_input, sizeof current_input);
    ag_fill_rect(wid, 0, 0, WIN_W, FB_H, AG_PANEL);
    ag_fill_rect(wid, 2, 2, 64, FB_H - 4, AG_WHITE);
    ag_draw_rect(wid, 2, 2, 64, FB_H - 4, AG_DARK);
    ag_draw_text(wid, 8, 5, ref, AG_DARK);
    draw_editor_with_refs(70, 2, current_input);
    if (editing) {
        int32_t cx = 70 + 2 + (int32_t)strlen(edit_buf) * (int32_t)glyph_width();
        ag_draw_line(wid, cx, 4, cx, FB_H - 6, AG_ACCENT);
    }
}

static void draw_grid(void) {
    ao_sheet_t *s = cur_sheet();

    /* gutter + header background */
    ag_fill_rect(wid, 0, FB_H, GX, SY - FB_H, AG_PANEL);
    ag_fill_rect(wid, GX, FB_H, WIN_W - GX, COLHDR_H, AG_PANEL);
    ag_draw_line(wid, 0, GY - 1, WIN_W, GY - 1, AG_DARK);

    /* visible column list: frozen then scrolled */
    uint32_t cols[AO_GRID_MAX_COLS];
    uint32_t ncols = 0;
    uint32_t x = 0;
    for (uint32_t c = 0; c < grid.freeze_cols && ncols < AO_GRID_MAX_COLS; c++) {
        cols[ncols++] = c;
        x += ao_grid_col_w(&grid, c);
    }
    for (uint32_t c = grid.freeze_cols;
         c < AO_GRID_MAX_COLS && x < WIN_W && ncols < AO_GRID_MAX_COLS; c++) {
        if (c < grid.left_col) continue;
        cols[ncols++] = c;
        x += ao_grid_col_w(&grid, c);
    }

    /* column letters */
    for (uint32_t i = 0; i < ncols; i++) {
        uint32_t c = cols[i];
        uint32_t cx = col_x_view(c) + GX;
        uint32_t cw = ao_grid_col_w(&grid, c);
        char cl[8];
        ao_ref_format(0, c, 0, 0, cl, sizeof cl);
        char *last = cl + strlen(cl) - 1;
        while (*last >= '0' && *last <= '9') last--;   /* strip the row digit */
        *(last + 1) = 0;
        ag_fill_rect(wid, cx, FB_H, cw, COLHDR_H, AG_PANEL);
        ag_draw_rect(wid, cx, FB_H, cw, COLHDR_H, AG_GRAY);
        ag_draw_text(wid, cx + cw / 2 - 4, FB_H + 3, cl, AG_DARK);
    }

    /* rows */
    uint32_t out_rows[256];
    uint32_t n = visible_rows(out_rows, sizeof out_rows / sizeof out_rows[0]);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = out_rows[i];
        uint32_t y = row_y_view(r);
        if (y == UINT32_MAX || y >= SY) continue;
        /* row number */
        char rn[12];
        snprintf(rn, sizeof rn, "%u", r + 1);
        ag_fill_rect(wid, 0, y, GX, ROWH, AG_PANEL);
        ag_draw_rect(wid, 0, y, GX, ROWH, AG_GRAY);
        ag_draw_text(wid, 6, y + 3, rn, AG_DARK);

        for (uint32_t j = 0; j < ncols; j++) {
            uint32_t c = cols[j];
            uint32_t cx = col_x_view(c) + GX;
            uint32_t cw = ao_grid_col_w(&grid, c);
            int in_sel = ao_grid_in_sel(&grid, r, c);
            uint32_t bg = in_sel ? AG_ACCENT : AG_WHITE;
            ag_fill_rect(wid, cx, y, cw, ROWH, bg);
            ag_draw_rect(wid, cx, y, cw, ROWH, AG_GRAY);
            ao_value_t v = s ? ao_sheet_get(grid.book, s, r, c) : ao_val_blank();
            char vb[160];
            vb[0] = 0;
            if (v.kind != AO_VAL_BLANK) ao_val_to_text(grid.book, v, vb, sizeof vb);
            if (vb[0]) {
                uint32_t fg = (v.kind == AO_VAL_ERR) ? AG_RED :
                              in_sel ? AG_WHITE : AG_BLACK;
                size_t len = strlen(vb);
                size_t maxc = (cw - 8) / glyph_width();
                if (len > maxc) { vb[maxc] = 0; len = maxc; }
                int32_t tx = (int32_t)cx + 4;
                if (v.kind == AO_VAL_NUM) tx += (int32_t)((cw - 8) - len * glyph_width());
                ag_draw_text(wid, tx, y + 3, vb, fg);
            }
        }
        if (y + ROWH >= SY) break;
    }

    /* active cell outline */
    uint32_t cy = row_y_view(grid.cur_row);
    uint32_t cx = col_x_view(grid.cur_col) + GX;
    uint32_t cw = ao_grid_col_w(&grid, grid.cur_col);
    if (cy != UINT32_MAX && cx < WIN_W) {
        ag_draw_rect(wid, cx, cy, cw, ROWH, AG_DARK);
        ag_draw_rect(wid, cx + 1, cy + 1, cw - 2, ROWH - 2, AG_DARK);
        /* fill handle */
        ag_fill_rect(wid, cx + cw - 5, cy + ROWH - 5, 5, 5, AG_ACCENT);
    }
    /* freeze dividers */
    if (grid.freeze_rows) {
        uint32_t y = GY + grid.freeze_rows * ROWH - 1;
        ag_draw_line(wid, 0, y, WIN_W, y, AG_DARK);
    }
    if (grid.freeze_cols) {
        uint32_t xf = prefix_w_all(grid.freeze_cols) + GX - 1;
        ag_draw_line(wid, xf, FB_H, xf, SY, AG_DARK);
    }
}

static double bsin(double x) {
    /* Bhaskara I, |error| < 0.002 rad across [0, pi]; mirrored after. */
    const double PI = 3.14159265358979323846;
    x = x - (double)((int64_t)(x / (2 * PI))) * 2 * PI;
    if (x < 0) x += 2 * PI;
    if (x > PI) return -bsin(x - PI);
    double num = 16.0 * x * (PI - x);
    double den = 5.0 * PI * PI - 4.0 * x * (PI - x);
    return num / den;
}
static double bcos(double x) { return bsin(x + 1.5707963267948966); }

static void draw_chart(const chart_t *c) {
    uint32_t x = col_x_view(c->acol) + GX;
    uint32_t y = row_y_view(c->arow);
    if (x >= WIN_W || y == UINT32_MAX || y >= SY) return;
    const uint32_t CW = 160, CH = 110;
    ag_fill_rect(wid, (int32_t)x, (int32_t)y, CW, CH, AG_WHITE);
    ag_draw_rect(wid, (int32_t)x, (int32_t)y, CW, CH, AG_DARK);

    ao_sheet_t *s = cur_sheet();
    double vals[64];
    uint32_t n = 0;
    double vmax = 0, vsum = 0;
    for (uint32_t r = c->r1; r <= c->r2 && n < 64; r++) {
        ao_value_t v = s ? ao_sheet_get(grid.book, s, r, c->c1) : ao_val_blank();
        double num = 0;
        if (ao_val_to_num(v, &num) == 0 && num > 0) {
            vals[n] = num;
            if (num > vmax) vmax = num;
            vsum += num;
            n++;
        } else {
            vals[n] = 0;
            n++;
        }
    }
    char ttl[64];
    char tfmt[24];
    ao_ref_name(c->r1, c->c1, tfmt, sizeof tfmt);
    snprintf(ttl, sizeof ttl, "%s %s", CH_NAME[c->type], tfmt);
    ag_draw_text(wid, (int32_t)x + 4, (int32_t)y + 3, ttl, AG_DARK);

    if (n == 0) return;
    if (c->type == CH_BAR) {
        uint32_t step = (CW - 16) / n;
        if (step < 2) step = 2;
        uint32_t bw = step > 5 ? step - 3 : step - 1;
        uint32_t base = y + CH - 12;
        ag_draw_line(wid, (int32_t)x + 8, (int32_t)base,
                     (int32_t)x + CW - 8, (int32_t)base, AG_DARK);
        for (uint32_t i = 0; i < n; i++) {
            double frac = vmax > 0 ? vals[i] / vmax : 0;
            uint32_t bh = (uint32_t)(frac * 60.0);
            ag_fill_rect(wid, (int32_t)(x + 10 + i * step), (int32_t)(base - bh),
                         bw, bh ? bh : 1, CH_COLORS[i % 6]);
        }
        return;
    }
    if (c->type == CH_LINE) {
        uint32_t step = n > 1 ? (CW - 16) / (n - 1) : 1;
        uint32_t py = 0, px = 0;
        for (uint32_t i = 0; i < n; i++) {
            double frac = vmax > 0 ? vals[i] / vmax : 0;
            uint32_t cx = x + 10 + i * step;
            uint32_t cy = (y + CH - 12) - (uint32_t)(frac * 70.0);
            if (i > 0) ag_draw_line(wid, (int32_t)px, (int32_t)py,
                                    (int32_t)cx, (int32_t)cy, CH_COLORS[0]);
            ag_fill_rect(wid, (int32_t)cx - 1, (int32_t)cy - 1, 3, 3, CH_COLORS[0]);
            px = cx;
            py = cy;
        }
        return;
    }
    /* pie: filled wedges, drawn as a line fan from the centre.  No libm
       in the guest, so sin/cos are Bhaskara approximations (max error
       ~0.2 deg — invisible at chart radius). */
    int32_t cx = (int32_t)x + (int32_t)CW / 2, cy = (int32_t)y + (int32_t)CH / 2 + 6;
    int32_t rad = 34;
    double acc = -90.0;
    for (uint32_t i = 0; i < n; i++) {
        double frac = vsum > 0 ? vals[i] / vsum : 0;
        double a0 = acc, a1 = acc + frac * 360.0;
        acc = a1;
        for (double a = a0; a < a1; a += 0.8) {
            double rr = a * 0.017453292519943295;
            int32_t ex = cx + (int32_t)((double)rad * bcos(rr));
            int32_t ey = cy + (int32_t)((double)rad * bsin(rr));
            ag_draw_line(wid, cx, cy, ex, ey, CH_COLORS[i % 6]);
        }
    }
}

static void draw_fill_preview(void) {
    if (drag_mode != DRAG_FILL || fill_tr == UINT32_MAX) return;
    uint32_t r1, c1, r2, c2;
    ao_grid_sel(&grid, &r1, &c1, &r2, &c2);
    uint32_t y2 = row_y_view(fill_tr > r2 ? fill_tr : r2);
    uint32_t y1 = row_y_view(r1);
    if (y1 == UINT32_MAX || y2 == UINT32_MAX) return;
    uint32_t last_c = fill_tc > c2 ? fill_tc : c2;
    uint32_t x1 = GX + col_x_view(c1);
    uint32_t xe = GX + col_x_view(last_c);
    uint32_t cw = ao_grid_col_w(&grid, last_c);
    ag_draw_rect(wid, (int32_t)x1, (int32_t)y1, xe + cw - x1, y2 + ROWH - y1,
                 AG_YELLOW);
}

static void draw_charts(void) {
    for (int i = 0; i < chart_n; i++) draw_chart(&charts[i]);
}

static void draw_statusbar(void) {
    char st[384];
    ag_fill_rect(wid, 0, SY, WIN_W, WIN_H - SY, AG_PANEL);
    ag_draw_line(wid, 0, SY, WIN_W, SY, AG_DARK);
    if (filter_mode) {
        snprintf(st, sizeof st, "filter> %s   (Enter applies, Esc cancels; empty clears)",
                 filter_buf);
        ag_draw_text(wid, 8, SY + 6, st, AG_DARK);
        return;
    }
    char ref[24], vb[96], a[24], b2[24], sumb[96];
    ao_sheet_t *s = cur_sheet();
    ao_ref_name(grid.cur_row, grid.cur_col, ref, sizeof ref);
    ao_value_t v = s ? ao_sheet_get(grid.book, s, grid.cur_row, grid.cur_col)
                     : ao_val_blank();
    ao_val_to_text(grid.book, v, vb, sizeof vb);
    uint32_t r1, c1, r2, c2;
    ao_grid_sel(&grid, &r1, &c1, &r2, &c2);
    ao_ref_name(r1, c1, a, sizeof a);
    ao_ref_name(r2, c2, b2, sizeof b2);
    double sum = 0;
    int cnt = 0;
    if ((uint64_t)(r2 - r1 + 1) * (c2 - c1 + 1) <= 4096) {
        for (uint32_t r = r1; r <= r2; r++)
            for (uint32_t c = c1; c <= c2; c++) {
                ao_value_t sv = s ? ao_sheet_get(grid.book, s, r, c)
                                  : ao_val_blank();
                double num = 0;
                if (ao_val_to_num(sv, &num) == 0) { sum += num; cnt++; }
            }
    }
    ao_val_to_text(grid.book, ao_val_num(sum), sumb, sizeof sumb);
    uint32_t cells = s ? ao_sheet_cell_count(s) : 0;
    snprintf(st, sizeof st,
             "%s = %s   sel %s:%s   sum %s n %d   %u cells%s   "
             "F6 bar F7 line F8 pie F9 freeze",
             ref, vb, a, b2, sumb, cnt, cells, dirty ? " *" : "");
    ag_draw_text(wid, 8, SY + 6, st, AG_DARK);
}

static void repaint(void) {
    long t0 = now_us();
    ag_fill_rect(wid, 0, 0, WIN_W, WIN_H, AG_PANEL);
    draw_formula_bar();
    draw_grid();
    draw_fill_preview();
    draw_charts();
    draw_statusbar();
    ag_window_invalidate(wid);
    ag_render_now();
    if (bench_left > 0) {
        long us = now_us() - t0;
        bench_total += us;
        if (us > bench_max) bench_max = us;
    }
}

/* --------------------------------------------------------- editing ----- */

static void start_edit(const char *initial) {
    editing = 1;
    snprintf(edit_buf, sizeof edit_buf, "%s", initial ? initial : "");
    edit_len = (int)strlen(edit_buf);
}

static void cancel_edit(void) {
    editing = 0;
    edit_len = 0;
    edit_buf[0] = 0;
}

/* move: 1 = down, 2 = right */
static void commit_edit(int move) {
    ao_sheet_t *s = cur_sheet();
    if (!editing || !s) return;
    push_undo();
    ao_sheet_set(grid.book, s, grid.cur_row, grid.cur_col, edit_buf);
    dirty = 1;
    editing = 0;
    printf("[sheet] set %s -> ", edit_buf);
    receipt_cursor();
    if (move == 1) ao_grid_move(&grid, 1, 0, 0);
    else if (move == 2) ao_grid_move(&grid, 0, 1, 0);
    receipt_cursor();
}

static void clear_selection(void) {
    ao_sheet_t *s = cur_sheet();
    if (!s) return;
    uint32_t r1, c1, r2, c2;
    ao_grid_sel(&grid, &r1, &c1, &r2, &c2);
    push_undo();
    for (uint32_t r = r1; r <= r2; r++)
        for (uint32_t c = c1; c <= c2; c++)
            ao_sheet_set(grid.book, s, r, c, "");
    dirty = 1;
    printf("[sheet] cleared cells\n");
}

/* reference-mode insertion: a cell ref (or a range when the shift flag is
   set on the click) appended at the cursor of the formula being edited */
static void insert_ref(uint32_t row, uint32_t col) {
    char ref[24];
    ao_ref_name(row, col, ref, sizeof ref);
    size_t rl = strlen(ref);
    if (edit_len + (int)rl >= (int)sizeof(edit_buf) - 1) return;
    memcpy(edit_buf + edit_len, ref, rl + 1);
    edit_len += (int)rl;
}

static void do_copy(int cut) {
    ao_sheet_t *s = cur_sheet();
    if (!s) return;
    uint32_t r1, c1, r2, c2;
    ao_grid_sel(&grid, &r1, &c1, &r2, &c2);
    memsink_t ms = {0};
    ms.buf = malloc(1024);
    if (!ms.buf) return;
    ms.cap = 1024;
    ms.buf[0] = 0;
    ao_grid_copy_tsv(grid.book, s, r1, c1, r2, c2, memsink_emit, &ms);
    ag_set_clipboard(ms.buf);
    copy_r = (long)r1;
    copy_c = (long)c1;
    printf("[sheet] %s %ux%u cells\n", cut ? "cut" : "copied",
           r2 - r1 + 1, c2 - c1 + 1);
    free(ms.buf);
    if (cut) {
        push_undo();
        for (uint32_t r = r1; r <= r2; r++)
            for (uint32_t c = c1; c <= c2; c++)
                ao_sheet_set(grid.book, s, r, c, "");
        dirty = 1;
    }
}

static void do_paste(void) {
    ao_sheet_t *s = cur_sheet();
    if (!s) return;
    char clip[16384];
    if (ag_get_clipboard(clip, sizeof clip) < 0 || !clip[0]) {
        printf("[sheet] paste: clipboard empty\n");
        return;
    }
    push_undo();
    int dr = 0, dc = 0;
    if (copy_r >= 0) {
        dr = (int)grid.cur_row - (int)copy_r;
        dc = (int)grid.cur_col - (int)copy_c;
    }
    uint32_t n = ao_grid_paste_tsv(grid.book, s, clip, grid.cur_row, grid.cur_col, dr, dc);
    dirty = 1;
    printf("[sheet] pasted %u cells\n", n);
}

static void do_sort(int desc) {
    ao_sheet_t *s = cur_sheet();
    if (!s) return;
    uint32_t r1, c1, r2, c2;
    ao_grid_sel(&grid, &r1, &c1, &r2, &c2);
    if (r2 == r1) { printf("[sheet] sort: select more than one row\n"); return; }
    push_undo();
    ao_grid_sort(grid.book, s, r1, c1, r2, c2, c1, desc);
    dirty = 1;
    printf("[sheet] sort %s by column\n", desc ? "desc" : "asc");
}

static void do_filter_apply(void) {
    ao_sheet_t *s = cur_sheet();
    filter_mode = 0;
    if (!s) return;
    uint32_t r1, c1, r2, c2;
    ao_grid_sel(&grid, &r1, &c1, &r2, &c2);
    if (!filter_buf[0]) {
        ao_grid_filter_clear(&grid);
        printf("[sheet] filter cleared\n");
        return;
    }
    push_undo();
    uint32_t kept = ao_grid_filter(&grid, s, r1, c1, r2, c2, grid.cur_col, filter_buf);
    dirty = 1;
    printf("[sheet] filter '%s': %u rows kept\n", filter_buf, kept);
}

static void do_chart_toggle(int type) {
    uint32_t r1, c1, r2, c2;
    ao_grid_sel(&grid, &r1, &c1, &r2, &c2);
    if (chart_n >= 16) { printf("[sheet] chart: full\n"); return; }
    push_undo();
    chart_t *c = &charts[chart_n++];
    c->type = type;
    c->r1 = r1; c->c1 = c1; c->r2 = r2; c->c2 = c2;
    c->arow = r2 + 2;
    c->acol = c1;
    dirty = 1;
    printf("[sheet] chart %s over %u:%u-%u:%u\n", CH_NAME[type], r1, c1, r2, c2);
}

static void do_freeze_toggle(void) {
    if (grid.freeze_rows || grid.freeze_cols) {
        grid.freeze_rows = grid.freeze_cols = 0;
        printf("[sheet] freeze off\n");
        return;
    }
    grid.freeze_rows = grid.cur_row;
    grid.freeze_cols = grid.cur_col;
    printf("[sheet] freeze %ux%u\n", grid.freeze_rows, grid.freeze_cols);
}

/* ------------------------------------------------------------ mouse ----- */

static int cell_at(int32_t mx, int32_t my, uint32_t *row, uint32_t *col) {
    if (my < (int32_t)GY || my >= (int32_t)SY) return -1;
    uint32_t rows[256];
    uint32_t n = visible_rows(rows, sizeof rows / sizeof rows[0]);
    uint32_t y = GY;
    int found = 0;
    for (uint32_t i = 0; i < n; i++) {
        if ((uint32_t)my >= y && (uint32_t)my < y + ROWH) { *row = rows[i]; found = 1; break; }
        y += ROWH;
    }
    if (!found) return -1;
    for (uint32_t c = 0; c < AO_GRID_MAX_COLS; c++) {
        if (c >= grid.freeze_cols && c < grid.left_col) continue;
        uint32_t cx = GX + col_x_view(c);
        if (cx >= WIN_W) break;
        uint32_t cw = ao_grid_col_w(&grid, c);
        if ((uint32_t)mx >= cx && (uint32_t)mx < cx + cw) { *col = c; return 0; }
    }
    return -1;
}

static int header_edge_col(int32_t mx, int32_t my) {
    if (my < (int32_t)FB_H || my >= (int32_t)GY) return -1;
    for (uint32_t c = 0; c < AO_GRID_MAX_COLS; c++) {
        if (c >= grid.freeze_cols && c < grid.left_col) continue;
        uint32_t cx = GX + col_x_view(c);
        if (cx >= WIN_W) break;
        int32_t edge = (int32_t)(cx + ao_grid_col_w(&grid, c));
        if (mx >= edge - 3 && mx <= edge + 2) return (int)c;
    }
    return -1;
}

static int header_col(int32_t mx, int32_t my) {
    if (my < (int32_t)FB_H || my >= (int32_t)GY) return -1;
    for (uint32_t c = 0; c < AO_GRID_MAX_COLS; c++) {
        if (c >= grid.freeze_cols && c < grid.left_col) continue;
        uint32_t cx = GX + col_x_view(c);
        if (cx >= WIN_W) break;
        uint32_t cw = ao_grid_col_w(&grid, c);
        if ((uint32_t)mx >= cx && (uint32_t)mx < cx + cw) return (int)c;
    }
    return -1;
}

static int gutter_row(int32_t mx, int32_t my, uint32_t *row) {
    if (mx >= (int32_t)GX) return -1;
    if (my < (int32_t)GY || my >= (int32_t)SY) return -1;
    uint32_t rows[256];
    uint32_t n = visible_rows(rows, sizeof rows / sizeof rows[0]);
    uint32_t y = GY;
    for (uint32_t i = 0; i < n; i++) {
        if ((uint32_t)my >= y && (uint32_t)my < y + ROWH) { *row = rows[i]; return 0; }
        y += ROWH;
    }
    return -1;
}

static int on_fill_handle(int32_t mx, int32_t my) {
    uint32_t r1, c1, r2, c2;
    ao_grid_sel(&grid, &r1, &c1, &r2, &c2);
    uint32_t y = row_y_view(r2);
    if (y == UINT32_MAX) return 0;
    uint32_t cx = GX + col_x_view(c2);
    uint32_t cw = ao_grid_col_w(&grid, c2);
    if (cx >= WIN_W) return 0;
    uint32_t x0 = cx + cw - 6, y0 = y + ROWH - 6;
    return mx >= (int32_t)x0 && mx < (int32_t)(x0 + 6) &&
           my >= (int32_t)y0 && my < (int32_t)(y0 + 6);
}

static void col_select(uint32_t c) {
    ao_sheet_t *s = cur_sheet();
    uint32_t n = s ? ao_sheet_cell_count(s) : 0, last = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r, cc;
        ao_cell_pos(ao_sheet_cell_sorted(grid.book, s, i), &r, &cc);
        if (r > last) last = r;
    }
    grid.anchor_row = 0;
    grid.anchor_col = c;
    grid.cur_row = last;
    grid.cur_col = c;
    grid.extend = 1;
}

static void row_select(uint32_t r) {
    grid.anchor_row = r;
    grid.anchor_col = 0;
    grid.cur_row = r;
    grid.cur_col = AO_GRID_MAX_COLS - 1;
    grid.extend = 1;
}

static void mouse_down(int32_t mx, int32_t my, uint32_t mods) {
    int hc = header_edge_col(mx, my);
    if (hc >= 0) { drag_mode = DRAG_RESIZE; drag_col = (uint32_t)hc; return; }
    int hcl = header_col(mx, my);
    if (hcl >= 0) { col_select((uint32_t)hcl); receipt_cursor(); return; }
    uint32_t gr;
    if (gutter_row(mx, my, &gr) == 0) { row_select(gr); receipt_cursor(); return; }
    if (on_fill_handle(mx, my)) {
        uint32_t r1, c1, r2, c2;
        ao_grid_sel(&grid, &r1, &c1, &r2, &c2);
        drag_mode = DRAG_FILL;
        fill_tr = r2;
        fill_tc = c2;
        return;
    }
    uint32_t r, c;
    if (cell_at(mx, my, &r, &c) != 0) return;
    if (editing && edit_buf[0] == '=') { insert_ref(r, c); return; }
    if (mods & M_SHIFT) {
        grid.cur_row = r;
        grid.cur_col = c;
        grid.extend = 1;
    } else {
        grid.anchor_row = r;
        grid.anchor_col = c;
        grid.cur_row = r;
        grid.cur_col = c;
        grid.extend = 0;
    }
    ao_grid_ensure_visible(&grid, r, c, SY - GY, WIN_W - GX);
    drag_mode = DRAG_SEL;
    receipt_cursor();
}

static void mouse_move(int32_t mx, int32_t my) {
    if (drag_mode == DRAG_RESIZE) {
        uint32_t cx = GX + col_x_view(drag_col);
        uint32_t nw = (mx > (int32_t)cx) ? (uint32_t)(mx - (int32_t)cx) : 24;
        ao_grid_col_w_set(&grid, drag_col, nw);
        return;
    }
    uint32_t r, c;
    if (cell_at(mx, my, &r, &c) != 0) return;
    if (drag_mode == DRAG_SEL) {
        grid.cur_row = r;
        grid.cur_col = c;
        grid.extend = 1;
        ao_grid_ensure_visible(&grid, r, c, SY - GY, WIN_W - GX);
        return;
    }
    if (drag_mode == DRAG_FILL) {
        uint32_t r1, c1, r2, c2;
        ao_grid_sel(&grid, &r1, &c1, &r2, &c2);
        if (r >= r2) fill_tr = r;
        if (c >= c2) fill_tc = c;
    }
}

static void mouse_up(void) {
    if (drag_mode == DRAG_FILL) {
        uint32_t r1, c1, r2, c2;
        ao_grid_sel(&grid, &r1, &c1, &r2, &c2);
        if ((fill_tr != UINT32_MAX && fill_tr > r2) ||
            (fill_tc != UINT32_MAX && fill_tc > c2)) {
            push_undo();
            ao_sheet_t *s = cur_sheet();
            if (s) {
                uint32_t tr2 = fill_tr == UINT32_MAX ? r2 : fill_tr;
                uint32_t tc2 = fill_tc == UINT32_MAX ? c2 : fill_tc;
                ao_grid_fill(grid.book, s, r1, c1, r2, c2, tr2, tc2);
                dirty = 1;
                printf("[sheet] fill to %u", tr2);
                if (tc2 > c2) printf(" (cols to %u)", tc2);
                printf("\n");
            }
        }
        fill_tr = fill_tc = UINT32_MAX;
    }
    drag_mode = DRAG_NONE;
}

/* ------------------------------------------------------------ keys ------ */

static void filter_key(uint32_t key, uint32_t cp) {
    if (key == 0x1B) {                    /* Esc: cancel */
        filter_mode = 0;
        filter_buf[0] = 0;
        return;
    }
    if (key == '\n' || key == '\r') {     /* Enter: apply */
        do_filter_apply();
        return;
    }
    if (key == '\b') {
        size_t n = strlen(filter_buf);
        if (n) filter_buf[n - 1] = 0;
        return;
    }
    if (cp >= 0x20 && cp != 0x7F) {
        size_t n = strlen(filter_buf);
        if (n + 1 < sizeof filter_buf) {
            filter_buf[n] = (char)cp;
            filter_buf[n + 1] = 0;
        }
    }
}

static void editing_key(uint32_t key, uint32_t cp) {
    switch (key) {
    case 0x1B:                            /* Esc cancels the edit */
        cancel_edit();
        return;
    case '\n':
    case '\r':
        commit_edit(1);
        return;
    case '\t':
        commit_edit(2);
        return;
    case '\b':
        if (edit_len > 0) edit_buf[--edit_len] = 0;
        return;
    default:
        break;
    }
    if (cp >= 0x20 && cp != 0x7F) {
        if (edit_len + 1 < (int)sizeof edit_buf) {
            edit_buf[edit_len++] = (char)cp;
            edit_buf[edit_len] = 0;
        }
    }
}

static void on_key(uint32_t key, uint32_t cp, uint32_t mods) {
    int shift = (mods & M_SHIFT) != 0;
    int ctrl = (mods & M_CTRL) != 0;

    if (filter_mode) {
        filter_key(key, cp);
        return;
    }
    if (editing) {
        editing_key(key, cp);
        return;
    }

    if (ctrl && (key == 'f' || key == 'F') && shift) {
        filter_mode = 1;
        filter_buf[0] = 0;
        return;
    }
    if (ctrl) {
        switch (key) {
        case 'a': case 'A': ao_grid_select_all(&grid); return;
        case 'c': case 'C': do_copy(0); return;
        case 'x': case 'X': do_copy(1); return;
        case 'v': case 'V': do_paste(); return;
        case 'z': case 'Z': do_undo(); return;
        case 'y': case 'Y': do_redo(); return;
        case 's': case 'S': do_save(); return;
        case 'o': case 'O': do_load(doc_path); return;
        }
        switch (key) {
        case K_LEFT:  ao_grid_jump(&grid, 0, -1, shift); return;
        case K_RIGHT: ao_grid_jump(&grid, 0,  1, shift); return;
        case K_UP:    ao_grid_jump(&grid, -1, 0, shift); return;
        case K_DOWN:  ao_grid_jump(&grid,  1, 0, shift); return;
        }
        return;
    }

    switch (key) {
    case K_LEFT:  ao_grid_move(&grid, 0, -1, shift); receipt_cursor(); return;
    case K_RIGHT: ao_grid_move(&grid, 0,  1, shift); receipt_cursor(); return;
    case K_UP:    ao_grid_move(&grid, -1, 0, shift); receipt_cursor(); return;
    case K_DOWN:  ao_grid_move(&grid,  1, 0, shift); receipt_cursor(); return;
    case K_HOME:
        if (ctrl) {
            ao_grid_to_edge(&grid, -1, -1, shift);
            receipt_cursor();
        } else {
            grid.cur_col = 0;
            if (!shift) grid.anchor_col = 0;
            grid.extend = shift;
            receipt_cursor();
        }
        return;
    case K_END:
        if (ctrl) {
            ao_grid_to_edge(&grid, 1, 1, shift);
            receipt_cursor();
        } else {
            grid.cur_col = AO_GRID_MAX_COLS - 1;
            if (!shift) grid.anchor_col = grid.cur_col;
            grid.extend = shift;
            receipt_cursor();
        }
        return;
    case K_PGUP:  ao_grid_move(&grid, -10, 0, shift); receipt_cursor(); return;
    case K_PGDN:  ao_grid_move(&grid,  10, 0, shift); receipt_cursor(); return;
    case '\t':    ao_grid_move(&grid, 0, shift ? -1 : 1, 0); receipt_cursor(); return;
    case '\n':
    case '\r':    ao_grid_move(&grid, 1, 0, 0); receipt_cursor(); return;
    case K_DEL:   clear_selection(); return;
    case K_F2:    {
        ao_sheet_t *s = cur_sheet();
        ao_cell_t *c = s ? ao_sheet_cell(grid.book, s, grid.cur_row, grid.cur_col) : NULL;
        start_edit(c ? ao_cell_input(c) : "");
        return;
    }
    case K_F6:    do_chart_toggle(CH_BAR); return;
    case K_F7:    do_chart_toggle(CH_LINE); return;
    case K_F8:    do_chart_toggle(CH_PIE); return;
    case K_F9:    do_freeze_toggle(); return;
    case K_F11:   do_sort(0); return;
    case K_F12:   do_sort(1); return;
    }

    if (cp >= 0x20 && cp != 0x7F) {
        /* typing starts an edit right in the cell */
        char buf[8];
        snprintf(buf, sizeof buf, "%c", (char)cp);
        start_edit(buf);
    }
}

/* ------------------------------------------------------------- main ----- */

int sheet_gui(ao_book_t *b, const char *path) {
    grid.book = b;
    book = b;                       /* the file-scope alias used above */
    ao_sheet_t *sh = ao_book_sheet(book, "Sheet1", 1);
    (void)sh;
    ao_grid_init(&grid, book, 0);
    if (path) do_load(path);

    if (bench_requested) {
        ao_sheet_t *bs = cur_sheet();
        char num[24];
        for (uint32_t i = 0; i < 10000; i++) {
            snprintf(num, sizeof num, "%u", i + 1);
            ao_sheet_set(grid.book, bs, i, 0, num);
        }
        ao_sheet_set(grid.book, bs, 0, 1, "=SUM(A1:A10000)");
        bench_left = 200;
        bench_total = 0;
        bench_max = 0;
        printf("[sheet] bench: 10000 rows loaded\n");
    }

    wid = ag_window_create(40, 30, WIN_W, WIN_H, "AuraLite Sheet", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    ag_window_focus(wid);
    printf("[sheet] gui: workbook ready (%u cells)\n",
           ao_sheet_cell_count(cur_sheet()));
    repaint();
    receipt_cursor();

    for (;;) {
        ag_event_t e;
        if (!ag_poll_event(wid, &e)) {
            for (volatile int i = 0; i < 200000; i++) {}
            if (bench_left > 0) {
                grid.top_row += 25;
                if (grid.top_row > 9800) grid.top_row = 0;
                repaint();
                if (--bench_left == 0) {
                    printf("[sheet] bench scroll: 200 frames, avg %ld us, "
                           "worst %ld us, arena %llu bytes\n",
                           bench_total / 200, bench_max,
                           (unsigned long long)ao_arena_used(&arena));
                    printf("[sheet] bench: done\n");
                    bench_left = -1;
                }
            }
            continue;
        }
        if (e.type == AG_EVT_CLOSE_REQ) break;
        if (e.type == AG_EVT_KEY_DOWN) {
            if (e.key == 0x1B && !editing && !filter_mode) break;
            uint32_t cp = e.data ? e.data : (e.key < 0x80 ? e.key : 0);
            on_key(e.key, cp, e.mods);
            repaint();
            continue;
        }
        if (e.type == AG_EVT_MOUSE_WHEEL) {
            int delta = (int)(int32_t)e.key;
            ao_grid_scroll(&grid, -delta * 3, 0);
            repaint();
            continue;
        }
        if (e.type == AG_EVT_MOUSE_DOWN) {
            mouse_down(e.x, e.y, e.mods);
            repaint();
            continue;
        }
        if (e.type == AG_EVT_MOUSE_MOVE) {
            if (drag_mode != DRAG_NONE) {
                mouse_move(e.x, e.y);
                repaint();
            }
            continue;
        }
        if (e.type == AG_EVT_MOUSE_UP) {
            mouse_up();
            repaint();
            continue;
        }
        if (e.type == AG_EVT_PAINT) {
            repaint();
            continue;
        }
    }

    if (dirty) printf("[sheet] unsaved changes (Esc discarded nothing)\n");
    ag_window_destroy(wid);
    return 0;
}

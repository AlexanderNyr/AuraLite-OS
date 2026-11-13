/* tests/unit/test_ao_grid.c — OFFICE_PLAN OF5 grid gate.
 *
 * Viewport arithmetic (no per-row allocation, ever), the selection
 * model, formula translation for copy/fill, TSV round trips, series
 * fill, stable sort, and the filter predicates — all against the OF4
 * engine, all on the host.
 */
#include "aocalc.h"
#include "aogrid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t mem[64 * 1024 * 1024];
static ao_arena_t arena;
static ao_book_t *book;
static int total, fails;

static void check(const char *name, int cond) {
    total++;
    if (!cond) { printf("AO GRID FAIL: %s\n", name); fails++; }
}

static ao_sheet_t *new_sheet(void) {
    /* Detach a scratch arena per case where the book must stay pristine;
       most cases share the big one. */
    return ao_book_sheet(book, "Sheet1", 1);
}

typedef struct { char *buf; size_t len, cap; } sink_t;
static void sink_emit(void *ctx, const char *data, size_t n) {
    sink_t *sk = ctx;
    if (sk->len + n + 1 > sk->cap) {
        sk->cap = (sk->len + n + 1) * 2;
        sk->buf = realloc(sk->buf, sk->cap);
    }
    memcpy(sk->buf + sk->len, data, n);
    sk->len += n;
    sk->buf[sk->len] = 0;
}

static double getn(ao_sheet_t *s, uint32_t r, uint32_t c) {
    ao_value_t v = ao_sheet_get(book, s, r, c);
    return v.kind == AO_VAL_NUM ? v.num : -1e99;
}
static void cellstr(ao_sheet_t *s, uint32_t r, uint32_t c, char *buf, size_t n) {
    ao_cell_t *cell = ao_sheet_cell(book, s, r, c);
    snprintf(buf, n, "%s", cell ? ao_cell_input(cell) : "");
}

int main(void) {
    ao_arena_init(&arena, mem, sizeof mem);
    book = ao_book_new(&arena);
    if (!book) { printf("AO GRID: FAIL (no book)\n"); return 1; }
    ao_sheet_t *s = new_sheet();

    /* ---- viewport: the scroll position is arithmetic, not allocation --- */
    ao_grid_t g;
    ao_grid_init(&g, book, 0);
    check("rows visible basic", ao_grid_rows_visible(&g, 16 * 20) == 20);
    check("cols visible default", ao_grid_cols_visible(&g, 72 * 8) == 8);
    g.top_row = 99999;
    check("viewport at 100k rows without iterating", g.top_row == 99999 &&
          ao_grid_rows_visible(&g, 16 * 20) == 20);
    g.top_row = 0;
    g.freeze_rows = 2; g.freeze_cols = 1;
    check("freeze removes from the scrolled pane", ao_grid_rows_visible(&g, 16 * 20) == 18);
    g.freeze_rows = 0; g.freeze_cols = 0;

    /* ensure_visible: down past the bottom pulls the window along */
    g.top_row = 0;
    ao_grid_ensure_visible(&g, 50, 0, 16 * 20, 72 * 8);
    check("ensure_visible scrolls down", g.top_row == 31);
    ao_grid_ensure_visible(&g, 0, 0, 16 * 20, 72 * 8);
    check("ensure_visible scrolls back up", g.top_row == 0);

    /* ---- selection ------------------------------------------------------- */
    ao_grid_init(&g, book, 0);
    ao_grid_move(&g, 4, 3, 0);              /* cursor to D5 */
    check("cursor moved", g.cur_row == 4 && g.cur_col == 3);
    check("anchor follows without shift", g.anchor_row == 4 && g.anchor_col == 3);
    ao_grid_move(&g, 0, -2, 1);             /* shift-select D5:B5 */
    {
        uint32_t r1, c1, r2, c2;
        ao_grid_sel(&g, &r1, &c1, &r2, &c2);
        check("selection normalises", r1 == 4 && r2 == 4 && c1 == 1 && c2 == 3);
    }
    check("in_sel member", ao_grid_in_sel(&g, 4, 2) && !ao_grid_in_sel(&g, 3, 2));
    ao_grid_move(&g, 0, 2, 0);              /* plain move drops the selection */
    check("plain move collapses selection", ao_grid_sel_rows(&g) == 1);
    ao_grid_jump(&g, -1, 0, 0);             /* Home-ish: A-column boundary */
    check("jump falls back to edge of data", g.cur_row == 0);

    /* ---- formula translation -------------------------------------------- */
    char out[512];
    ao_formula_translate("=A1+B2", 2, 3, out, sizeof out);
    check("plain refs translate", strcmp(out, "=D3+E4") == 0);
    ao_formula_translate("=$A$1+$B2", 2, 3, out, sizeof out);
    check("$ anchors hold", strcmp(out, "=$A$1+$B4") == 0);
    ao_formula_translate("=Data!A1", 1, 1, out, sizeof out);
    check("sheet prefix survives", strcmp(out, "=Data!B2") == 0);
    ao_formula_translate("=LOG10(100)", 1, 1, out, sizeof out);
    check("function names untouched", strcmp(out, "=LOG10(100)") == 0);
    ao_formula_translate("=SUM(A1:A5)", 2, 2, out, sizeof out);
    check("ranges translate", strcmp(out, "=SUM(C3:C7)") == 0);
    ao_formula_translate("=Z99999999999+A1", 1, 1, out, sizeof out);
    check("off-grid becomes #REF!", strstr(out, "#REF!") != NULL && strstr(out, "B2") != NULL);
    ao_formula_translate("=\"A1\"", 2, 3, out, sizeof out);
    check("string literals untouched", strcmp(out, "=\"A1\"") == 0);
    ao_formula_translate("=B2-1", 0, -1, out, sizeof out);
    check("left column translate", strcmp(out, "=A2-1") == 0);

    /* ---- copy/paste round trip ------------------------------------------ */
    ao_sheet_set(book, s, 0, 0, "1");
    ao_sheet_set(book, s, 0, 2, "3");
    ao_sheet_set(book, s, 1, 0, "=A1*10");
    sink_t sk = {0};
    ao_grid_copy_tsv(book, s, 0, 0, 1, 2, sink_emit, &sk);
    check("TSV copy shape", sk.buf &&
          strcmp(sk.buf, "1\t\t3\t\n=A1*10\t\t\t\n") == 0);
    free(sk.buf);

    /* Paste into fresh cells with a delta: formulas translate */
    uint32_t pw = ao_grid_paste_tsv(book, s, "=A1+100\t\n", 5, 0, 2, 1);
    check("paste count", pw == 1);
    {
        char f[64]; cellstr(s, 5, 0, f, sizeof f);
        /* dr=2 was relative to the copy delta; here we asked (2,1). */
        check("paste translates formula", strcmp(f, "=B3+100") == 0);
    }

    /* Paste TSV with blanks preserves column positions */
    pw = ao_grid_paste_tsv(book, s, "9\t\t11\n\t8\t\n", 10, 0, 0, 0);
    check("paste with gaps counts written", pw == 3);
    check("gap kept empty", getn(s, 10, 0) == 9 && getn(s, 10, 2) == 11);
    check("empty row shifts rows", getn(s, 11, 1) == 8);

    /* ---- fill: series continuation and translated formulas -------------- */
    ao_sheet_set(book, s, 20, 0, "10");
    ao_sheet_set(book, s, 21, 0, "20");
    ao_grid_fill(book, s, 20, 0, 21, 0, 24, 0);
    check("series continues from the last source cell",
          getn(s, 22, 0) == 30 && getn(s, 23, 0) == 40 && getn(s, 24, 0) == 50);

    ao_sheet_set(book, s, 30, 0, "=A29*2");     /* after A29 which is empty here */
    ao_grid_fill(book, s, 30, 0, 30, 0, 32, 0);
    {
        char f1[64], f2[64];
        cellstr(s, 31, 0, f1, sizeof f1);
        cellstr(s, 32, 0, f2, sizeof f2);
        check("formula fill translates one row per step",
              strcmp(f1, "=A30*2") == 0 && strcmp(f2, "=A31*2") == 0);
    }

    /* ---- sort: stable, blanks last --------------------------------------- */
    ao_sheet_set(book, s, 40, 0, "b");
    ao_sheet_set(book, s, 41, 0, "d");
    ao_sheet_set(book, s, 42, 0, "a");
    ao_sheet_set(book, s, 43, 0, "c");
    ao_sheet_set(book, s, 44, 0, "e");
    ao_sheet_set(book, s, 40, 1, "b1");
    ao_sheet_set(book, s, 41, 1, "d1");
    ao_sheet_set(book, s, 42, 1, "a1");
    ao_sheet_set(book, s, 43, 1, "c1");
    ao_sheet_set(book, s, 44, 1, "e1");
    ao_grid_sort(book, s, 40, 0, 44, 1, 0, 0);
    {
        char a[8], bcol[8];
        cellstr(s, 40, 0, a, sizeof a); cellstr(s, 40, 1, bcol, sizeof bcol);
        check("sort ascending row 0", strcmp(a, "a") == 0 && strcmp(bcol, "a1") == 0);
        cellstr(s, 44, 0, a, sizeof a);
        check("sort ascending last", strcmp(a, "e") == 0);
    }
    ao_grid_sort(book, s, 40, 0, 44, 1, 0, 1);
    {
        char a[8];
        cellstr(s, 40, 0, a, sizeof a);
        check("sort descending", strcmp(a, "e") == 0);
    }
    /* stability: equal keys keep their original relative order */
    ao_sheet_set(book, s, 50, 0, "1");
    ao_sheet_set(book, s, 51, 0, "1");
    ao_sheet_set(book, s, 52, 0, "2");
    ao_sheet_set(book, s, 50, 1, "z");
    ao_sheet_set(book, s, 51, 1, "y");
    ao_sheet_set(book, s, 52, 1, "x");
    ao_grid_sort(book, s, 50, 0, 52, 1, 0, 0);
    {
        char p[8], q[8];
        cellstr(s, 50, 1, p, sizeof p);
        cellstr(s, 51, 1, q, sizeof q);
        check("sort stable for equal keys", strcmp(p, "z") == 0 && strcmp(q, "y") == 0);
    }

    /* ---- filter: numeric comparison and globs ----------------------------- */
    ao_sheet_set(book, s, 60, 0, "1");
    ao_sheet_set(book, s, 61, 0, "2");
    ao_sheet_set(book, s, 62, 0, "3");
    ao_sheet_set(book, s, 63, 0, "4");
    ao_sheet_set(book, s, 64, 0, "5");
    {
        ao_grid_t fg; ao_grid_init(&fg, book, 0);
        uint32_t kept = ao_grid_filter(&fg, s, 60, 0, 64, 0, 0, ">= 3");
        check("filter >=3 keeps three rows", kept == 3);
        check("filter hides the rest", ao_grid_row_hidden(&fg, 60) &&
              !ao_grid_row_hidden(&fg, 62));
        ao_grid_filter_clear(&fg);
        check("filter clear restores visibility", !ao_grid_row_hidden(&fg, 60));
        kept = ao_grid_filter(&fg, s, 60, 0, 64, 0, 0, "1");
        check("filter exact number", kept == 1 && !ao_grid_row_hidden(&fg, 60));
        ao_grid_filter_clear(&fg);
        ao_sheet_set(book, s, 70, 0, "alpha");
        ao_sheet_set(book, s, 71, 0, "beta");
        ao_sheet_set(book, s, 72, 0, "album");
        kept = ao_grid_filter(&fg, s, 70, 0, 72, 0, 0, "a*");
        check("glob a* keeps two rows", kept == 2 &&
              !ao_grid_row_hidden(&fg, 70) && !ao_grid_row_hidden(&fg, 72) &&
              ao_grid_row_hidden(&fg, 71));
        ao_grid_filter_clear(&fg);
    }

    printf("AO GRID: %s (%d checks, arena %llu bytes)\n",
           fails ? "FAIL" : "PASS", total, (unsigned long long)ao_arena_used(&arena));
    return fails ? 1 : 0;
}

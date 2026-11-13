/* aogrid.h — OFFICE_PLAN.md OF5: the virtualised grid behind the Sheet.
 *
 * The grid never allocates per row: only the viewport maths and the
 * selection live in here, everything else goes through the OF4 engine.
 * A 100 000-row workbook scrolls in O(viewport), because the frame only
 * ever asks "which cells are visible?" and "what do they contain?".
 *
 * Selection is rectangular: an anchor and the cursor.  F2/arrows/Tab/
 * Enter move the cursor; Shift extends the selection; Ctrl+arrows jump
 * the used range; F9 freezes the panes at the cursor; F11/F12 sort the
 * selection by its first column; Ctrl+Shift+F filters the selection by
 * a typed criterion (">= 3", "a*").  Fill (the drag handle) extends a
 * numeric series or translates formulas from the source range; paste
 * translates formulas relative to their new home.
 */
#ifndef AOGRID_H
#define AOGRID_H

#include <stdint.h>
#include <stddef.h>
#include "aocalc.h"

#define AO_GRID_ROW_H   16
#define AO_GRID_HDR_W   34          /* row-number gutter                  */
#define AO_GRID_MAX_COLS 80         /* A..CB — what the grid can show     */
#define AO_GRID_MAX_ROWS 1048576u
#define AO_GRID_DEFAULT_COL_W 72

/* Filter ops for ao_grid_filter(). */
typedef enum {
    AO_FILTER_NUM = 0,   /* parsed from ">= 3", "<= 10", "= 7", "<> 0", "> 1" */
    AO_FILTER_TEXT  = 1, /* glob: "a*", "*z", "*mid*", exact text otherwise   */
} ao_grid_filter_t;

typedef struct ao_grid {
    ao_book_t *book;
    uint32_t   sheet;          /* sheet index in the book (always 0 for now) */

    uint32_t top_row;          /* first visible scrolled row (below freeze) */
    uint32_t left_col;         /* first visible scrolled col (right of freeze) */
    uint32_t freeze_rows;      /* rows always visible (0 = no freeze)       */
    uint32_t freeze_cols;

    uint32_t cur_row, cur_col;     /* the cursor (active cell)              */
    uint32_t anchor_row, anchor_col; /* where the selection started         */
    int      extend;               /* Shift was held for the last move      */

    uint16_t col_w[AO_GRID_MAX_COLS]; /* per-column override, 0 = default   */

    /* Filter: rows hidden by the last filter() call.  malloc'd; rebuilt on
     * every filter() call, freed by filter_clear() / a later filter(). */
    uint32_t *hidden;
    uint32_t  hidden_n;
    int       filter_on;
} ao_grid_t;

void ao_grid_init(ao_grid_t *g, ao_book_t *book, uint32_t sheet);

/* --- viewport ------------------------------------------------------------ */

/* Default column width when no per-column override is set. */
void     ao_grid_col_w_set(ao_grid_t *g, uint32_t col, uint32_t px);
uint32_t ao_grid_col_w(const ao_grid_t *g, uint32_t col);
/* X offset of a scrolled column in pixels, left of the data area. */
uint32_t ao_grid_col_x(const ao_grid_t *g, uint32_t col);
/* Pixel x of the data area's right edge for the window. */
uint32_t ao_grid_x_of_col(const ao_grid_t *g, uint32_t col);
/* How many rows/cols fit in a window of `height_px`/`width_px` below the
   headers/gutter.  Never touches the engine. */
uint32_t ao_grid_rows_visible(const ao_grid_t *g, uint32_t height_px);
uint32_t ao_grid_cols_visible(const ao_grid_t *g, uint32_t width_px);
/* Scroll so `row`/`col` are inside the scrolled viewport (freeze rows and
   cols never scroll out). */
void ao_grid_ensure_visible(ao_grid_t *g, uint32_t row, uint32_t col,
                            uint32_t height_px, uint32_t width_px);
/* Scroll by whole rows/cols (wheel). */
void ao_grid_scroll(ao_grid_t *g, int drows, int dcols);

/* --- selection ------------------------------------------------------------ */

void ao_grid_move(ao_grid_t *g, int drow, int dcol, int extend);
void ao_grid_jump(ao_grid_t *g, int dr, int dc, int extend);   /* Ctrl+arrows */
void ao_grid_to_edge(ao_grid_t *g, int dr, int dc, int extend); /* Ctrl+Home/End */
void ao_grid_select_all(ao_grid_t *g);
void ao_grid_sel(const ao_grid_t *g,
                 uint32_t *r1, uint32_t *c1, uint32_t *r2, uint32_t *c2);
uint32_t ao_grid_sel_rows(const ao_grid_t *g);
uint32_t ao_grid_sel_cols(const ao_grid_t *g);
int      ao_grid_in_sel(const ao_grid_t *g, uint32_t row, uint32_t col);

/* --- editing --------------------------------------------------------------- */

/* Move one cell right/down with wrap to the next row at a column edge?
   Kept internal to the app; here only the primitives it needs. */

/* --- copy/paste/fill -------------------------------------------------------- */

/* Translate every cell reference in a formula by (drow, dcol), keeping $
   anchors, sheet prefixes and ranges intact; references forced off-grid
   become #REF!.  Non-formula input is copied verbatim.  `out` may alias
   `in`. */
void ao_formula_translate(const char *in, int drow, int dcol,
                          char *out, size_t out_n);

/* Render the selected range as TSV (tab-separated), trailing tab after
   each cell including the last one in a row; newline per row. */
typedef void (*ao_grid_emit_fn)(void *ctx, const char *data, size_t n);
void ao_grid_copy_tsv(ao_book_t *b, ao_sheet_t *s,
                      uint32_t r1, uint32_t c1, uint32_t r2, uint32_t c2,
                      ao_grid_emit_fn emit, void *ctx);
/* Paste TSV at (row, col): formulas translate by (drow, dcol) relative to
   the copy origin — pass (0, 0) for a foreign clipboard.  Values go
   through the engine.  Returns the number of non-empty cells written. */
uint32_t ao_grid_paste_tsv(ao_book_t *b, ao_sheet_t *s,
                           const char *tsv, uint32_t row, uint32_t col,
                           int drow, int dcol);
/* Extend a selection: with a numeric run the next cells continue the
   step; with formulas they translate.  `target` is the one-cell-high or
   one-cell-wide extension (drag handle). */
void ao_grid_fill(ao_book_t *b, ao_sheet_t *s,
                  uint32_t r1, uint32_t c1, uint32_t r2, uint32_t c2,
                  uint32_t tr2, uint32_t tc2);

/* --- sort / filter ----------------------------------------------------------- */

/* Stable sort of the rows [r1..r2] over columns [c1..c2] by column `by`
   (absolute), ascending unless `desc`.  Blanks go last. */
void ao_grid_sort(ao_book_t *b, ao_sheet_t *s,
                  uint32_t r1, uint32_t c1, uint32_t r2, uint32_t c2,
                  uint32_t by, int desc);
/* Parse ">= 3" / "<= 2" / "= 5" / "<> 1" / "a*" / "*text" / "plain" and
   hide the non-matching rows of [r1..r2].  Returns the rows kept.
   `criterion` is freed afterwards only by the grid (arena of the book). */
uint32_t ao_grid_filter(ao_grid_t *g, ao_sheet_t *s,
                        uint32_t r1, uint32_t c1, uint32_t r2, uint32_t c2,
                        uint32_t col, const char *criterion);
void     ao_grid_filter_clear(ao_grid_t *g);
int      ao_grid_row_hidden(const ao_grid_t *g, uint32_t row);

#endif /* AOGRID_H */

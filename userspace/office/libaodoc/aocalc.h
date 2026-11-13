/* userspace/office/libaodoc/aocalc.h — the Sheet calculation engine.
 *
 * OFFICE_PLAN OF4: an engine that is correct before it is pretty.  This is
 * the pure core the grid (OF5) will draw: a workbook of sparse sheets, a
 * formula parser (recursive descent over an on-demand tokeniser), a
 * dependency graph with dirty propagation, cycle detection and a
 * recalculation budget, the 49 functions the plan names, number
 * formatting, and CSV/TSV import/export.
 *
 * Everything lives in the caller's arena: the book, its sheets, the hash
 * slots, the parsed formulas and the computed strings.  Resetting the
 * arena frees the whole workbook; the arena's own counters are the leak
 * report the gate prints.
 *
 * Addressing is the spreadsheet standard: rows 0..AO_SHEET_MAX_ROWS-1 and
 * columns 0..AO_SHEET_MAX_COLS-1 (A1..XFD1048576) may be *named* by any
 * reference, but a sheet physically holds at most AO_SHEET_MAX_CELLS
 * non-blank cells — the practical cap, documented here on purpose.
 */
#ifndef AOCALC_H
#define AOCALC_H

#include "aodoc.h"   /* ao_arena_t */

#include <stddef.h>
#include <stdint.h>

#define AO_SHEET_MAX_ROWS   1048576u
#define AO_SHEET_MAX_COLS   16384u
#define AO_SHEET_MAX_CELLS  65536u     /* per sheet: the practical cap */
#define AO_BOOK_MAX_SHEETS  64
#define AO_RANGE_MAX_CELLS  1048576u   /* one range may span at most this */
#define AO_RECALC_BUDGET    4000000L   /* node evals per recalculation */

enum {
    AO_CALC_OK = 0,
    AO_CALC_ERR_ARGS = -1,
    AO_CALC_ERR_PARSE = -2,     /* a formula that does not parse */
    AO_CALC_ERR_NOMEM = -3,     /* arena exhausted */
    AO_CALC_ERR_CAP = -4,       /* the practical cell cap was reached */
    AO_CALC_ERR_BUDGET = -5,    /* recalculation budget refused the graph */
    AO_CALC_ERR_FORMAT = -6,    /* malformed .aosheet / CSV input */
    AO_CALC_ERR_NOTFOUND = -7
};

/* ---- values ------------------------------------------------------------ */

typedef enum {
    AO_VAL_BLANK = 0,
    AO_VAL_NUM,
    AO_VAL_TEXT,
    AO_VAL_BOOL,
    AO_VAL_ERR,
    AO_VAL_RANGE     /* internal: only produced by a range expression */
} ao_val_kind_t;

typedef enum {
    AO_ERR_NONE = 0,
    AO_ERR_DIV0,     /* #DIV/0! */
    AO_ERR_VALUE,    /* #VALUE! */
    AO_ERR_REF,      /* #REF!   */
    AO_ERR_NAME,     /* #NAME?  */
    AO_ERR_NUM,      /* #NUM!   */
    AO_ERR_CYCLE,    /* #CYCLE! */
    AO_ERR_NA        /* #N/A    */
} ao_err_t;

typedef struct {
    uint8_t  kind;          /* ao_val_kind_t */
    uint8_t  err;           /* ao_err_t when kind == AO_VAL_ERR */
    double   num;           /* NUM and BOOL (0/1) */
    const char *text;       /* TEXT (arena-owned) */
    /* RANGE payload (internal): */
    int32_t  sheet;
    uint32_t r1, c1, r2, c2;
} ao_value_t;

ao_value_t ao_val_blank(void);
ao_value_t ao_val_num(double n);
ao_value_t ao_val_bool(int b);
ao_value_t ao_val_text(const char *s);
ao_value_t ao_val_err(ao_err_t e);
const char *ao_err_str(ao_err_t e);

/* Coercions, the spreadsheet way.  Errors propagate through both. */
int ao_val_to_num(ao_value_t v, double *out);        /* 0 ok, else err set */
typedef struct ao_book ao_book_t;
const char *ao_val_to_text(ao_book_t *b, ao_value_t v, char *buf, size_t n);

/* ---- references --------------------------------------------------------- */

/* Parse "A1" / "$B$12" (no sheet part) to 0-based row/col.
 * Returns 0 on success, f < 0 on failure.  abs_r/abs_c report the $ flags. */
int ao_ref_parse(const char *s, uint32_t *row, uint32_t *col,
                 int *abs_row, int *abs_col);
/* Format 0-based row/col as "B12" (with $ when asked). */
void ao_ref_format(uint32_t row, uint32_t col, int abs_row, int abs_col,
                   char *buf, size_t n);
/* "B12" from 0-based row/col with no $ markers. */
void ao_ref_name(uint32_t row, uint32_t col, char *buf, size_t n);

/* ---- the workbook ------------------------------------------------------- */

typedef struct ao_sheet ao_sheet_t;
typedef struct ao_cell ao_cell_t;

ao_book_t *ao_book_new(ao_arena_t *arena);
/* Sheet by name (created when create != 0), or NULL.  Names are
 * case-insensitive and may not contain '!' or ' '. */
ao_sheet_t *ao_book_sheet(ao_book_t *b, const char *name, int create);
ao_sheet_t *ao_book_sheet_at(ao_book_t *b, int idx);
int         ao_book_sheet_count(const ao_book_t *b);
const char *ao_sheet_name(const ao_sheet_t *s);
/* Copy a sheet (contents and formulas) or rename it.  0 on success. */
int ao_book_sheet_copy(ao_book_t *b, const char *from, const char *to);
int ao_book_sheet_rename(ao_book_t *b, const char *from, const char *to);

/* Set a cell from user text: "=..." is a formula, a number is a number,
 * TRUE/FALSE a boolean, anything else text.  An empty string clears.
 * Editing marks the cell and everything that (transitively) depends on
 * it dirty. */
int ao_sheet_set(ao_book_t *b, ao_sheet_t *s, uint32_t row, uint32_t col,
                 const char *input);
/* The cell at row/col, or NULL when blank (never stored). */
ao_cell_t *ao_sheet_cell(ao_book_t *b, ao_sheet_t *s, uint32_t row, uint32_t col);
uint32_t   ao_sheet_cell_count(const ao_sheet_t *s);
const char *ao_cell_input(const ao_cell_t *c);   /* what the user typed */
/* The computed value; recalculates the dirty subgraph under it first.
 * A blank cell reads as VAL_BLANK. */
ao_value_t ao_sheet_get(ao_book_t *b, ao_sheet_t *s, uint32_t row, uint32_t col);

/* Recalculate every dirty cell in the book.  AO_CALC_OK, or
 * AO_CALC_ERR_BUDGET when the graph refused to settle inside the budget
 * (the cells it could not settle read #NUM!). */
int ao_book_recalc(ao_book_t *b);
void ao_book_set_budget(ao_book_t *b, long budget);
long ao_book_last_steps(const ao_book_t *b);   /* node evals, last recalc */

/* Pin "now" (an Excel serial: days since 1899-12-30, fraction = time of
 * day) for TODAY()/NOW(); 0 resumes the wall clock. */
void ao_book_set_now(ao_book_t *b, double serial);

/* Iterate stored cells in row/col order: idx 0..count-1. */
ao_cell_t *ao_sheet_cell_sorted(ao_book_t *b, ao_sheet_t *s, uint32_t idx);
void ao_cell_pos(const ao_cell_t *c, uint32_t *row, uint32_t *col);

/* ---- one-shot evaluation (the formula gate's front door) ----------------- */

/* Evaluate a formula string (with or without the leading '=') in the
 * context of a sheet.  The result may still be an error value — a parse
 * failure is the only AO_CALC_ERR_* return (with the value set to the
 * matching error kind). */
int ao_formula_eval(ao_book_t *b, ao_sheet_t *ctx, const char *formula,
                    ao_value_t *out);

/* ---- number formatting ---------------------------------------------------- */

/* Format a value with an Excel-flavoured format string:
 *   "General", "0", "0.00", "#,##0", "#,##0.00", "0%", "0.00%",
 *   "$#,##0.00" (any literal prefix/suffix), "yyyy-mm-dd" (and friends:
 *   dd/mm/yyyy, hh:mm within a date format).
 * Unknown formats fall back to General. */
void ao_format_value(ao_book_t *b, ao_value_t v, const char *fmt,
                     char *buf, size_t n);

/* ---- the .aosheet file -----------------------------------------------------
 *
 * A text format a shell can write and a human can read:
 *     # comment
 *     [Sheet1]
 *     A1 = 5
 *     A2 = "text with spaces"
 *     A3 = =SUM(A1:A2)
 *     [Costs]
 *     B1 = =Sheet1!A3*2
 * Blank lines and #-comments are ignored.  Saving writes every non-blank
 * cell in row/col order, formulas exactly as typed.
 */
typedef void (*ao_emit_fn)(void *ctx, const char *data, size_t n);
int ao_book_save(ao_book_t *b, ao_emit_fn emit, void *ctx);
int ao_book_load(ao_book_t *b, const char *text, size_t n);

/* ---- CSV / TSV --------------------------------------------------------------
 * Import parses quoted fields ("" escapes, embedded delimiters and
 * newlines, CRLF or LF).  With sniff != 0 the delimiter is guessed from
 * the first line (tab, then ';', then ',').  Imported fields use the
 * same content rules as cell input, so "=..." in a CSV is a formula.
 * Export writes one sheet, quoting exactly when a field needs it.
 */
int ao_csv_import(ao_book_t *b, ao_sheet_t *s, const char *text, size_t n,
                  int sniff, char delim);
int ao_csv_export(ao_book_t *b, ao_sheet_t *s, char delim,
                  ao_emit_fn emit, void *ctx);

#endif /* AOCALC_H */

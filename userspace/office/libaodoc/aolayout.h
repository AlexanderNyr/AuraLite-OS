/* userspace/office/libaodoc/aolayout.h — page layout (OFFICE_PLAN.md OF3).
 *
 * OF2's Writer edited a model and painted fixed rows.  OF3 gives the document
 * a PAGE: a geometry (size + margins), paragraph styles with metrics, greedy
 * word wrap against a caller-supplied measuring function, list markers,
 * tables as a second layout algorithm over TABLE paragraphs, inline images
 * scaled to the page, and pagination with headers/footers/page numbers.
 *
 * Everything here is pure: the model plus two callbacks (measure text, image
 * size) in, line/page tables out.  The same tables drive the on-screen page
 * view, the print preview and (from OF9) the PDF writer — one page model,
 * several consumers.  The host gate grades the shipping source with a
 * fixed-width measuring fixture, so wrap points are asserted, not eyeballed.
 *
 * Paragraph kinds come from the model (AO_PARA_* in aomodel.h):
 *   TABLE  — one paragraph is one row; cells are '\t'-separated; a cell whose
 *            first byte is AO_TABLE_MERGE_MARK continues the cell to its left
 *            (horizontal merge).  Consecutive TABLE paragraphs are one table.
 *   IMAGE  — the paragraph text is a path; the image-size callback supplies
 *            pixels, layout scales to fit and caps the height.
 */
#ifndef AURALITE_OFFICE_AOLAYOUT_H
#define AURALITE_OFFICE_AOLAYOUT_H

#include <stddef.h>
#include <stdint.h>

#include "aomodel.h"

#define AO_TABLE_MERGE_MARK '\x01'

/* A4 at the suite's internal 1px = 1/10mm-ish scale is overkill; the page is
 * 595x842 units (points, as PDF will want in OF9).  The Writer's window maps
 * its 560px page onto the content width between margins. */
typedef struct {
    uint32_t page_w, page_h;
    uint32_t margin_l, margin_r, margin_t, margin_b;
} ao_page_geom_t;

static inline ao_page_geom_t ao_page_geom_default(void) {
    ao_page_geom_t g = { 595, 842, 56, 56, 48, 48 };
    return g;
}
static inline uint32_t ao_page_content_w(const ao_page_geom_t *g) {
    return g->page_w - g->margin_l - g->margin_r;
}
static inline uint32_t ao_page_content_h(const ao_page_geom_t *g) {
    return g->page_h - g->margin_t - g->margin_b;
}

/* Metrics a paragraph style contributes.  Direct formatting layers over
 * this; the style never touches run bits. */
typedef struct {
    uint8_t size_px;        /* nominal text size for wrap/height */
    int16_t space_before;   /* units above the first line */
    int16_t space_after;    /* units below the last line */
    int16_t indent;         /* extra left indent (lists add level indent) */
    uint8_t bold;           /* headings render bold even without run bits */
    uint8_t italic;         /* quote */
} ao_para_metrics_t;

void ao_para_metrics(uint8_t para_style, ao_para_metrics_t *out);

/* Measure `len` bytes of UTF-8 at `size_px`; returns width in page units.
 * Supplied by the consumer (the Writer wraps ao_text_width; the host gate
 * wraps a fixed-width fixture). */
typedef int (*ao_measure_fn)(void *ctx, const char *text, size_t len,
                             uint8_t size_px);
/* Pixel size of the image at `path`; 0 return = unknown (layout then uses a
 * capped placeholder box). */
typedef int (*ao_image_size_fn)(void *ctx, const char *path, uint32_t len,
                                uint32_t *w, uint32_t *h);

#define AO_LAYOUT_MAX_LINE_MARKER 12

typedef struct {
    uint32_t para;          /* paragraph index */
    uint32_t start, len;    /* byte slice of the paragraph text (no '\n') */
    int32_t  y;             /* top of the line, page units from page top */
    uint16_t h;             /* line height */
    uint8_t  size_px;
    uint8_t  kind;          /* AO_PARA_* kind of the paragraph */
    char     marker[AO_LAYOUT_MAX_LINE_MARKER]; /* "•", "3.", "" */
} ao_layout_line_t;

typedef struct {
    uint32_t first_line, line_count;
} ao_layout_page_t;

typedef struct {
    ao_layout_line_t *lines;
    uint32_t line_cap, line_len;
    ao_layout_page_t *pages;
    uint32_t page_cap, page_len;
} ao_layout_t;

void ao_layout_init(ao_layout_t *l, ao_layout_line_t *lines, uint32_t line_cap,
                    ao_layout_page_t *pages, uint32_t page_cap);

/* Lay out the whole document.  Returns AO_OK, or AO_E_FULL when the caller's
 * line/page arrays are too small (line_len then holds what would be needed
 * in lines; nothing is half-paginated silently — pages beyond the cap are
 * counted in page_len but not stored... see aolayout.c: page_len is the
 * total, stored pages are min(page_len, page_cap)). */
int ao_layout_build(ao_layout_t *l, const ao_model_t *m,
                    const ao_page_geom_t *g,
                    ao_measure_fn measure, void *measure_ctx,
                    ao_image_size_fn image_size, void *image_ctx);

/* Index of the page containing the line that contains byte `pos`, or -1. */
int ao_layout_page_of(const ao_layout_t *l, const ao_model_t *m, uint32_t pos);

/* Header/footer text.  The header is the document title; the footer is
 * "Page N of M" — page numbers are a layout product, not stored text. */
void ao_layout_header(char *out, size_t cap, const char *title);
void ao_layout_footer(char *out, size_t cap, uint32_t page, uint32_t total);

/* PNG size from the file bytes themselves (signature + IHDR).  0 = not a
 * PNG we can size.  This is the whole of what layout needs from an image;
 * the pixel decode stays with the consumer. */
int ao_png_size(const uint8_t *data, size_t n, uint32_t *w, uint32_t *h);

/* Display size of an image of `iw`x`ih` pixels on this page: scaled to fit
 * the content width, height capped at the content height (placement cap). */
void ao_image_fit(const ao_page_geom_t *g, uint32_t iw, uint32_t ih,
                  uint32_t *dw, uint32_t *dh);

/* ------------------------------------------------------------ tables ---- */

/* One cell of the grid algorithm: text, horizontal span, header shading. */
typedef struct {
    const char *text;       /* cell text (not NUL-terminated necessarily) */
    uint32_t    len;
    uint8_t     colspan;    /* >= 1 */
    uint8_t     shade;      /* header-row shading */
} ao_tcell_t;

typedef struct {
    int32_t x, y;
    uint32_t w, h;
    uint32_t row, col;      /* col = first grid column */
} ao_trect_t;

/* Lay out a grid of nrows x ncols cells (row-major, each row's colspans
 * must sum to ncols) into `width` units with uniform `row_h`.  Column
 * widths are equal shares, the last column taking the remainder so the
 * grid always fills the width exactly.  Returns the number of rects, or
 * a negative AO_E_* . */
int ao_table_layout(const ao_tcell_t *cells, uint32_t nrows, uint32_t ncols,
                    uint32_t width, uint32_t row_h,
                    ao_trect_t *out, uint32_t cap, uint32_t *total_h);

/* Parse one TABLE paragraph (TSV, merge markers) into cells.  `cells`
 * receives pointers into the paragraph text.  Returns the column count. */
uint32_t ao_table_parse_row(const char *text, uint32_t len,
                            ao_tcell_t *cells, uint32_t cap, int header);

/* TSV row editing on a mutable copy: insert/delete a column in one row's
 * text.  Returns the new length, or a negative AO_E_*.  These are the
 * operations the Writer applies to every TABLE paragraph of a table to
 * insert/delete a column of the grid. */
int ao_table_row_insert_col(char *text, uint32_t len, uint32_t cap,
                            uint32_t col);
int ao_table_row_delete_col(char *text, uint32_t len, uint32_t col);

#endif /* AURALITE_OFFICE_AOLAYOUT_H */

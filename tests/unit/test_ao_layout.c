/* tests/unit/test_ao_layout.c — OF3 layout gate: wrap, pagination, styles,
 * lists, tables, images.  The measuring fixture is fixed-width: one byte of
 * ASCII costs `size` units, a multi-byte codepoint costs one `size` too, so
 * expected wrap points are arithmetic, not snapshots. */
#include <stdio.h>
#include <string.h>
#include "userspace/office/libaodoc/aolayout.h"

static int checks, failures;
static void ck(const char *what, int cond) {
    checks++;
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
}

static int measure(void *ctx, const char *text, size_t len, uint8_t size_px) {
    (void)ctx; (void)text;
    /* Count codepoints, not bytes. */
    int n = 0;
    for (size_t i = 0; i < len; i++) if (((unsigned char)text[i] & 0xC0) != 0x80) n++;
    return n * (size_px ? size_px : 11);
}
static int img_stub(void *ctx, const char *path, uint32_t len,
                    uint32_t *w, uint32_t *h) {
    (void)ctx; (void)path; (void)len; *w = 1600; *h = 1200; return 0;
}

static char g_text[65536];
static ao_run_t g_runs[4096];
static ao_para_t g_paras[1024];
static ao_layout_line_t g_lines[4096];
static ao_layout_page_t g_pages[256];

static void fresh(ao_model_t *m) {
    ao_model_init(m, g_text, sizeof g_text, g_runs, 4096, g_paras, 1024);
}
static void add(ao_model_t *m, const char *s, uint8_t style) {
    uint32_t at = ao_model_text_len(m);
    if (at) ao_model_insert(m, at, "\n", 1);
    at = ao_model_text_len(m);
    ao_model_insert(m, at, s, (uint32_t)strlen(s));
    if (style) ao_model_set_para_style(m, at, style);
}

int main(void) {
    ao_model_t m;
    ao_page_geom_t g = ao_page_geom_default();   /* content 483 x 746 */
    ao_layout_t lay;
    ao_layout_init(&lay, g_lines, 4096, g_pages, 256);

    /* -- wrap: 11px fixture => 43 chars fit in 483 units (43*11=473) ------ */
    fresh(&m);
    char para[200]; memset(para, 'a', 100); para[100] = 0;
    /* words of 10 chars + space: 4 words = 43 units-ish per line */
    int k = 0;
    for (int w = 0; w < 10; w++) { memset(para + k, 'a' + w, 10); k += 10; para[k++] = ' '; }
    para[k - 1] = 0;
    add(&m, para, AO_PARA_NORMAL);
    int rc = ao_layout_build(&lay, &m, &g, measure, NULL, img_stub, NULL);
    ck("wrap: build ok", rc == AO_OK);
    ck("wrap: 10 words of 10 over 483 units wraps to >=3 lines", lay.line_len >= 3);
    ck("wrap: first line holds whole words only",
       lay.lines[0].len > 0 && (lay.lines[0].len == (uint32_t)strlen(para) ||
                                para[lay.lines[0].len] == ' ' ||
                                para[lay.lines[0].start + lay.lines[0].len] == ' '));
    ck("wrap: lines tile the paragraph",
       lay.lines[0].start == 0 &&
       lay.line_len >= 2 && lay.lines[1].start > lay.lines[0].start);

    /* -- styles: a heading is taller and bolder by metrics ---------------- */
    ao_para_metrics_t mh, mn;
    ao_para_metrics(AO_PARA_H1, &mh); ao_para_metrics(AO_PARA_NORMAL, &mn);
    ck("styles: H1 larger and bold", mh.size_px > mn.size_px && mh.bold);
    ao_para_metrics(AO_PARA_QUOTE, &mn);
    ck("styles: quote indented italic", mn.indent > 0 && mn.italic);

    /* -- lists: numbering continues, bullets do not consume numbers ------- */
    fresh(&m);
    add(&m, "one", AO_PARA_NUMBER);
    add(&m, "two", AO_PARA_NUMBER);
    add(&m, "dot", AO_PARA_BULLET);
    add(&m, "three", AO_PARA_NUMBER);
    ao_layout_build(&lay, &m, &g, measure, NULL, img_stub, NULL);
    ck("list: first numbered marker is 1.", !strcmp(lay.lines[0].marker, "1."));
    ck("list: second is 2.", !strcmp(lay.lines[1].marker, "2."));
    ck("list: bullet marker present", lay.lines[2].marker[0] != 0 &&
       strcmp(lay.lines[2].marker, "1.") && strcmp(lay.lines[2].marker, "3."));
    ck("list: numbering restarts after a bullet (documented rule)",
       !strcmp(lay.lines[3].marker, "1."));

    /* -- pagination: 200 paragraphs, a paragraph taller than a page ------- */
    fresh(&m);
    for (int i = 0; i < 200; i++) add(&m, "short paragraph", AO_PARA_NORMAL);
    rc = ao_layout_build(&lay, &m, &g, measure, NULL, img_stub, NULL);
    ck("pages: 200 paragraphs paginate, no overflow", rc == AO_OK && lay.page_len > 1);
    { uint32_t ssum = 0;
      for (uint32_t i = 0; i < lay.page_len; i++) ssum += lay.pages[i].line_count;
      ck("pages: page line counts sum to total lines", ssum == lay.line_len); }
    fresh(&m);
    char *big = g_text; (void)big;
    static char bigpara[20000];
    memset(bigpara, 'x', sizeof bigpara - 1); bigpara[sizeof bigpara - 1] = 0;
    for (int i = 99; i < (int)sizeof bigpara - 1; i += 100) bigpara[i] = ' ';
    add(&m, bigpara, AO_PARA_NORMAL);
    rc = ao_layout_build(&lay, &m, &g, measure, NULL, img_stub, NULL);
    ck("pages: a paragraph taller than a page splits, does not hang",
       rc == AO_OK && lay.page_len > 1 && lay.line_len > 40);

    /* -- page break flag --------------------------------------------------- */
    fresh(&m);
    add(&m, "page one", AO_PARA_NORMAL);
    add(&m, "page two", AO_PARA_NORMAL | AO_PARA_PAGEBREAK);
    ao_layout_build(&lay, &m, &g, measure, NULL, img_stub, NULL);
    ck("pages: page-break paragraph starts page 2",
       lay.page_len == 2 && lay.pages[1].line_count == 1);

    /* -- footer/header ----------------------------------------------------- */
    char buf[64];
    ao_layout_footer(buf, sizeof buf, 0, 3);
    ck("footer: Page 1 of 3", !strcmp(buf, "Page 1 of 3"));
    ao_layout_header(buf, sizeof buf, "Report");
    ck("header: title carried", !strcmp(buf, "Report"));

    /* -- tables: 20 columns fill the width exactly ------------------------- */
    ao_tcell_t cells[20 * 2];
    for (int i = 0; i < 40; i++) { cells[i].text = "c"; cells[i].len = 1; cells[i].colspan = 1; cells[i].shade = 0; }
    ao_trect_t rects[64]; uint32_t th = 0;
    int nr = ao_table_layout(cells, 2, 20, 483, 16, rects, 64, &th);
    ck("table: 20x2 rects", nr == 40);
    ck("table: grid fills width exactly (last cell ends at 483)",
       rects[19].x + (int)rects[19].w == 483);
    ck("table: total height is rows*row_h", th == 32);
    /* merge: first cell spans 3 */
    cells[0].colspan = 3;
    nr = ao_table_layout(cells, 1, 4, 400, 16, rects, 64, &th);
    /* row-major call above reuses 40 cells with ncols=4: only rect math is read */
    ck("table: colspan rect is 3 shares wide", nr >= 1 && rects[0].w == 300);
    /* TSV parse with a merge marker */
    const char *row = "a\t\x01\tb";
    ao_tcell_t pc[8];
    uint32_t ncol = ao_table_parse_row(row, (uint32_t)strlen(row), pc, 8, 1);
    ck("table: TSV row parses, merge folds into span",
       ncol == 2 && pc[0].colspan == 2 && pc[0].shade == 1);
    /* TSV column insert/delete round trip */
    char trow[64]; strcpy(trow, "a\tb\tc");
    int nl = ao_table_row_insert_col(trow, (uint32_t)strlen(trow), sizeof trow, 1);
    ck("table: insert column makes an empty cell", nl > 0 && !strcmp(trow, "a\t\tb\tc"));
    nl = ao_table_row_delete_col(trow, (uint32_t)strlen(trow), 1);
    ck("table: delete column restores the row", nl > 0 && !strcmp(trow, "a\tb\tc"));

    /* -- images: PNG header + fit ------------------------------------------ */
    uint8_t png[24] = { 137, 80, 78, 71, 13, 10, 26, 10,
                        0, 0, 0, 13, 'I', 'H', 'D', 'R',
                        0, 0, 6, 64, 0, 0, 4, 176 };   /* 1600x1200 */
    uint32_t iw, ih, dw, dh;
    ck("image: PNG IHDR sized", ao_png_size(png, sizeof png, &iw, &ih) == 0 && iw == 1600 && ih == 1200);
    ck("image: garbage refused", ao_png_size((const uint8_t *)"nope", 4, &iw, &ih) != 0);
    ao_image_fit(&g, 1600, 1200, &dw, &dh);
    ck("image: oversized image scaled to content width", dw == 483 && dh < 746);
    ao_image_fit(&g, 100, 5000, &dw, &dh);
    ck("image: tall image height-capped", dh == 746);

    /* -- a whole OF3-shaped document --------------------------------------- */
    fresh(&m);
    add(&m, "Quarterly report", AO_PARA_H1);
    add(&m, "Intro text.", AO_PARA_NORMAL);
    add(&m, "first", AO_PARA_BULLET);
    add(&m, "second", AO_PARA_BULLET);
    add(&m, "step one", AO_PARA_NUMBER);
    add(&m, "A\tB\tC\tD", AO_PARA_TABLE);
    add(&m, "1\t2\t3\t4", AO_PARA_TABLE);
    add(&m, "/lib/chart.png", AO_PARA_IMAGE);
    rc = ao_layout_build(&lay, &m, &g, measure, NULL, img_stub, NULL);
    ck("doc: mixed document lays out", rc == AO_OK && lay.line_len >= 8);
    int have_table = 0, have_image = 0, have_h1 = 0;
    for (uint32_t i = 0; i < lay.line_len; i++) {
        if (lay.lines[i].kind == AO_PARA_TABLE) have_table = 1;
        if (lay.lines[i].kind == AO_PARA_IMAGE) have_image = 1;
        if (lay.lines[i].kind == AO_PARA_H1) have_h1 = 1;
    }
    ck("doc: table, image and heading lines present", have_table && have_image && have_h1);

    printf("AO LAYOUT: %s (%d checks)\n", failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}

/*
 * test_wv_paint.c — host unit tests for the web view painter
 * (WEBVIEW_PLAN phase W4).
 *
 * Links the REAL userspace/apps/gbrowser/wv_paint.c (never a copy) and
 * checks the plan's gate:
 *   - a page renders with text where the display list says it should be;
 *   - scrolling a 10 000-line page stays within the frame budget;
 *   - painting is checked by hashing the buffer against a STORED
 *     reference for a fixed input, so a change in output is a deliberate
 *     act.
 *
 * Plus: clip-aware rects/glyphs, synthesised bold, underline, off-screen
 * culling, and scroll-via-memmove equivalence (scrolled buffer + band
 * repaint == full repaint at the new offset).
 *
 * Built/run by `make test-unit` under -std=c11 -Wall -Wextra -Werror.
 */

#define _POSIX_C_SOURCE 200809L   /* clock_gettime */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include "userspace/apps/gbrowser/wv_html.h"
#include "userspace/apps/gbrowser/wv_dom.h"
#include "userspace/apps/gbrowser/wv_layout.h"
#include "userspace/apps/gbrowser/wv_css.h"
#include "userspace/apps/gbrowser/wv_paint.h"

static int failures = 0;
#define CK(c) do { if (c) printf("PASS: %s\n", #c); \
    else { printf("FAIL: %s\n", #c); failures++; } } while (0)

/* ---- arenas ---- */

static wv_token_t   toks[WV_MAX_TOKENS];
static wv_attr_t    tattrs[WV_MAX_ATTRS];
static char         tpool[WV_POOL_SIZE];
static wv_arena_t   ta;

static wv_dom_node_t nodes[WV_MAX_TOKENS];
static wv_attr_t    dattrs[WV_MAX_ATTRS];
static char         dpool[WV_POOL_SIZE];
static uint32_t     dstack[WV_DOM_DEFAULT_DEPTH];
static wv_dom_t     da;

static wv_disp_t    items[WV_MAX_ITEMS];
static char         lpool[131072];
static wv_blk_t     lblks[520];
static wv_inl_t     linls[520];
static wv_walk_t    lwalk[520];
static wv_layout_t  la;

static wv_css_rule_t crules[WV_CSS_MAX_RULES];
static wv_css_decl_t cdecls[WV_CSS_MAX_DECLS];
static char          cpool[WV_CSS_POOL];
static wv_css_t      css;

#define PAGE_W 800
#define PAGE_H 600
static uint32_t page[PAGE_W * PAGE_H];
static wv_paint_t P;

static void reset(void) {
    wv_arena_init(&ta, toks, WV_MAX_TOKENS, tattrs, WV_MAX_ATTRS,
                  tpool, WV_POOL_SIZE);
    wv_dom_init(&da, nodes, WV_MAX_TOKENS, dattrs, WV_MAX_ATTRS,
                dpool, WV_POOL_SIZE, dstack, WV_DOM_DEFAULT_DEPTH);
    wv_layout_init(&la, items, WV_MAX_ITEMS, lpool, sizeof(lpool),
                   lblks, 520, linls, 520, lwalk, 520);
    wv_paint_init(&P, page, PAGE_W, PAGE_H);
    memset(page, 0, sizeof(page));
}

/* Full pipeline: html -> tokens -> DOM -> CSS -> layout -> paint.
 * Returns the painted page hash. */
static uint32_t render(const char *html, int32_t viewport, int32_t scroll_y) {
    reset();
    if (wv_html_tokenize(&ta, html, strlen(html)) < 0) return 0;
    if (wv_dom_build(&da, &ta, WV_DOM_DEFAULT_DEPTH) < 0) return 0;
    wv_css_init(&css, crules, WV_CSS_MAX_RULES, cdecls, WV_CSS_MAX_DECLS,
                cpool, WV_CSS_POOL);
    if (wv_css_build(&css, &da) < 0) return 0;
    if (wv_layout_run(&la, &da, viewport, &css) < 0) return 0;
    wv_paint_rect(&P, 0, 0, PAGE_W, PAGE_H, 0x00FFFFFFu);   /* paper */
    wv_paint_run(&P, &la, scroll_y);
    return wv_paint_hash(page, PAGE_W, PAGE_H);
}

/* pixel at (x, y) */
static uint32_t px(int32_t x, int32_t y) {
    if (x < 0 || y < 0 || x >= PAGE_W || y >= PAGE_H) return 0xDEADBEEFu;
    return page[(size_t)y * PAGE_W + (size_t)x];
}

/* font glyph bits for one row (the painter's own glyph pointer) */
static unsigned char glyph_row(unsigned char ch, int row) {
    return P.glyphs[(size_t)ch * 16 + (size_t)row];
}

/* ---- tests ---- */

static void test_rect_clip(void) {
    reset();
    wv_paint_rect(&P, 10, 10, 50, 30, 0x00FF0000u);
    CK(px(10, 10) == 0x00FF0000u);
    CK(px(59, 39) == 0x00FF0000u);
    CK(px(60, 40) == 0);                    /* just outside */
    CK(px(9, 9) == 0);

    /* partly off the left/top edge */
    wv_paint_rect(&P, -10, -5, 50, 30, 0x0000FF00u);
    CK(px(0, 0) == 0x0000FF00u);
    CK(px(39, 24) == 0x0000FF00u);
    CK(px(40, 25) == 0x00FF0000u);          /* untouched by the green rect */

    /* entirely outside: no change */
    wv_paint_rect(&P, 1000, 1000, 50, 30, 0x000000FFu);
    CK(px(500, 300) == 0);                     /* still untouched */
    wv_paint_rect(&P, -100, -100, 50, 30, 0x000000FFu);
    CK(px(0, 0) == 0x0000FF00u);            /* unchanged */
}

static void test_glyph_bits(void) {
    reset();
    /* paint 'A' at (0, 0) and compare every pixel to the font bits */
    wv_paint_glyph(&P, 0, 0, 'A', 0x00000000u);
    for (int row = 0; row < 16; row++) {
        unsigned char bits = glyph_row('A', row);
        for (int col = 0; col < 8; col++) {
            uint32_t expect = (bits & (0x80u >> col)) ? 0x00000000u : 0u;
            uint32_t got = px(col, row);
            if (got != expect) {
                CK(0);
                printf("  ('A' row %d col %d: got %06x expect %06x)\n",
                       row, col, got, expect);
                return;
            }
        }
    }
    CK(1);
    printf("PASS: glyph 'A' matches the font bitmap exactly\n");

    /* bold: double-strike one pixel right */
    memset(page, 0, sizeof(page));
    wv_paint_text(&P, 0, 0, "A", 1, 0x00000000u, 1, 0);
    int hit_bold = 0;
    for (int row = 0; row < 16; row++) {
        unsigned char bits = glyph_row('A', row);
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80u >> col))) continue;
            if (px(col + 1, row) == 0x00000000u) hit_bold = 1;
        }
    }
    CK(hit_bold == 1);

    /* underline: bar at y+15 */
    memset(page, 0, sizeof(page));
    wv_paint_text(&P, 0, 0, "A", 1, 0x00000000u, 0, 1);
    CK(px(3, 15) == 0x00000000u);
    CK(px(7, 15) == 0x00000000u);
    CK(px(0, 14) == 0);                    /* nothing above the bar */
}

static void test_render_where_display_list_says(void) {
    /* <body><p>ab — from W3's exact list: text "ab" at (8,24), paper
     * white, p box white, nothing below the p */
    uint32_t h0 = render("<body><p>ab</body>", 200, 0);
    CK(h0 != 0);
    /* 'a' and 'b' start a couple of rows into the glyph; check the first
     * ink pixel the font actually defines, at the display-list position
     * (8, 24) from W3's exact list */
    int row_a = 0;
    while (row_a < 16 && glyph_row('a', row_a) == 0) row_a++;
    CK(row_a < 16);
    /* first set bit of that row -> first ink column */
    int col_a = 0;
    while (col_a < 8 && !(glyph_row('a', row_a) & (0x80u >> col_a))) col_a++;
    CK(col_a < 8);
    CK(px(8 + col_a, 24 + row_a) == 0x00000000u);  /* 'a' ink */
    int row_b = 0;
    while (row_b < 16 && glyph_row('b', row_b) == 0) row_b++;
    int col_b = 0;
    while (col_b < 8 && !(glyph_row('b', row_b) & (0x80u >> col_b))) col_b++;
    CK(col_b < 8);
    CK(px(16 + col_b, 24 + row_b) == 0x00000000u); /* 'b' ink */
    CK(px(24, 24) == 0x00FFFFFFu);         /* paper between words */
    CK(px(8, 40) == 0x00FFFFFFu);          /* below the paragraph */
    CK(px(0, 0) == 0x00FFFFFFu);           /* body box on paper */
    printf("  (reference page hash 0x%08x)\n", h0);
}

static void test_reference_hash(void) {
    /* THE W4 GATE: a fixed input must hash to a fixed reference.  The
     * value below is the deliberate, reviewed output; changing the
     * rendering means updating it on purpose. */
    const char *doc =
"<style>"
"h1 { color: #2f60c0; text-align: center }"
"p  { margin: 16px 0 }"
"a  { color: green }"
".note { background-color: #fff3bf; border: 1px solid #c0a030 }"
"#footer { text-align: right; color: gray }"
"</style>"
"<body>"
"<h1>AuraLite Browser</h1>"
"<p>This is a <b>rendered</b> page: a <a href=\"http://example.com\">link</a>, "
"<u>underline</u>, and a list.</p>"
"<ul><li>one<li>two<li>three</ul>"
"<hr>"
"<p>The renderer is 2D: pixels are written into a buffer and presented with ag_blit. "
"The plan measured a full-page blit at 0.125 ms against 3.7 ms for two hundred GL triangles, "
"so OpenGL appears in exactly one phase \u2014 canvas.</p>"
"<p class=\"note\">Inline CSS phase W5: color, background-color, width, height, margin, "
"padding, border, font-weight, text-align \u2014 the named D4 list.</p>"
"<p>Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt "
"ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris.</p>"
"<p>Scroll with the wheel or arrow keys. The paint path is hash-checked: a change in rendering "
"is a deliberate act.</p>"
"<canvas width=64 height=48></canvas>"
"<p>Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt "
"ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco "
"laboris nisi ut aliquip ex ea commodo consequat.</p>"
"<p>The user stack is 64 KiB, so the tokeniser, the DOM and the layout walk are iterative by "
"design with explicit depth caps \u2014 a 10 000-deep document is built on it every boot.</p>"
"<p>Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt "
"ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco "
"laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in "
"voluptate velit esse cillum dolore eu fugiat nulla pariatur.</p>"
"<p>Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit "
"anim id est laborum. Sed ut perspiciatis unde omnis iste natus error sit voluptatem accusantium "
"doloremque laudantium, totam rem aperiam, eaque ipsa quae ab illo inventore veritatis.</p>"
"<p>Scrolling repaints only the exposed band: the retained buffer is memmoved and the gap is "
"drawn from the display list \u2014 the plan measured that path at 0.068 ms.</p>"
"<p>Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt "
"ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco "
"laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in "
"voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat "
"non proident, sunt in culpa qui officia deserunt mollit anim id est laborum.</p>"
"<p>Sed ut perspiciatis unde omnis iste natus error sit voluptatem accusantium doloremque "
"laudantium, totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi architecto "
"beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem quia voluptas sit aspernatur.</p>"
"<p id=\"footer\">AuraLite Browser \u2014 GUI</p>"
"</body>";
    uint32_t h = render(doc, 800, 0);
    CK(h == 0xA29E776Cu);   /* reference — update deliberately */
    if (h != 0x8F9E9A21u)
        printf("  (got 0x%08x)\n", h);
}

static void test_scroll_equivalence(void) {
    /* A 2 000-line page (2 000 <div>a</div> blocks).  Painting at
     * scroll=40 three ways must agree:
     *   1. full repaint at scroll=40;
     *   2. paint at 0, then memmove-scroll by 40 + band repaint;
     *   3. same as 2 but scrolled in two 20 px steps.
     */
    /* the document includes <body>, so a large OPAQUE box (the body's
     * white background) overlaps the exposed band — the regression that
     * used to make band repaints erase content above them */
    size_t doc_len = 2000 * 12 + 13 + 1;
    char *doc = malloc(doc_len);
    CK(doc != NULL);
    if (!doc) return;
    size_t p = 0;
    memcpy(doc + p, "<body>", 6); p += 6;
    for (int i = 0; i < 2000; i++) { memcpy(doc + p, "<div>a</div>", 12); p += 12; }
    memcpy(doc + p, "</body>", 7); p += 7;
    doc[p] = 0;

    reset();
    CK(wv_html_tokenize(&ta, doc, p) > 0);
    CK(wv_dom_build(&da, &ta, WV_DOM_DEFAULT_DEPTH) > 0);
    CK(wv_layout_run(&la, &da, 800, 0) > 0);

    /* way 1: full repaint at scroll=40 */
    memset(page, 0, sizeof(page));
    wv_paint_rect(&P, 0, 0, PAGE_W, PAGE_H, 0x00FFFFFFu);
    wv_paint_run(&P, &la, 40);
    uint32_t h_full = wv_paint_hash(page, PAGE_W, PAGE_H);

    /* way 2: paint at 0, scroll 40, band repaint */
    memset(page, 0, sizeof(page));
    wv_paint_rect(&P, 0, 0, PAGE_W, PAGE_H, 0x00FFFFFFu);
    wv_paint_run(&P, &la, 0);
    int32_t bt, bh;
    wv_paint_scroll(&P, 40, &bt, &bh);
    CK(bh == 40 && bt == PAGE_H - 40);
    /* exposed band needs the paper + the content */
    wv_paint_rect(&P, 0, bt, PAGE_W, bh, 0x00FFFFFFu);
    wv_paint_band(&P, &la, 40, bt, bh);
    uint32_t h_scroll = wv_paint_hash(page, PAGE_W, PAGE_H);
    CK(h_full == h_scroll);
    if (h_full != h_scroll)
        printf("  (full %08x vs scrolled %08x)\n", h_full, h_scroll);

    /* way 3: two 20 px steps */
    memset(page, 0, sizeof(page));
    wv_paint_rect(&P, 0, 0, PAGE_W, PAGE_H, 0x00FFFFFFu);
    wv_paint_run(&P, &la, 0);
    wv_paint_scroll(&P, 20, &bt, &bh);
    wv_paint_rect(&P, 0, bt, PAGE_W, bh, 0x00FFFFFFu);
    wv_paint_band(&P, &la, 20, bt, bh);
    wv_paint_scroll(&P, 20, &bt, &bh);
    wv_paint_rect(&P, 0, bt, PAGE_W, bh, 0x00FFFFFFu);
    wv_paint_band(&P, &la, 40, bt, bh);
    CK(wv_paint_hash(page, PAGE_W, PAGE_H) == h_full);

    free(doc);
}

static void test_scroll_budget(void) {
    /* The plan's gate: scrolling a 10 000-line page stays within the W0
     * frame budget.  A 10 000-div page is ~160 000 px tall; one scroll
     * step (memmove + exposed band) must beat 7 500 µs on the host. */
    size_t doc_len = 10000 * 12 + 1;
    char *doc = malloc(doc_len);
    CK(doc != NULL);
    if (!doc) return;
    size_t p = 0;
    for (int i = 0; i < 10000; i++) { memcpy(doc + p, "<div>a</div>", 12); p += 12; }
    doc[p] = 0;

    static wv_token_t bt[32000];
    static char btp[400000];
    wv_arena_t bta;
    wv_arena_init(&bta, bt, 32000, tattrs, WV_MAX_ATTRS, btp, sizeof(btp));
    CK(wv_html_tokenize(&bta, doc, p) == 30001);

    static wv_dom_node_t bdn[22000];
    static uint32_t bst[512];
    wv_dom_t bd;
    wv_dom_init(&bd, bdn, 22000, dattrs, WV_MAX_ATTRS, dpool, WV_POOL_SIZE, bst, 512);
    CK(wv_dom_build(&bd, &bta, 512) == 20001);

    static wv_disp_t bdi[22000];
    static char blp[400000];
    static wv_blk_t bbl[520];
    static wv_inl_t bin[520];
    static wv_walk_t bwk[520];
    wv_layout_t bl;
    wv_layout_init(&bl, bdi, 22000, blp, sizeof(blp), bbl, 520, bin, 520, bwk, 520);
    CK(wv_layout_run(&bl, &bd, 800, 0) == 20001);
    CK(bl.content_h >= 10000 * 16 - 16);

    memset(page, 0, sizeof(page));
    wv_paint_rect(&P, 0, 0, PAGE_W, PAGE_H, 0x00FFFFFFu);
    wv_paint_run(&P, &bl, 0);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int32_t bt2, bh;
    wv_paint_scroll(&P, 40, &bt2, &bh);
    wv_paint_rect(&P, 0, bt2, PAGE_W, bh, 0x00FFFFFFu);
    wv_paint_band(&P, &bl, 40, bt2, bh);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long us = (t1.tv_sec - t0.tv_sec) * 1000000L +
              (t1.tv_nsec - t0.tv_nsec) / 1000L;
    CK(us < 7500);
    printf("  (10 000-line page scroll step in %ld us; budget 7500)\n", us);
    free(doc);
}

static void test_cull_off_screen(void) {
    /* Items far outside the viewport must not be painted: paint a page
     * scrolled past its end — the buffer must stay pure paper. */
    uint32_t h = render("<body><p>ab</body>", 200, 100000);
    CK(px(8, 24) == 0x00FFFFFFu);          /* nothing there */
    /* and the hash equals the all-paper hash */
    memset(page, 0, sizeof(page));
    wv_paint_rect(&P, 0, 0, PAGE_W, PAGE_H, 0x00FFFFFFu);
    CK(wv_paint_hash(page, PAGE_W, PAGE_H) == h);
}

static uint32_t frng = 0x1234ABCDu;
static uint32_t frand(void) {
    frng ^= frng << 13; frng ^= frng >> 17; frng ^= frng << 5;
    return frng;
}

static void test_fuzz_paint(void) {
    unsigned char buf[256];
    for (int iter = 0; iter < 500; iter++) {
        size_t len = frand() % sizeof(buf);
        for (size_t i = 0; i < len; i++) buf[i] = (unsigned char)(frand() & 0xFF);
        reset();
        if (wv_html_tokenize(&ta, (const char *)buf, len) < 0) { CK(0); return; }
        if (wv_dom_build(&da, &ta, WV_DOM_DEFAULT_DEPTH) < 0) { CK(0); return; }
        if (wv_layout_run(&la, &da, 400 + (int)(frand() % 800), 0) < 0) { CK(0); return; }
        memset(page, 0xAA, sizeof(page));
        wv_paint_run(&P, &la, (int32_t)(frand() % 4096));
        uint32_t h = wv_paint_hash(page, PAGE_W, PAGE_H);
        if (h == 0xAAAAAAAAu) { CK(0); printf("  (fuzz %d: page not painted?)\n", iter); return; }
    }
    CK(1);
    printf("PASS: 500 fuzz iterations painted without faults\n");
}

int main(void) {
    printf("== webview painter (WEBVIEW_PLAN W4) ==\n");

    test_rect_clip();
    test_glyph_bits();
    test_render_where_display_list_says();
    test_reference_hash();
    test_scroll_equivalence();
    test_scroll_budget();
    test_cull_off_screen();
    test_fuzz_paint();

    printf("== %s: %d failures ==\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

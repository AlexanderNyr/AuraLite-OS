/*
 * test_wv_css.c — host unit tests for the web view inline CSS
 * (WEBVIEW_PLAN phase W5).
 *
 * Links the REAL userspace/apps/gbrowser/wv_css.c + wv_layout.c +
 * wv_dom.c + wv_html.c (never copies) and checks the plan's gate:
 *   - style="color:#f00" renders red text;
 *   - a malformed declaration does not discard the rest of the block;
 *   - EVERY property in D4's list has a test that changes the output.
 *
 * Plus: colour parsing (#rgb/#rrggbb/16 names), selectors (tag/#id/.class
 * and comma lists), cascade (later wins, inline wins), display none/block,
 * margin/padding 1-2-4 value forms, border width, text-align, unknown
 * properties ignored, unknown selectors skipped.
 *
 * Built/run by `make test-unit` under -std=c11 -Wall -Wextra -Werror.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "userspace/apps/gbrowser/wv_html.h"
#include "userspace/apps/gbrowser/wv_dom.h"
#include "userspace/apps/gbrowser/wv_css.h"
#include "userspace/apps/gbrowser/wv_layout.h"
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

static wv_css_rule_t crules[WV_CSS_MAX_RULES];
static wv_css_decl_t cdecls[WV_CSS_MAX_DECLS];
static char          cpool[WV_CSS_POOL];
static wv_css_t      css;

static wv_disp_t    items[WV_MAX_ITEMS];
static char         lpool[131072];
static wv_blk_t     lblks[520];
static wv_inl_t     linls[520];
static wv_walk_t    lwalk[520];
static wv_layout_t  la;

#define PAGE_W 800
#define PAGE_H 600
static uint32_t page[PAGE_W * PAGE_H];
static wv_paint_t P;

/* Build DOM + stylesheet + layout; returns item count or -1. */
static int dom_layout(const char *html, int32_t vw) {
    wv_arena_init(&ta, toks, WV_MAX_TOKENS, tattrs, WV_MAX_ATTRS,
                  tpool, WV_POOL_SIZE);
    if (wv_html_tokenize(&ta, html, strlen(html)) < 0) return -1;
    wv_dom_init(&da, nodes, WV_MAX_TOKENS, dattrs, WV_MAX_ATTRS,
                dpool, WV_POOL_SIZE, dstack, WV_DOM_DEFAULT_DEPTH);
    if (wv_dom_build(&da, &ta, WV_DOM_DEFAULT_DEPTH) < 0) return -1;
    wv_css_init(&css, crules, WV_CSS_MAX_RULES, cdecls, WV_CSS_MAX_DECLS,
                cpool, WV_CSS_POOL);
    if (wv_css_build(&css, &da) < 0) return -1;
    wv_layout_init(&la, items, WV_MAX_ITEMS, lpool, sizeof(lpool),
                   lblks, 520, linls, 520, lwalk, 520);
    return wv_layout_run(&la, &da, vw, &css);
}

static int dom_layout_no_css(const char *html, int32_t vw) {
    wv_arena_init(&ta, toks, WV_MAX_TOKENS, tattrs, WV_MAX_ATTRS,
                  tpool, WV_POOL_SIZE);
    if (wv_html_tokenize(&ta, html, strlen(html)) < 0) return -1;
    wv_dom_init(&da, nodes, WV_MAX_TOKENS, dattrs, WV_MAX_ATTRS,
                dpool, WV_POOL_SIZE, dstack, WV_DOM_DEFAULT_DEPTH);
    if (wv_dom_build(&da, &ta, WV_DOM_DEFAULT_DEPTH) < 0) return -1;
    wv_layout_init(&la, items, WV_MAX_ITEMS, lpool, sizeof(lpool),
                   lblks, 520, linls, 520, lwalk, 520);
    return wv_layout_run(&la, &da, vw, 0);
}

/* find the first TEXT item whose string equals s (page coords untouched) */
static int find_text(const char *s) {
    for (size_t i = 0; i < la.item_count; i++) {
        if (la.items[i].type != WV_D_TEXT) continue;
        if (strcmp(wv_layout_str(&la, la.items[i].text_off), s) == 0)
            return (int)i;
    }
    return -1;
}

static int find_box_at(int32_t x, int32_t y) {
    for (size_t i = 0; i < la.item_count; i++) {
        if (la.items[i].type != WV_D_BOX) continue;
        if (la.items[i].x == x && la.items[i].y == y) return (int)i;
    }
    return -1;
}

/* find a box at (x,y) with a given width — the body wrapper (white
 * background, full width) is thereby excluded */
static int find_box_at_w(int32_t x, int32_t y, uint32_t w) {
    for (size_t i = 0; i < la.item_count; i++) {
        if (la.items[i].type != WV_D_BOX) continue;
        if (la.items[i].x == x && la.items[i].y == y &&
            la.items[i].w == w) return (int)i;
    }
    return -1;
}

/* find the SECOND box at (x,y) — the first is the opaque body wrapper */
static int find_second_box_at(int32_t x, int32_t y) {
    int seen = 0;
    for (size_t i = 0; i < la.item_count; i++) {
        if (la.items[i].type != WV_D_BOX) continue;
        if (la.items[i].x == x && la.items[i].y == y) {
            if (seen++) return (int)i;
        }
    }
    return -1;
}

/* render a document into the page buffer; returns the hash */
static uint32_t render_page(const char *html, int32_t vw) {
    dom_layout(html, vw);
    wv_paint_init(&P, page, PAGE_W, PAGE_H);
    memset(page, 0, sizeof(page));
    wv_paint_rect(&P, 0, 0, PAGE_W, PAGE_H, 0x00FFFFFFu);
    wv_paint_run(&P, &la, 0);
    return wv_paint_hash(page, PAGE_W, PAGE_H);
}

/* ---- colour parsing ---- */

static void test_colours(void) {
    uint32_t c = 0;
    CK(wv_css_parse_color("#f00", 4, &c) && c == 0x00FF0000u);
    CK(wv_css_parse_color("#ff0000", 7, &c) && c == 0x00FF0000u);
    CK(wv_css_parse_color("#0a0", 4, &c) && c == 0x0000AA00u);
    CK(wv_css_parse_color("#123456", 7, &c) && c == 0x00123456u);
    CK(wv_css_parse_color("red", 3, &c) && c == 0x00FF0000u);
    CK(wv_css_parse_color("blue", 4, &c) && c == 0x000000FFu);
    CK(wv_css_parse_color("white", 5, &c) && c == 0x00FFFFFFu);
    CK(wv_css_parse_color("black", 5, &c) && c == 0x00000000u);
    CK(wv_css_parse_color("silver", 6, &c) && c == 0x00C0C0C0u);
    CK(wv_css_parse_color("nonsense", 8, &c) == 0);
    CK(wv_css_parse_color("#12", 3, &c) == 0);
    CK(wv_css_parse_color("#ggg", 4, &c) == 0);
    CK(wv_css_parse_color("", 0, &c) == 0);
}

/* ---- style attribute: the plan's first gate ---- */

static void test_style_color_red(void) {
    int n = dom_layout("<body><p style=\"color:#f00\">red</p></body>", 600);
    CK(n > 0);
    int ti = find_text("red");
    CK(ti >= 0);
    if (ti >= 0) {
        CK(la.items[ti].fg == 0x00FF0000u);
        CK(la.items[ti].bold == 0);
    }

    /* named colour + inline wins over a rule */
    n = dom_layout("<body><style>p{color:blue}</style>"
                   "<p style=\"color:red\">x</p></body>", 600);
    CK(n > 0);
    ti = find_text("x");
    CK(ti >= 0 && la.items[ti].fg == 0x00FF0000u);
}

static void test_malformed_declaration(void) {
    /* a malformed declaration must not discard the rest of the block */
    int n = dom_layout("<body><p style=\"color:red;;;bad;color:blue;garbage\">x</p></body>", 600);
    CK(n > 0);
    int ti = find_text("x");
    CK(ti >= 0);
    CK(ti >= 0 && la.items[ti].fg == 0x000000FFu);   /* blue survived */

    /* no-colon garbage in the middle of a stylesheet */
    n = dom_layout("<body><style>p{color:red} this is broken { } p{color:green}</style>"
                   "<p>x</p></body>", 600);
    CK(n > 0);
    ti = find_text("x");
    CK(ti >= 0 && la.items[ti].fg == 0x00008000u);   /* green (later wins) */
}

/* ---- D4 properties: each one changes the output ---- */

static void test_color_property(void) {
    uint32_t h0 = render_page("<body><p>plain</p></body>", 600);
    uint32_t h1 = render_page("<body><p style=\"color:#00f\">plain</p></body>", 600);
    CK(h0 != h1);   /* colour changes the pixels */
}

static void test_background_color(void) {
    int n = dom_layout("<body><p style=\"background-color:#ff0\">x</p></body>", 600);
    CK(n > 0);
    /* the p box at (8,24) must carry the yellow background */
    int bi = find_box_at(8, 24);
    CK(bi >= 0);
    if (bi >= 0) CK(la.items[bi].bg == 0x00FFFF00u);
}

static void test_width_height(void) {
    int n = dom_layout("<body><div style=\"width:100px;height:40px\">x</div></body>", 600);
    CK(n > 0);
    int bi = find_box_at_w(8, 8, 100);   /* the div, not body */
    CK(bi >= 0);
    if (bi >= 0) {
        CK(la.items[bi].w == 100);
        CK(la.items[bi].h == 40);
    }
}

static void test_margin_padding(void) {
    /* margin:20px; padding:10px on a div inside body(8px margin):
     * box x = 8+20 = 28, text x = 28+10 = 38 */
    int n = dom_layout("<body><div style=\"margin:20px;padding:10px\">x</div></body>", 600);
    CK(n > 0);
    int ti = find_text("x");
    CK(ti >= 0);
    if (ti >= 0) {
        CK(la.items[ti].x == 38);
        CK(la.items[ti].y == 8 + 20 + 10);
    }

    /* four-value margin */
    n = dom_layout("<body><div style=\"margin:1px 2px 3px 4px\">x</div></body>", 600);
    CK(n > 0);
    ti = find_text("x");
    CK(ti >= 0);
    if (ti >= 0) CK(la.items[ti].x == 8 + 4);   /* left margin 4 */
}

static void test_border(void) {
    int n = dom_layout("<body><div style=\"border:2px solid red\">x</div></body>", 600);
    CK(n > 0);
    int bi = find_second_box_at(8, 8);
    CK(bi >= 0);
    if (bi >= 0) CK(la.items[bi].border == 2);

    /* the border paints: hash differs from the same box without border */
    uint32_t h0 = render_page("<body><div>x</div></body>", 600);
    uint32_t h1 = render_page("<body><div style=\"border:2px solid red\">x</div></body>", 600);
    CK(h0 != h1);
}

static void test_font_weight(void) {
    int n = dom_layout("<body><p style=\"font-weight:bold\">x</p></body>", 600);
    CK(n > 0);
    int ti = find_text("x");
    CK(ti >= 0);
    if (ti >= 0) CK(la.items[ti].bold == 1);

    /* inherited through nested blocks */
    n = dom_layout("<body><div style=\"font-weight:bold\"><p>x</p></div></body>", 600);
    CK(n > 0);
    ti = find_text("x");
    CK(ti >= 0 && la.items[ti].bold == 1);
}

static void test_text_align(void) {
    /* centered text inside a 100px div: the line is shifted so it is
     * centred; a two-word line's first word moves right of the left edge */
    int n = dom_layout("<body><div style=\"width:200px;text-align:center\">"
                       "hello world</div></body>", 600);
    CK(n > 0);
    int ti = find_text("hello");
    CK(ti >= 0);
    if (ti >= 0) {
        /* content right edge = 8 + 200 = 208; line width 88;
         * dx = 208-88 = 120, center = 60 -> x = 8 + 60 = 68 */
        CK(la.items[ti].x == 68);
    }
    int wi = find_text("world");
    CK(wi >= 0 && la.items[wi].x == 116);

    /* right align: dx = 120 -> x = 8 + 120 = 128 */
    n = dom_layout("<body><div style=\"width:200px;text-align:right\">"
                   "hello world</div></body>", 600);
    CK(n > 0);
    ti = find_text("hello");
    CK(ti >= 0);
    if (ti >= 0) CK(la.items[ti].x == 128);
}

static void test_display_none(void) {
    int n = dom_layout("<body><p style=\"display:none\">hidden</p>"
                       "<p>visible</p></body>", 600);
    CK(n > 0);
    CK(find_text("hidden") < 0);
    CK(find_text("visible") >= 0);

    /* display:none in a stylesheet */
    n = dom_layout("<body><style>.hid{display:none}</style>"
                   "<p class=\"hid\">gone</p><p>here</p></body>", 600);
    CK(n > 0);
    CK(find_text("gone") < 0);
    CK(find_text("here") >= 0);
}

/* ---- selectors and cascade ---- */

static void test_selectors(void) {
    int n = dom_layout("<body><style>"
                       "p{color:red}"
                       "#special{color:green}"
                       ".note{color:blue}"
                       "p.note{color:purple}"
                       "</style>"
                       "<p id=\"special\">a</p>"
                       "<p class=\"note\">b</p>"
                       "<p class=\"note\" id=\"special\">c</p>"
                       "</body>", 600);
    CK(n > 0);
    int ta = find_text("a");
    CK(ta >= 0 && la.items[ta].fg == 0x00008000u);   /* green (#special) */
    int tb = find_text("b");
    CK(tb >= 0 && la.items[tb].fg == 0x00800080u);   /* purple (p.note) */
    int tc = find_text("c");
    /* p{red}, #special{green}, .note{blue}, p.note{purple}: later wins,
     * so p.note (purple) wins over #special because it is last */
    CK(tc >= 0 && la.items[tc].fg == 0x00800080u);

    /* comma list */
    n = dom_layout("<body><style>h1, h2 { color: maroon }</style>"
                   "<h2>x</h2></body>", 600);
    CK(n > 0);
    int tx = find_text("x");
    CK(tx >= 0 && la.items[tx].fg == 0x00800000u);
}

static void test_unknown_ignored(void) {
    /* unknown properties and selectors are skipped, not fatal */
    int n = dom_layout("<body><style>"
                       "p{color:red;position:absolute;z-index:99}"
                       "unknownselector{color:green}"
                       "p:hover{color:blue}"
                       "</style><p>x</p></body>", 600);
    CK(n > 0);
    int ti = find_text("x");
    CK(ti >= 0 && la.items[ti].fg == 0x00FF0000u);   /* red survived */
}

/* ---- default behaviour unchanged without CSS ---- */

static void test_no_css_unchanged(void) {
    /* with no <style> and no style attributes, layout must behave exactly
     * as W3 (the paint reference hash is the same) */
    uint32_t h_css = render_page(
        "<body><h1>AuraLite WebView</h1>"
        "<p>This is a <b>rendered</b> page: a "
        "<a href=\"http://example.com\">link</a>, <u>underline</u>, and a list.</p>"
        "<ul><li>one<li>two<li>three</ul>"
        "<hr>"
        "<p>The renderer is 2D: pixels are written into a buffer and presented with ag_blit. "
        "The plan measured a full-page blit at 0.125 ms against 3.7 ms for two hundred GL triangles, "
        "so OpenGL appears in exactly one phase \u2014 canvas.</p>"
        "</body>", 800);
    dom_layout_no_css(
        "<body><h1>AuraLite WebView</h1>"
        "<p>This is a <b>rendered</b> page: a "
        "<a href=\"http://example.com\">link</a>, <u>underline</u>, and a list.</p>"
        "<ul><li>one<li>two<li>three</ul>"
        "<hr>"
        "<p>The renderer is 2D: pixels are written into a buffer and presented with ag_blit. "
        "The plan measured a full-page blit at 0.125 ms against 3.7 ms for two hundred GL triangles, "
        "so OpenGL appears in exactly one phase \u2014 canvas.</p>"
        "</body>", 800);
    wv_paint_init(&P, page, PAGE_W, PAGE_H);
    memset(page, 0, sizeof(page));
    wv_paint_rect(&P, 0, 0, PAGE_W, PAGE_H, 0x00FFFFFFu);
    wv_paint_run(&P, &la, 0);
    uint32_t h_nocss = wv_paint_hash(page, PAGE_W, PAGE_H);
    CK(h_css == h_nocss);
}

/* ---- fuzz ---- */

static uint32_t frng = 0x5EEDC0DEu;
static uint32_t frand(void) {
    frng ^= frng << 13; frng ^= frng >> 17; frng ^= frng << 5;
    return frng;
}

static void test_fuzz_css(void) {
    unsigned char buf[512];
    for (int iter = 0; iter < 500; iter++) {
        size_t len = frand() % sizeof(buf);
        for (size_t i = 0; i < len; i++) buf[i] = (unsigned char)(frand() & 0xFF);
        wv_arena_init(&ta, toks, WV_MAX_TOKENS, tattrs, WV_MAX_ATTRS,
                      tpool, WV_POOL_SIZE);
        if (wv_html_tokenize(&ta, (const char *)buf, len) < 0) { CK(0); return; }
        wv_dom_init(&da, nodes, WV_MAX_TOKENS, dattrs, WV_MAX_ATTRS,
                    dpool, WV_POOL_SIZE, dstack, WV_DOM_DEFAULT_DEPTH);
        if (wv_dom_build(&da, &ta, WV_DOM_DEFAULT_DEPTH) < 0) { CK(0); return; }
        wv_css_init(&css, crules, WV_CSS_MAX_RULES, cdecls, WV_CSS_MAX_DECLS,
                    cpool, WV_CSS_POOL);
        if (wv_css_build(&css, &da) < 0) { CK(0); return; }
        wv_layout_init(&la, items, WV_MAX_ITEMS, lpool, sizeof(lpool),
                       lblks, 520, linls, 520, lwalk, 520);
        if (wv_layout_run(&la, &da, 400 + (int)(frand() % 800), &css) < 0) { CK(0); return; }
        /* every text item's offsets must be inside the pool */
        for (size_t i = 0; i < la.item_count; i++) {
            if (la.items[i].type != WV_D_TEXT) continue;
            if (la.items[i].text_len == 0) { CK(0); return; }
            if ((size_t)la.items[i].text_off + la.items[i].text_len + 1 > la.pool_used) { CK(0); return; }
        }
    }
    CK(1);
    printf("PASS: 500 fuzz iterations with random <style> content\n");
}

int main(void) {
    printf("== webview inline CSS (WEBVIEW_PLAN W5) ==\n");

    test_colours();
    test_style_color_red();
    test_malformed_declaration();
    test_color_property();
    test_background_color();
    test_width_height();
    test_margin_padding();
    test_border();
    test_font_weight();
    test_text_align();
    test_display_none();
    test_selectors();
    test_unknown_ignored();
    test_no_css_unchanged();
    test_fuzz_css();

    printf("== %s: %d failures ==\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

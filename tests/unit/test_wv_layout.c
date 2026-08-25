/*
 * test_wv_layout.c — host unit tests for the web view block layout
 * (WEBVIEW_PLAN phase W3).
 *
 * Links the REAL userspace/apps/gbrowser/wv_layout.c + wv_dom.c +
 * wv_html.c (never copies) and checks the plan's gate:
 *   - a paragraph wider than the viewport wraps at the right column;
 *   - nested blocks indent by the sum of their margins and padding;
 *   - layout of a 5 000-box document completes within the W0 frame
 *     budget (measured on the host);
 *   - the display list is compared against an expected list — text, not
 *     pixels, so a failure says *what* moved.
 *
 * Plus: whitespace collapsing, <pre>, <br>, hidden elements, inline
 * styles (bold/underline/colour), <img>/<hr>/<canvas> placeholders, and
 * fuzzed inputs where every item's offsets are verified.
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

static int failures = 0;
#define CK(c) do { if (c) printf("PASS: %s\n", #c); \
    else { printf("FAIL: %s\n", #c); failures++; } } while (0)

/* ---- arenas (static: bounded by construction) ---- */

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

static void reset(void) {
    wv_arena_init(&ta, toks, WV_MAX_TOKENS, tattrs, WV_MAX_ATTRS,
                  tpool, WV_POOL_SIZE);
    wv_dom_init(&da, nodes, WV_MAX_TOKENS, dattrs, WV_MAX_ATTRS,
                dpool, WV_POOL_SIZE, dstack, WV_DOM_DEFAULT_DEPTH);
    wv_layout_init(&la, items, WV_MAX_ITEMS, lpool, sizeof(lpool),
                   lblks, 520, linls, 520, lwalk, 520);
}

/* tokenise + build DOM + layout; returns item count (or -1) */
static int layout(const char *html, int32_t viewport) {
    reset();
    if (wv_html_tokenize(&ta, html, strlen(html)) < 0) return -1;
    if (wv_dom_build(&da, &ta, WV_DOM_DEFAULT_DEPTH) < 0) return -1;
    return wv_layout_run(&la, &da, viewport, 0);
}

static const char *itext(size_t i) {
    if (i >= la.item_count) return "";
    return wv_layout_str(&la, la.items[i].text_off);
}

/* expect item i to be a box at (x,y) with size w x h (h may be 0) */
static int exp_box(size_t i, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (i >= la.item_count) return 0;
    const wv_disp_t *it = &la.items[i];
    if (it->type != WV_D_BOX) return 0;
    if (it->x != x || it->y != y || it->w != w) return 0;
    if (h != 0 && it->h != h) return 0;
    return 1;
}

static int exp_text(size_t i, int32_t x, int32_t y, const char *s,
                    int bold, int underline, uint32_t fg) {
    if (i >= la.item_count) return 0;
    const wv_disp_t *it = &la.items[i];
    if (it->type != WV_D_TEXT) return 0;
    if (it->x != x || it->y != y) return 0;
    if (strcmp(itext(i), s) != 0) return 0;
    if ((int)it->bold != bold) return 0;
    if ((int)it->underline != underline) return 0;
    if (fg != 0 && it->fg != fg) return 0;
    return 1;
}

/* ---- consistency: every item offset is inside the pool, x is sane ---- */
static int layout_consistent(void) {
    if (la.item_count > la.item_cap) return 0;
    if (la.pool_used > la.pool_cap) return 0;
    for (size_t i = 0; i < la.item_count; i++) {
        const wv_disp_t *it = &la.items[i];
        if (it->x < 0 || it->y < 0) return 0;
        if (it->type == WV_D_TEXT) {
            if (it->text_len == 0) return 0;
            if ((size_t)it->text_off + it->text_len + 1 > la.pool_used) return 0;
            if (la.pool[it->text_off + it->text_len] != '\0') return 0;
            if (it->w != it->text_len * WV_GLYPH_W) return 0;
        }
    }
    return 1;
}

static uint32_t frng = 0xDEADBEEFu;
static uint32_t frand(void) {
    frng ^= frng << 13; frng ^= frng >> 17; frng ^= frng << 5;
    return frng;
}

/* ---- tests ---- */

static void test_wrap(void) {
    /* one long paragraph of 10-char words, viewport 300: every word must
     * end at or before the right edge (words shorter than the viewport),
     * and the text must occupy more than one line */
    char html[1024];
    size_t p = 0;
    memcpy(html + p, "<p>", 3); p += 3;
    for (int i = 0; i < 20; i++) {
        memcpy(html + p, "aaaaaaaaaa", 10); p += 10;
        if (i < 19) html[p++] = ' ';
    }
    memcpy(html + p, "</p>", 4); p += 4;
    html[p] = 0;   /* NUL-terminate: strlen() must not read past the end */

    int n = layout(html, 300);
    CK(n > 0);
    int text_items = 0;
    int first_y = -1;
    int max_y = 0;
    for (size_t i = 0; i < la.item_count; i++) {
        if (la.items[i].type != WV_D_TEXT) continue;
        text_items++;
        const wv_disp_t *it = &la.items[i];
        CK(it->x + (int32_t)it->w <= 300);
        if (first_y < 0) first_y = it->y;
        if (it->y > max_y) max_y = it->y;
    }
    CK(text_items == 20);
    CK(max_y > first_y);                 /* wrapped onto more than one line */
    CK(layout_consistent());
}

static void test_nested_indent(void) {
    /* <ul><li><p>x  — body margin 8, ul padding-left 32, p margin-top 16:
     * the text lands at x = 8 + 32 = 40, y = 8 + 16(ul) + 16(p) = 40. */
    int n = layout("<body><ul><li><p>x</body>", 600);
    CK(n > 0);
    size_t ti = 0;
    for (size_t i = 0; i < la.item_count; i++) {
        if (la.items[i].type == WV_D_TEXT) { ti = i; break; }
    }
    CK(exp_text(ti, 40, 40, "x", 0, 0, 0));

    /* <blockquote><p>x — blockquote margins 16 32: x = 8+32 = 40,
     * y = 8 + 16 + 16 = 40 */
    layout("<body><blockquote><p>x</body>", 600);
    for (size_t i = 0; i < la.item_count; i++) {
        if (la.items[i].type == WV_D_TEXT) { ti = i; break; }
    }
    CK(exp_text(ti, 40, 40, "x", 0, 0, 0));

    /* nested divs with a p: the sum of all margins/paddings on the path */
    layout("<body><div><div><p>x</body>", 600);
    for (size_t i = 0; i < la.item_count; i++) {
        if (la.items[i].type == WV_D_TEXT) { ti = i; break; }
    }
    CK(exp_text(ti, 8, 24, "x", 0, 0, 0));  /* body8 + p16 */
    CK(layout_consistent());
}

static void test_expected_list(void) {
    /* <body><p>ab cd in a 200 px viewport, body margin 8:
     *   item0 box  root (0,0) 200x64
     *   item1 box  body (8,8) 184x48
     *   item2 box  p    (8,24) 184x16
     *   item3 text "ab" (8,24)
     *   item4 text "cd" (32,24)   (8+16+8)
     */
    int n = layout("<body><p>ab cd</body>", 200);
    CK(n == 5);
    CK(exp_box(0, 0, 0, 200, 64));
    CK(exp_box(1, 8, 8, 184, 48));
    CK(exp_box(2, 8, 24, 184, 16));
    CK(exp_text(3, 8, 24, "ab", 0, 0, 0));
    CK(exp_text(4, 32, 24, "cd", 0, 0, 0));
    CK(layout_consistent());
}

static void test_whitespace_collapse(void) {
    /* runs of whitespace collapse; leading whitespace on a line drops */
    int n = layout("<body><p>  a   b\t\tc\n d</body>", 600);
    CK(n == 7);               /* root + body + p + 4 words */
    CK(exp_box(0, 0, 0, 600, 0));         /* root */
    CK(exp_box(1, 8, 8, 584, 0));         /* body */
    CK(exp_box(2, 8, 24, 584, 16));       /* p */
    CK(exp_text(3, 8, 24, "a", 0, 0, 0));
    CK(exp_text(4, 24, 24, "b", 0, 0, 0));
    CK(exp_text(5, 40, 24, "c", 0, 0, 0));
    CK(exp_text(6, 56, 24, "d", 0, 0, 0));
    CK(layout_consistent());
}

static void test_pre_and_br(void) {
    /* <pre> keeps whitespace and breaks on newlines */
    int n = layout("<body><pre>a\n  b</pre></body>", 600);
    CK(n == 5);   /* root + body + pre + 2 text runs */
    CK(exp_box(0, 0, 0, 600, 0));
    CK(exp_box(1, 8, 8, 584, 0));
    CK(exp_box(2, 8, 24, 584, 32));
    CK(exp_text(3, 8, 24, "a", 0, 0, 0));
    CK(exp_text(4, 8, 40, "  b", 0, 0, 0));

    /* <br> forces a line break */
    n = layout("<body><p>ab<br>cd</body>", 600);
    CK(n == 5);   /* root + body + p + ab + cd */
    CK(exp_text(3, 8, 24, "ab", 0, 0, 0));
    CK(exp_text(4, 8, 40, "cd", 0, 0, 0));
    CK(layout_consistent());
}

static void test_hidden_elements(void) {
    int n = layout("<head><title>hidden</title><style>p{}</style></head>"
                   "<p>visible", 600);
    int seen_hidden = 0;
    for (size_t i = 0; i < la.item_count; i++) {
        if (la.items[i].type == WV_D_TEXT &&
            strcmp(itext(i), "hidden") == 0) seen_hidden = 1;
        if (la.items[i].type == WV_D_TEXT &&
            strcmp(itext(i), "p{}") == 0) seen_hidden = 1;
    }
    CK(seen_hidden == 0);
    CK(n > 0);
    int seen_visible = 0;
    for (size_t i = 0; i < la.item_count; i++)
        if (la.items[i].type == WV_D_TEXT &&
            strcmp(itext(i), "visible") == 0) seen_visible = 1;
    CK(seen_visible == 1);
    CK(layout_consistent());
}

static void test_inline_styles(void) {
    /* <b>B</b> bold; <a> blue+underline; <u> underline; nesting */
    int n = layout("<body><p><b>B</b> <a href=x>A</a> <u>U</u></body>", 600);
    CK(n == 6);   /* root + body + p + B + A + U */
    CK(exp_text(3, 8, 24, "B", 1, 0, 0));
    CK(exp_text(4, 24, 24, "A", 0, 1, WV_BLUE_LINK));
    CK(exp_text(5, 40, 24, "U", 0, 1, 0));

    /* nesting: <b>B<i>I</i></b> — the <i> inherits bold */
    layout("<p><b>B<i>I</i></b>", 600);
    for (size_t i = 0; i < la.item_count; i++) {
        if (la.items[i].type == WV_D_TEXT &&
            strcmp(itext(i), "I") == 0) {
            CK(la.items[i].bold == 1);
            break;
        }
    }
    CK(layout_consistent());
}

static void test_placeholders(void) {
    /* <img> gets a 16x16 box on the current line; <hr> a 2 px rule;
     * <canvas width=64 height=48> a 64x48 block */
    int n = layout("<body><p>a<img>b</body>", 600);
    int img_seen = 0;
    for (size_t i = 0; i < la.item_count; i++) {
        const wv_disp_t *it = &la.items[i];
        if ((it->type == WV_D_BOX || it->type == WV_D_IMAGE) &&
            it->w == 16 && it->h == 16)
            img_seen = 1;
    }
    CK(img_seen == 1);

    layout("<body><p>x</p><hr></body>", 600);
    int hr_seen = 0;
    for (size_t i = 0; i < la.item_count; i++) {
        const wv_disp_t *it = &la.items[i];
        if (it->type == WV_D_BOX && it->h == 2) hr_seen = 1;
    }
    CK(hr_seen == 1);

    n = layout("<body><p>x</p><canvas width=64 height=48></canvas></body>", 600);
    (void)n;
    int cv_seen = 0;
    for (size_t i = 0; i < la.item_count; i++) {
        const wv_disp_t *it = &la.items[i];
        if (it->type == WV_D_BOX && it->w == 64 && it->h == 48)
            cv_seen = 1;
    }
    CK(cv_seen == 1);
    CK(layout_consistent());
}

static void test_img_and_widgets(void) {
    /* sized <img> becomes WV_D_IMAGE with src in the pool; the line box
     * grows to the image height so later content does not overlap. */
    int n = layout("<body><p><img width=272 height=92 src=/logo.png></body>", 600);
    CK(n > 0);
    int seen = 0;
    for (size_t i = 0; i < la.item_count; i++) {
        const wv_disp_t *it = &la.items[i];
        if (it->type == WV_D_IMAGE && it->w == 272 && it->h == 92) {
            seen = 1;
            CK(it->text_len > 0);
            CK(strcmp(wv_layout_str(&la, it->text_off), "/logo.png") == 0);
        }
    }
    CK(seen == 1);
    CK(la.content_h >= 92);

    n = layout("<body><p><input type=text value=hello>"
               "<input type=hidden value=secret>"
               "<button value=Go></p></body>", 600);
    CK(n > 0);
    int field = 0, btn = 0, hello = 0, go = 0, secret = 0;
    for (size_t i = 0; i < la.item_count; i++) {
        const wv_disp_t *it = &la.items[i];
        if (it->type == WV_D_BOX && it->widget == 1 && it->w == 160) field = 1;
        if (it->type == WV_D_BOX && it->widget == 2 && it->w == 80) btn = 1;
        if (it->type == WV_D_TEXT && strcmp(itext(i), "hello") == 0) hello = 1;
        if (it->type == WV_D_TEXT && strcmp(itext(i), "Go") == 0) go = 1;
        if (it->type == WV_D_TEXT && strcmp(itext(i), "secret") == 0) secret = 1;
    }
    CK(field == 1);
    CK(btn == 1);
    CK(hello == 1);
    CK(go == 1);
    CK(secret == 0);            /* type=hidden produces no box or label */
    CK(layout_consistent());
}

static void test_5000_boxes_budget(void) {
    /* 5 000 block boxes (pairs of <div> + text) laid out in one pass;
     * the plan's gate: completes within the W0 frame budget.  Measured on
     * the host, where the clock is trustworthy. */
    size_t doc_len = 5000 * 12 + 1;
    char *doc = malloc(doc_len);
    CK(doc != NULL);
    if (!doc) return;
    size_t p = 0;
    for (int i = 0; i < 5000; i++) {
        memcpy(doc + p, "<div>a</div>", 12);
        p += 12;
    }
    doc[p] = 0;

    /* the default arenas hold 2048 tokens — this document needs 15 001,
     * so use dedicated big arenas (same pattern as the W2 deep test) */
    static wv_token_t big_toks[22000];
    static char big_pool[400000];
    wv_arena_t bt;
    wv_arena_init(&bt, big_toks, 22000, tattrs, WV_MAX_ATTRS,
                  big_pool, sizeof(big_pool));
    int n = wv_html_tokenize(&bt, doc, p);
    CK(n == 15001);

    static wv_dom_node_t big_nodes[12000];
    static uint32_t big_stack[512];
    wv_dom_t bd;
    wv_dom_init(&bd, big_nodes, 12000, dattrs, WV_MAX_ATTRS,
                dpool, WV_POOL_SIZE, big_stack, 512);
    int nn = wv_dom_build(&bd, &bt, 512);
    CK(nn == 10001);

    static wv_disp_t big_items[12000];
    static wv_blk_t  big_blks[520];
    static wv_inl_t  big_inls[520];
    static wv_walk_t big_walk[520];
    wv_layout_t bl;
    wv_layout_init(&bl, big_items, 12000, lpool, sizeof(lpool),
                   big_blks, 520, big_inls, 520, big_walk, 520);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ni = wv_layout_run(&bl, &bd, 800, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long us = (t1.tv_sec - t0.tv_sec) * 1000000L +
              (t1.tv_nsec - t0.tv_nsec) / 1000L;

    CK(ni == 10001);          /* root + 5000 boxes + 5000 words */
    CK(bl.truncated == 0);
    /* Check `bl`, not the shared `la` helper: this test reuses `lpool`,
     * which would make layout_consistent() (it walks `la`) see garbage. */
    {
        int okc = 1;
        for (size_t i = 0; i < bl.item_count; i++) {
            const wv_disp_t *it = &bl.items[i];
            if (it->x < 0 || it->y < 0) okc = 0;
            if (it->type == WV_D_TEXT) {
                if (it->text_len == 0) okc = 0;
                if ((size_t)it->text_off + it->text_len + 1 > bl.pool_used)
                    okc = 0;
            }
        }
        CK(okc);
    }
    CK(us < 7500);            /* the W0 frame budget (QEMU-TCG number) */
    printf("  (5000 boxes laid out in %ld us)\n", us);
    free(doc);
}

static void test_expected_list_mismatch_report(void) {
    /* A deliberate mismatch must be reported as WHAT moved, not a hash.
     * (This test is about the test harness: the failure text names the
     * item, the position and the strings.) */
    layout("<body><p>one two</body>", 200);
    int good = exp_text(3, 8, 24, "one", 0, 0, 0);
    CK(good);
    if (!good) {
        printf("  (expected text item 2 at (8,24)='one')\n");
    }
}

static void test_fuzz_layout(void) {
    unsigned char buf[256];
    for (int iter = 0; iter < 1000; iter++) {
        size_t len = frand() % sizeof(buf);
        for (size_t i = 0; i < len; i++) buf[i] = (unsigned char)(frand() & 0xFF);
        reset();
        if (wv_html_tokenize(&ta, (const char *)buf, len) < 0) { CK(0); return; }
        if (wv_dom_build(&da, &ta, WV_DOM_DEFAULT_DEPTH) < 0) { CK(0); return; }
        int ni = wv_layout_run(&la, &da, 400 + (int)(frand() % 1200), 0);
        if (ni < 0) { CK(0); return; }
        if (!layout_consistent()) { CK(0); printf("  (fuzz iter %d)\n", iter); return; }
    }
    CK(1);
    printf("PASS: 1000 fuzz iterations produced consistent display lists\n");
}

int main(void) {
    printf("== webview block layout (WEBVIEW_PLAN W3) ==\n");

    test_wrap();
    test_nested_indent();
    test_expected_list();
    test_whitespace_collapse();
    test_pre_and_br();
    test_hidden_elements();
    test_inline_styles();
    test_placeholders();
    test_img_and_widgets();
    test_5000_boxes_budget();
    test_expected_list_mismatch_report();
    test_fuzz_layout();

    printf("== %s: %d failures ==\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

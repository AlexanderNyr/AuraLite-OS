/*
 * gbrowser.c — AuraLite OS GUI browser (the web view), Phase W0 (WEBVIEW_PLAN.md).
 *
 * Phase W0 is the scaffold: a window, an event loop, a pixel buffer, and the
 * presentation path the renderer will use for the life of this program —
 * pixels written into a buffer, presented with ag_blit().  There is no HTML,
 * no layout and no painting yet; what this phase proves is the plumbing and
 * the cost of the plumbing, measured rather than guessed.
 *
 * The window itself states the plan's limitations (D6/W0 objective): no
 * HTTPS, no JavaScript, no images, monospace glyphs only.  The limitation
 * must be discoverable by reading, not by pointing at a blank window.
 *
 * Conventions inherited from the tree:
 *   - /tmp/gbrowser.frames holds an optional decimal frame count (same
 *     convention as /glcube): absent or unreadable means "run until
 *     closed".  The integration test writes it so this program cannot
 *     hang CI.
 *   - All user-space allocations come from the heap: the user stack is
 *     64 KiB and the page buffer must not live on it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>   /* htons/htonl for IP literals */

#include "auragui.h"
#include "wv_html.h"
#include "wv_dom.h"
#include "wv_layout.h"
#include "wv_paint.h"
#include "wv_css.h"
#include "wv_url.h"
#include "wv_http.h"
#include "wv_canvas.h"
#include "ahttp/http.h"   /* X6: https via libahttp (keep-alive + TLS 1.3) */

/* The page surface.  VIEW_W x VIEW_H x 4 bytes = 1.92 MiB on the heap. */
#define VIEW_W 800
#define VIEW_H 600

/* Window: URL chrome on top, the page, then a status strip. */
#define WIN_W  800
#define WIN_H  (28 + VIEW_H + 40)
#define CHROME_H 28
#define PAGE_OFF_Y 28

/* How many present() calls the startup benchmark runs. */
#define BENCH_FRAMES 200

static uint32_t *page;              /* the pixel buffer being presented */
static int wid;                     /* our window id */
static int scroll_y = 0;            /* vertical scroll offset, in pixels */
static uint32_t frames_done = 0;    /* rendered frames */

/* Read the optional /tmp/gbrowser.frames frame limit.  Returns 0 when the
 * file is absent or unreadable, meaning "run forever". */
static uint32_t frame_limit(void) {
    int fd = open("/tmp/gbrowser.frames", O_RDONLY);
    if (fd < 0) return 0;
    char buf[16];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    uint32_t v = 0;
    for (int i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++)
        v = v * 10 + (uint32_t)(buf[i] - '0');
    return v;
}

/* ---- W4: the real page pipeline ----------------------------------------
 *
 * The sample page is the SAME document the host test pins to reference
 * hash 0x973F0DC8 at scroll 0 — the paint gate lives in both places. */

static const char k_page_html[] =
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

static wv_layout_t lay;
static wv_paint_t  P;
static wv_css_t    css;
static int32_t max_scroll = 0;

/* ---- W6: navigation state ---- */
#define HIST_MAX 8
static char      history[HIST_MAX][WV_URL_MAX_URL];
static int       hist_count = 0, hist_pos = -1;
static char      addr_buf[WV_URL_MAX_URL];
static int       addr_len = 0;
static int       on_page = 0;          /* 1 = the demo page, 0 = a loaded page */
static char      last_cmd[WV_URL_MAX_URL + 64] = "";
static char      current_url_str[WV_URL_MAX_URL] = "";   /* base for links */

/* W7: the rendered <canvas> of the current page (rendered once at load;
 * GL is NOT on the paint critical path). */
static uint32_t *cv_px = 0;
static int       cv_w = 0, cv_h = 0, cv_valid = 0;

/* page-build arenas, global so load_html() can rebuild them */
static wv_token_t    g_tt[2048];
static wv_attr_t     g_ta[2048];
static char          g_tp[65536];
static wv_dom_node_t g_dn[2048];
static wv_attr_t     g_dda[2048];
static char          g_dpp[65536];
static uint32_t      g_stk[512];
static wv_disp_t     g_di[4096];
static char          g_lpo[131072];
static wv_blk_t      g_lb[520];
static wv_inl_t      g_li[520];
static wv_walk_t     g_lw[520];
static wv_css_rule_t g_cr[256];
static wv_css_decl_t g_cd[1024];
static char          g_cp[32768];

/* forward declarations (the navigation block precedes the paint block) */
static void repaint(void);
static void navigate(const char *url_text);
static void navigate_ex(const char *url_text, int push);
static void nav_back(void);
static void nav_forward(void);
static int follow_link(int n);
static void page_build(void);
static void set_title_for_url(void);
static char hover_url[WV_URL_MAX_URL] = "";   /* link under the cursor */

/* Rebuild DOM + CSS + layout from raw HTML (a loaded page). */
static void page_load_html(const char *html, size_t len) {
    wv_arena_t toks_a;
    wv_arena_init(&toks_a, g_tt, 2048, g_ta, 2048, g_tp, sizeof(g_tp));
    if (wv_html_tokenize(&toks_a, html, len) < 0) return;
    wv_dom_t dom;
    wv_dom_init(&dom, g_dn, 2048, g_dda, 2048, g_dpp, sizeof(g_dpp), g_stk, 512);
    if (wv_dom_build(&dom, &toks_a, 512) < 0) return;
    wv_css_init(&css, g_cr, 256, g_cd, 1024, g_cp, sizeof(g_cp));
    wv_css_build(&css, &dom);
    wv_layout_init(&lay, g_di, 4096, g_lpo, sizeof(g_lpo),
                   g_lb, 520, g_li, 520, g_lw, 520);
    wv_layout_run(&lay, &dom, VIEW_W, &css);
    max_scroll = lay.content_h > (uint32_t)VIEW_H
                     ? (int32_t)lay.content_h - VIEW_H : 0;
    scroll_y = 0;
    on_page = 0;

    /* W7: render every <canvas data-scene="cube"> box once, into the
     * cached slot.  The cost is measured and printed. */
    if (cv_px) { free(cv_px); cv_px = 0; }
    cv_valid = 0;
    for (size_t i = 0; i < lay.item_count; i++) {
        const wv_disp_t *it = &lay.items[i];
        if (it->type != WV_D_BOX || it->scene == 0) continue;
        int w = (int)it->w, h = (int)it->h;
        if (w < 8 || h < 8 || w > 256 || h > 256) {
            printf("[gbrowser] canvas: skip oversized %dx%d\n", w, h);
            continue;
        }
        uint32_t *px = malloc((size_t)w * (size_t)h * 4);
        if (!px) continue;
        long us = -1;
        if (wv_canvas_render_cube(px, w, h, wid, &us) != 0) {
            free(px);
            printf("[gbrowser] canvas: render failed %dx%d\n", w, h);
            continue;
        }
        if (cv_px) free(cv_px);
        cv_px = px;
        cv_w = w;
        cv_h = h;
        cv_valid = 1;
        printf("[gbrowser] canvas: rendered %dx%d cube in %ld us\n", w, h, us);
        break;   /* one canvas per page is enough for the built-in scenes */
    }
}

/* ---- Fetch path: libahttp keep-alive client (REALINTERNET_PLAN X6) ----
 *
 * The browser used to hand-roll one net_connect/net_send/net_recv round
 * per navigation.  It now speaks both http:// and https:// through
 * libahttp, on a persistent client: repeated navigations to the same
 * origin reuse the socket ([ahttp] keep-alive lines on the serial log),
 * http→https redirects are followed with chain validation, and POST is
 * available the day forms land.
 *
 * wv_http.c's request/response helpers stay for the host unit tests;
 * the guest fetch path itself is the code below. */

static ahttp_client *g_http;
static int           g_http_roots = -1;   /* -1 = not tried yet */
static int           g_last_tls_hrc;

static ahttp_client *wv_http_client(void) {
    if (!g_http) {
        g_http = ahttp_client_new();
        if (!g_http) return NULL;
        static atls_trust_root roots[32];
        static uint8_t root_der[65536];
        int n = 0;
        if (ahttp_load_trust_roots("/etc/ssl/roots.pem", roots, root_der,
                                   32, sizeof(root_der), &n) == 0) {
            ahttp_client_set_trust_roots(g_http, roots, n, NULL);
            g_http_roots = n;
            printf("[gbrowser] fetch: %d trust root(s) from /etc/ssl/roots.pem\n", n);
        } else {
            /* D7: fail loudly, not silently — TLS then runs in the
             * CertificateVerify-only mode, exactly like /http warns. */
            g_http_roots = 0;
            printf("[gbrowser] fetch: WARNING: /etc/ssl/roots.pem unusable — "
                   "https verifies server signatures only\n");
        }
    }
    return g_http;
}

/* Fetch http:// or https:// URL and load its body.  Returns 0 on success;
 * -status on an HTTP error, an ahttp error code (<0) on transport/TLS
 * failure.  libahttp's own timeouts cap worst-case wait. */
static int wv_fetch_url(const wv_url_t *u, char **body_out, size_t *body_len_out) {
    *body_out = NULL;
    *body_len_out = 0;

    ahttp_client *c = wv_http_client();
    if (!c) return -1;

    char fmt[WV_URL_MAX_URL];
    wv_url_format(u, fmt, sizeof(fmt));
    ahttp_response *r = ahttp_client_get(c, fmt);
    if (!r) return -1;   /* OOM */
    if (r->error != AHTTP_OK) {
        int rc = r->error;   /* already negative */
        g_last_tls_hrc = r->tls_error;
        ahttp_response_free(r);
        return rc;
    }
    if (r->status_code != 200) {
        int rc = -r->status_code;
        ahttp_response_free(r);
        return rc;
    }
    char *body = malloc(r->body_len + 1);
    if (!body) { ahttp_response_free(r); return -1; }
    memcpy(body, r->body, r->body_len);
    body[r->body_len] = 0;
    size_t bl = r->body_len;
    ahttp_response_free(r);
    *body_out = body;
    *body_len_out = bl;
    return 0;
}

/* The window title follows the current page. */
static void set_title_for_url(void) {
    char t[96];
    if (current_url_str[0]) {
        wv_url_t u;
        if (wv_url_parse(current_url_str, &u) && u.ok)
            snprintf(t, sizeof(t), "Browser - %s", u.host);
        else
            snprintf(t, sizeof(t), "Browser");
    } else {
        snprintf(t, sizeof(t), "Browser");
    }
    ag_window_set_title(wid, t);
}

/* Load a URL and render it (http:// and https:// alike, X6).  Input
 * without a scheme gets https:// prepended — secure by default now that
 * TLS exists.  push=1 records the visit in the history. */
static void navigate_ex(const char *url_text, int push) {
    char norm[WV_URL_MAX_URL];
    if (!strstr(url_text, "://")) {
        int nn = snprintf(norm, sizeof(norm), "https://%s", url_text);
        if (nn > 0 && (size_t)nn < sizeof(norm)) url_text = norm;
    }

    wv_url_t u;
    if (!wv_url_parse(url_text, &u) || !u.ok) {
        printf("[gbrowser] nav: bad url '%s'\n", url_text);
        return;
    }
    char fmt[WV_URL_MAX_URL];
    wv_url_format(&u, fmt, sizeof(fmt));

    printf("[gbrowser] nav: fetching %s\n", fmt);
    char *body = NULL;
    size_t blen = 0;
    g_last_tls_hrc = 0;
    int rc = wv_fetch_url(&u, &body, &blen);
    if (rc != 0) {
        const char *why = ahttp_strerror(rc, g_last_tls_hrc);
        printf("[gbrowser] nav: fetch failed (%d) for %s — %s\n",
               rc, fmt, why);
        char page[384];
        int pl = snprintf(page, sizeof(page),
            "<body><h1>Load failed</h1><p>%s</p><p>%s (%d)</p></body>",
            fmt, why, rc);
        page_load_html(page, (size_t)pl > 0 ? (size_t)pl : 0);
        strncpy(current_url_str, fmt, WV_URL_MAX_URL - 1);
        if (push && hist_pos < HIST_MAX - 1) {
            hist_pos++;
            hist_count = hist_pos + 1;
            strncpy(history[hist_pos], fmt, WV_URL_MAX_URL - 1);
        }
        set_title_for_url();
        repaint();
        return;
    }
    printf("[gbrowser] nav: loaded %u bytes from %s\n", (unsigned)blen, fmt);
    page_load_html(body, blen);
    printf("[gbrowser] nav: html built (items=%u, h=%u)\n",
           (unsigned)lay.item_count, (unsigned)lay.content_h);
    free(body);
    strncpy(current_url_str, fmt, WV_URL_MAX_URL - 1);
    if (push && hist_pos < HIST_MAX - 1) {
        hist_pos++;
        hist_count = hist_pos + 1;
        strncpy(history[hist_pos], fmt, WV_URL_MAX_URL - 1);
    }
    repaint();
}

static void navigate(const char *url_text) { navigate_ex(url_text, 1); }

/* Hit-test a click at page coordinates (x, y): returns 1 and follows the
 * link when the click landed on one. */
static int hit_test_link(int32_t x, int32_t y) {
    int yp = y + scroll_y;
    int seen = 0;
    for (size_t i = 0; i < lay.item_count; i++) {
        const wv_disp_t *it = &lay.items[i];
        if (it->type != WV_D_TEXT || it->link_off == 0) continue;
        if (x >= it->x && x < (int32_t)(it->x + it->w) &&
            yp >= it->y && yp < it->y + WV_FONT_H)
            return follow_link(seen);
        seen++;
    }
    return 0;
}

static void nav_back(void) {
    if (hist_pos <= 0) { printf("[gbrowser] back: no history\n"); return; }
    hist_pos--;
    printf("[gbrowser] back: %s\n", history[hist_pos]);
    navigate_ex(history[hist_pos], 0);
}

static void nav_forward(void) {
    if (hist_pos < 0 || hist_pos >= hist_count - 1) {
        printf("[gbrowser] forward: no history\n");
        return;
    }
    hist_pos++;
    printf("[gbrowser] forward: %s\n", history[hist_pos]);
    navigate_ex(history[hist_pos], 0);
}

/* Home: back to the built-in demo page. */
static void go_home(void) {
    printf("[gbrowser] home\n");
    page_build();      /* forward-declared below */
    on_page = 1;
    repaint();
}

/* Follow link number n of the current page.  Returns 1 when followed. */
static int follow_link(int n) {
    int seen = 0;
    for (size_t i = 0; i < lay.item_count; i++) {
        const wv_disp_t *it = &lay.items[i];
        if (it->type != WV_D_TEXT || it->link_off == 0) continue;
        if (seen++ != n) continue;
        if (it->link_off >= sizeof(g_dpp)) return 0;
        const char *href = &g_dpp[it->link_off];
        wv_url_t base, target;
        if (!wv_url_parse(current_url_str, &base) || !base.ok) return 0;
        if (!wv_url_resolve(href, &base, &target) || !target.ok) return 0;
        char fmt[WV_URL_MAX_URL];
        wv_url_format(&target, fmt, sizeof(fmt));
        printf("[gbrowser] link %d -> %s\n", n, fmt);
        navigate(fmt);
        return 1;
    }
    return 0;
}

/* Tokenise + build DOM + layout + paint context, once.  All working
 * arenas are static (bounded) — nothing on the 64 KiB user stack. */
static void page_build(void) {
    static wv_token_t   tt[2048];
    static wv_attr_t    ta[2048];
    static char         tp[65536];
    static wv_dom_node_t dn[2048];
    static wv_attr_t    dda[2048];
    static char         dpp[65536];
    static uint32_t     stk[512];
    static wv_disp_t    di[4096];
    static char         lpo[131072];
    static wv_blk_t     lb[520];
    static wv_inl_t     li[520];
    static wv_walk_t    lw[520];
    static wv_css_rule_t cr[256];
    static wv_css_decl_t cd[1024];
    static char         cp[32768];

    wv_arena_t toks_a;
    wv_arena_init(&toks_a, tt, 2048, ta, 2048, tp, sizeof(tp));
    wv_html_tokenize(&toks_a, k_page_html, strlen(k_page_html));
    wv_dom_t dom;
    wv_dom_init(&dom, dn, 2048, dda, 2048, dpp, sizeof(dpp), stk, 512);
    wv_dom_build(&dom, &toks_a, 512);
    wv_css_init(&css, cr, 256, cd, 1024, cp, sizeof(cp));
    wv_css_build(&css, &dom);
    printf("[gbrowser] css rules parsed: %d\n", (int)css.rule_count);
    wv_layout_init(&lay, di, 4096, lpo, sizeof(lpo), lb, 520, li, 520, lw, 520);
    wv_layout_run(&lay, &dom, VIEW_W, &css);
    wv_paint_init(&P, page, VIEW_W, VIEW_H);
    max_scroll = lay.content_h > (uint32_t)VIEW_H
                     ? (int32_t)lay.content_h - VIEW_H : 0;
    printf("[gbrowser] page rendered (content_h=%u, max_scroll=%d)\n",
           lay.content_h, max_scroll);
}

/* The W4 smoke: paint at scroll 0 and compare the buffer hash against the
 * stored reference — a change in rendering is a deliberate act. */
static void paint_smoke(void) {
    wv_paint_rect(&P, 0, 0, VIEW_W, VIEW_H, 0x00FFFFFFu);
    wv_paint_run(&P, &lay, 0);
    uint32_t h = wv_paint_hash(page, VIEW_W, VIEW_H);
    int ok = (h == 0xE57F068Cu);
    printf("[gbrowser] paint smoke: %s (hash=0x%08x)\n", ok ? "PASS" : "FAIL", h);
}

/* The W5 smoke: the <style> block must change the output (the styled page
 * hash differs from the un-styled build of the same document). */
static void css_smoke(void) {
    wv_paint_rect(&P, 0, 0, VIEW_W, VIEW_H, 0x00FFFFFFu);
    wv_paint_run(&P, &lay, 0);
    uint32_t h_css = wv_paint_hash(page, VIEW_W, VIEW_H);

    static wv_token_t tt2[2048];
    static wv_attr_t  ta2[2048];
    static char       tp2[65536];
    static wv_dom_node_t dn2[2048];
    static wv_attr_t  dda2[2048];
    static char       dpp2[65536];
    static uint32_t   stk2[512];
    static wv_disp_t  di2[4096];
    static char       lpo2[131072];
    static wv_blk_t   lb2[520];
    static wv_inl_t   li2[520];
    static wv_walk_t  lw2[520];
    wv_arena_t ta2a;
    wv_arena_init(&ta2a, tt2, 2048, ta2, 2048, tp2, sizeof(tp2));
    wv_html_tokenize(&ta2a, k_page_html, strlen(k_page_html));
    wv_dom_t dom2;
    wv_dom_init(&dom2, dn2, 2048, dda2, 2048, dpp2, sizeof(dpp2), stk2, 512);
    wv_dom_build(&dom2, &ta2a, 512);
    wv_layout_t lay2;
    wv_layout_init(&lay2, di2, 4096, lpo2, sizeof(lpo2), lb2, 520, li2, 520, lw2, 520);
    wv_layout_run(&lay2, &dom2, VIEW_W, 0);
    wv_paint_rect(&P, 0, 0, VIEW_W, VIEW_H, 0x00FFFFFFu);
    wv_paint_run(&P, &lay2, 0);
    uint32_t h_plain = wv_paint_hash(page, VIEW_W, VIEW_H);

    int ok = (h_css != h_plain);
    printf("[gbrowser] css smoke: %s (styled=0x%08x, plain=0x%08x)\n",
           ok ? "PASS" : "FAIL", h_css, h_plain);
}

/* The W7 smoke: render the built-in cube scene into a buffer and blit it
 * into the page — the page hash must change, and the cost is recorded
 * (the number the plan's §1 table was missing). */
static void canvas_smoke(void) {
    static uint32_t cb[64 * 48];
    long us = -1;
    int rc = wv_canvas_render_cube(cb, 64, 48, wid, &us);
    if (rc != 0) {
        printf("[gbrowser] canvas smoke: FAIL (render rc=%d)\n", rc);
        return;
    }
    wv_paint_rect(&P, 0, 0, VIEW_W, VIEW_H, 0x00FFFFFFu);
    wv_paint_run(&P, &lay, 0);
    uint32_t h_before = wv_paint_hash(page, VIEW_W, VIEW_H);
    wv_canvas_blit(page, VIEW_W, VIEW_H, cb, 64, 48, 24, 320, 0);
    uint32_t h_after = wv_paint_hash(page, VIEW_W, VIEW_H);
    int ok = (h_before != h_after);
    printf("[gbrowser] canvas smoke: %s (64x48 cube in %ld us, hash %08x -> %08x)\n",
           ok ? "PASS" : "FAIL", us, h_before, h_after);
}

/* The W4 scroll smoke: memmove-scroll + band repaint must equal a full
 * repaint at the new offset (the 0.068 ms path from the plan). */
static void scroll_smoke(void) {
    wv_paint_rect(&P, 0, 0, VIEW_W, VIEW_H, 0x00FFFFFFu);
    wv_paint_run(&P, &lay, 0);
    int32_t bt, bh;
    wv_paint_scroll(&P, 40, &bt, &bh);
    wv_paint_rect(&P, 0, bt, VIEW_W, bh, 0x00FFFFFFu);
    wv_paint_band(&P, &lay, 40, bt, bh);
    uint32_t h_scroll = wv_paint_hash(page, VIEW_W, VIEW_H);

    wv_paint_rect(&P, 0, 0, VIEW_W, VIEW_H, 0x00FFFFFFu);
    wv_paint_run(&P, &lay, 40);
    uint32_t h_full = wv_paint_hash(page, VIEW_W, VIEW_H);

    int ok = (h_scroll == h_full);
    printf("[gbrowser] paint scroll smoke: %s (memmove+band %s full repaint)\n",
           ok ? "PASS" : "FAIL", ok ? "==" : "!=");
}

/* Present the page buffer into the window, below the chrome bar. */
static void present(void) {
    ag_blit(wid, 0, PAGE_OFF_Y, VIEW_W, VIEW_H, page, VIEW_W);
    frames_done++;
}

/* The chrome: URL bar + Back/Go buttons + status strip. */
/* Button geometry: Back | Fwd | Home | [URL bar] | Go */
#define BTN_BACK_X 4
#define BTN_FWD_X  52
#define BTN_HOME_X 100
#define BTN_URL_X  148
#define BTN_URL_W  (WIN_W - 148 - 52)
#define BTN_GO_X   (WIN_W - 48)
#define BTN_H 20

static void draw_chrome(const char *status) {
    /* bar background */
    ag_fill_rect(wid, 0, 0, WIN_W, CHROME_H, 0x00202028);
    /* Back */
    ag_fill_rect(wid, BTN_BACK_X, 4, 44, BTN_H, 0x00303040);
    ag_draw_text(wid, BTN_BACK_X + 8, 8, "Back", 0x00DDEEFF);
    /* Fwd */
    ag_fill_rect(wid, BTN_FWD_X, 4, 44, BTN_H, 0x00303040);
    ag_draw_text(wid, BTN_FWD_X + 10, 8, "Fwd", 0x00DDEEFF);
    /* Home */
    ag_fill_rect(wid, BTN_HOME_X, 4, 44, BTN_H, 0x00303040);
    ag_draw_text(wid, BTN_HOME_X + 8, 8, "Home", 0x00DDEEFF);
    /* Go */
    ag_fill_rect(wid, BTN_GO_X, 4, 44, BTN_H, 0x002F60C0);
    ag_draw_text(wid, BTN_GO_X + 10, 8, "Go", 0x00FFFFFF);
    /* URL bar */
    ag_fill_rect(wid, BTN_URL_X, 4, BTN_URL_W, BTN_H, 0x00FFFFFF);
    ag_draw_text(wid, BTN_URL_X + 4, 8, addr_buf, 0x00000000);
    /* status strip: the hovered link URL takes priority */
    const char *st = hover_url[0] ? hover_url : status;
    ag_fill_rect(wid, 0, VIEW_H + PAGE_OFF_Y, WIN_W, 40, 0x00202028);
    ag_draw_text(wid, 8, VIEW_H + PAGE_OFF_Y + 12, st, 0x00DDEEFF);
}

/* Chrome hit zones (client coordinates). */
static int chrome_back_hit(int32_t x, int32_t y) {
    return x >= BTN_BACK_X && x < BTN_BACK_X + 44 && y >= 4 && y < 4 + BTN_H;
}
static int chrome_fwd_hit(int32_t x, int32_t y) {
    return x >= BTN_FWD_X && x < BTN_FWD_X + 44 && y >= 4 && y < 4 + BTN_H;
}
static int chrome_home_hit(int32_t x, int32_t y) {
    return x >= BTN_HOME_X && x < BTN_HOME_X + 44 && y >= 4 && y < 4 + BTN_H;
}
static int chrome_go_hit(int32_t x, int32_t y) {
    return x >= BTN_GO_X && x < BTN_GO_X + 44 && y >= 4 && y < 4 + BTN_H;
}
static int chrome_url_hit(int32_t x, int32_t y) {
    return x >= BTN_URL_X && x < BTN_URL_X + BTN_URL_W && y >= 4 && y < 4 + BTN_H;
}

/* Find the href of the link under (x, y) — page coordinates. */
static const char *link_under(int32_t x, int32_t y) {
    int yp = y + scroll_y;
    for (size_t i = 0; i < lay.item_count; i++) {
        const wv_disp_t *it = &lay.items[i];
        if (it->type != WV_D_TEXT || it->link_off == 0) continue;
        if (x >= it->x && x < (int32_t)(it->x + it->w) &&
            yp >= it->y && yp < it->y + WV_FONT_H) {
            if (it->link_off < sizeof(g_dpp)) return &g_dpp[it->link_off];
        }
    }
    return NULL;
}

/* Repaint: full page from the display list, present, chrome + status. */
static void repaint(void) {
    wv_paint_rect(&P, 0, 0, VIEW_W, VIEW_H, 0x00FFFFFFu);   /* paper */
    wv_paint_run(&P, &lay, scroll_y);
    if (cv_valid) {
        /* composite the cached GL canvas into its box (clipped, scrolled
         * with the page — wv_canvas_blit handles both) */
        for (size_t i = 0; i < lay.item_count; i++) {
            const wv_disp_t *it = &lay.items[i];
            if (it->type == WV_D_BOX && it->scene != 0) {
                wv_canvas_blit(page, VIEW_W, VIEW_H, cv_px, cv_w, cv_h,
                               it->x, it->y, scroll_y);
                break;
            }
        }
    }
    present();

    char status[96];
    if (on_page)
        snprintf(status, sizeof(status),
                 "demo page - scroll %d px - frames %u",
                 scroll_y, frames_done);
    else if (current_url_str[0])
        snprintf(status, sizeof(status),
                 "%s - scroll %d px - frames %u",
                 current_url_str, scroll_y, frames_done);
    else
        snprintf(status, sizeof(status),
                 "scroll %d px - frames %u", scroll_y, frames_done);
    draw_chrome(status);
    ag_render_now();
}

/* Scroll via memmove of the retained buffer + repaint of the exposed band
 * only (the plan's 0.068 ms path). */
static void scroll_to(int new_scroll) {
    if (new_scroll < 0) new_scroll = 0;
    if (new_scroll > max_scroll) new_scroll = max_scroll;
    hover_url[0] = 0;                 /* the hovered link moved away */
    int delta = new_scroll - scroll_y;
    if (delta == 0) return;
    scroll_y = new_scroll;
    int32_t bt, bh;
    wv_paint_scroll(&P, delta, &bt, &bh);
    wv_paint_rect(&P, 0, bt, VIEW_W, bh, 0x00FFFFFFu);      /* paper band */
    wv_paint_band(&P, &lay, scroll_y, bt, bh);
    present();
}

int main(void) {
    printf("[gbrowser] starting W0 scaffold\n");

    /* The page buffer goes on the heap, not the 64 KiB user stack. */
    page = malloc((size_t)VIEW_W * VIEW_H * sizeof(uint32_t));
    if (!page) {
        printf("[gbrowser] FAIL: cannot allocate %d x %d page buffer\n",
               VIEW_W, VIEW_H);
        return 1;
    }

    wid = ag_window_create(40, 40, WIN_W, WIN_H, "Browser", AG_WIN_DEFAULT);
    if (wid < 0) {
        printf("[gbrowser] FAIL: window create returned %d\n", wid);
        free(page);
        return 1;
    }
    ag_window_show(wid);
    printf("[gbrowser] window created (id %d, %dx%d)\n", wid, WIN_W, WIN_H);

    page_build();
    on_page = 1;
    paint_smoke();
    css_smoke();
    canvas_smoke();
    scroll_smoke();
    repaint();
    printf("[gbrowser] page rendered and presented\n");

    /* ---- Phase W0 benchmark: the presentation path, measured. ------------
     * The plan (§1) says a full-page blit at 800x600 is ~0.125 ms on the
     * reference machine.  This phase records the number for this build
     * instead of trusting the old one. */
    {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < BENCH_FRAMES; i++)
            ag_blit(wid, 0, 0, VIEW_W, VIEW_H, page, VIEW_W);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long sec = t1.tv_sec - t0.tv_sec;
        long nsec = t1.tv_nsec - t0.tv_nsec;
        if (nsec < 0) { sec--; nsec += 1000000000L; }
        long total_us = sec * 1000000L + nsec / 1000L;
        /* NOTE: libc printf has no %f, so times are reported in integer
         * microseconds — plenty for a frame budget. */
        printf("[gbrowser] blit %ux%u: %ld us/frame (%d frames, total %ld us)\n",
               VIEW_W, VIEW_H, total_us / BENCH_FRAMES, BENCH_FRAMES, total_us);
        printf("[gbrowser] PASS: blit benchmark completed\n");
    }

    /* ---- Phase W1 smoke: the HTML tokeniser runs in-guest too. ----------
     * The host unit tests are the real gate; this proves the same source
     * builds and behaves inside AuraLite user space. */
    {
        static wv_token_t wtoks[64];
        static wv_attr_t  watrs[256];
        static char       wpool[4096];
        wv_arena_t wa;
        wv_arena_init(&wa, wtoks, 64, watrs, 256, wpool, sizeof(wpool));
        const char *doc = "<p>Hello <b>web</b> &amp; bye<br/></p>";
        int n = wv_html_tokenize(&wa, doc, strlen(doc));
        printf("[gbrowser] tokeniser smoke: %d tokens, truncated=%d\n",
               n, wa.truncated);
        if (n == 9 && !wa.truncated) {
            /* START p, TEXT, START b, TEXT, END b, TEXT, START br,
             * END p, EOF */
            printf("[gbrowser] tokeniser smoke: PASS\n");
        } else {
            printf("[gbrowser] tokeniser smoke: FAIL\n");
        }
    }

    /* ---- Phase W2 smoke: the DOM builder runs in-guest too. ------------
     * Two checks: the plan's sibling-paragraph rule and the 10 000-deep
     * document, which must hit the depth cap WITHOUT overflowing the real
     * 64 KiB user stack (the point of the plan's W2 gate). */
    {
        static wv_dom_node_t dn[256];
        static wv_attr_t     da[512];
        static char          dp[16384];
        static uint32_t      dstack[512];
        wv_arena_t toks_a;
        static wv_token_t tt[512];
        static wv_attr_t  ta[512];
        static char       tp[8192];
        wv_arena_init(&toks_a, tt, 512, ta, 512, tp, sizeof(tp));
        const char *doc = "<p>a<p>b";
        wv_html_tokenize(&toks_a, doc, strlen(doc));
        wv_dom_t dom;
        wv_dom_init(&dom, dn, 256, da, 512, dp, sizeof(dp), dstack, 512);
        int nn = wv_dom_build(&dom, &toks_a, 512);
        /* doc + 2 p + 2 text; both p's are children of the document */
        int ok = (nn == 5 && dom.nodes[1].parent == 0 &&
                  dom.nodes[3].parent == 0 && dom.truncated == 0);
        printf("[gbrowser] dom smoke: %s (nodes=%d)\n",
               ok ? "PASS" : "FAIL", nn);
    }

    {
        /* 10 000 nested <div>s: tokeniser arena on the heap (20 001
         * tokens), DOM with a depth cap of 512.  On the 64 KiB user stack
         * this must not overflow — that is the W2 QEMU gate. */
        size_t doc_len = 10000 * 5 + 10000 * 6;
        char *doc = malloc(doc_len);
        if (!doc) {
            printf("[gbrowser] dom deep test: FAIL (malloc)\n");
        } else {
            size_t p = 0;
            for (int i = 0; i < 10000; i++) { memcpy(doc + p, "<div>", 5); p += 5; }
            for (int i = 0; i < 10000; i++) { memcpy(doc + p, "</div>", 6); p += 6; }

            wv_token_t *tt = malloc(22000 * sizeof(wv_token_t));
            wv_attr_t  *ta = malloc(1024 * sizeof(wv_attr_t));
            char       *tp = malloc(262144);
            wv_dom_node_t *dn = malloc(11000 * sizeof(wv_dom_node_t));
            wv_attr_t  *dda = malloc(1024 * sizeof(wv_attr_t));
            char       *dpp = malloc(262144);
            uint32_t   *stk = malloc(1024 * sizeof(uint32_t));
            if (!tt || !ta || !tp || !dn || !dda || !dpp || !stk) {
                printf("[gbrowser] dom deep test: FAIL (malloc arenas)\n");
            } else {
                wv_arena_t toks_a;
                wv_arena_init(&toks_a, tt, 22000, ta, 1024, tp, 262144);
                int n = wv_html_tokenize(&toks_a, doc, doc_len);
                wv_dom_t dom;
                wv_dom_init(&dom, dn, 11000, dda, 1024, dpp, 262144, stk, 1024);
                int nn = wv_dom_build(&dom, &toks_a, 512);
                uint32_t maxd = 0;
                for (int i = 0; i < nn && i < 11000; i++) {
                    uint32_t dd = wv_dom_depth(&dom, (uint32_t)i);
                    if (dd > maxd) maxd = dd;
                }
                int ok = (nn == 10001 && maxd == 512 && dom.truncated == 1);
                printf("[gbrowser] dom deep test: %s (tokens=%d nodes=%d maxdepth=%u truncated=%d)\n",
                       ok ? "PASS" : "FAIL", n, nn, maxd, dom.truncated);
            }
            free(tt); free(ta); free(tp);
            free(dn); free(dda); free(dpp); free(stk);
            free(doc);
        }
    }

    /* ---- Phase W3 smoke: block layout runs in-guest too. ----------------
     * Builds the 5 000-box document from the plan's gate, lays it out and
     * measures the cost — on the real 64 KiB user stack, with the real
     * clock. */
    {
        size_t doc_len = 5000 * 12;
        char *doc = malloc(doc_len);
        if (!doc) {
            printf("[gbrowser] layout smoke: FAIL (malloc)\n");
        } else {
            size_t p = 0;
            for (int i = 0; i < 5000; i++) {
                memcpy(doc + p, "<div>a</div>", 12);
                p += 12;
            }
            wv_token_t *tt = malloc(22000 * sizeof(wv_token_t));
            wv_attr_t  *ta = malloc(1024 * sizeof(wv_attr_t));
            char       *tp = malloc(262144);
            wv_dom_node_t *dn = malloc(12000 * sizeof(wv_dom_node_t));
            wv_attr_t  *dda = malloc(1024 * sizeof(wv_attr_t));
            char       *dpp = malloc(262144);
            uint32_t   *stk = malloc(1024 * sizeof(uint32_t));
            wv_disp_t  *di = malloc(12000 * sizeof(wv_disp_t));
            char       *lpo = malloc(262144);
            wv_blk_t   *lb = malloc(520 * sizeof(wv_blk_t));
            wv_inl_t   *li = malloc(520 * sizeof(wv_inl_t));
            wv_walk_t  *lw = malloc(520 * sizeof(wv_walk_t));
            if (!tt || !ta || !tp || !dn || !dda || !dpp || !stk ||
                !di || !lpo || !lb || !li || !lw) {
                printf("[gbrowser] layout smoke: FAIL (malloc arenas)\n");
            } else {
                wv_arena_t toks_a;
                wv_arena_init(&toks_a, tt, 22000, ta, 1024, tp, 262144);
                wv_html_tokenize(&toks_a, doc, doc_len);
                wv_dom_t dom;
                wv_dom_init(&dom, dn, 12000, dda, 1024, dpp, 262144, stk, 1024);
                wv_dom_build(&dom, &toks_a, 512);
                wv_layout_t lay;
                wv_layout_init(&lay, di, 12000, lpo, 262144, lb, 520, li, 520, lw, 520);

                struct timespec t0, t1;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                int ni = wv_layout_run(&lay, &dom, 800, 0);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                long us = (t1.tv_sec - t0.tv_sec) * 1000000L +
                          (t1.tv_nsec - t0.tv_nsec) / 1000L;
                int ok = (ni == 10001 && !lay.truncated);
                printf("[gbrowser] layout smoke: %s (items=%d, 5000 boxes in %ld us, page_h=%u)\n",
                       ok ? "PASS" : "FAIL", ni, us, lay.content_h);
            }
            free(tt); free(ta); free(tp);
            free(dn); free(dda); free(dpp); free(stk);
            free(di); free(lpo); free(lb); free(li); free(lw);
            free(doc);
        }
    }

    /* ---- W6: initial URL from /tmp/gbrowser.url (test hook) ---- */
    {
        int fd = open("/tmp/gbrowser.url", O_RDONLY);
        if (fd >= 0) {
            char url[WV_URL_MAX_URL];
            int n = read(fd, url, sizeof(url) - 1);
            close(fd);
            if (n > 0) {
                url[n] = 0;
                while (n > 0 && (url[n-1] == '\n' || url[n-1] == '\r')) url[--n] = 0;
                printf("[gbrowser] initial url: %s\n", url);
                navigate(url);
            }
        }
        /* /tmp/gbrowser.steps: "link 0; back; https; nav <url>" — executed
         * one by one with a pause between.  The init shell blocks on
         * `run`, so the test writes the whole script BEFORE starting the
         * web view. */
        int fd2 = open("/tmp/gbrowser.steps", O_RDONLY);
        if (fd2 >= 0) {
            char steps[512];
            int n = read(fd2, steps, sizeof(steps) - 1);
            close(fd2);
            if (n > 0) {
                steps[n] = 0;
                char *save = 0;
                int idx = 0;
                for (char *tok = strtok_r(steps, "|", &save); tok;
                     tok = strtok_r(0, "|", &save)) {
                    while (*tok == ' ' || *tok == '\t') tok++;
                    size_t tl = strlen(tok);
                    while (tl > 0 && (tok[tl-1] == ' ' || tok[tl-1] == '\t' ||
                                      tok[tl-1] == '\n')) tok[--tl] = 0;
                    if (tl == 0) continue;
                    printf("[gbrowser] step %d: %s\n", idx, tok);
                    if (strncmp(tok, "nav ", 4) == 0) {
                        navigate(tok + 4);
                    } else if (strcmp(tok, "back") == 0) {
                        nav_back();
                    } else if (strcmp(tok, "https") == 0) {
                        navigate("https://example.com/");
                    } else if (strncmp(tok, "link ", 5) == 0) {
                        int ln = 0;
                        for (const char *c = tok + 5; *c >= '0' && *c <= '9'; c++)
                            ln = ln * 10 + (*c - '0');
                        follow_link(ln);
                    }
                    idx++;
                    struct timespec pause = { 1, 500000000 };
                    nanosleep(&pause, 0);
                }
            }
        }
    }

    /* ---- Event loop ---- */
    uint32_t limit = frame_limit();
    if (limit) printf("[gbrowser] frame limit: %u frames\n", limit);

    for (;;) {
        ag_event_t ev;
        while (ag_poll_event(wid, &ev) > 0) {
            switch (ev.type) {
            case AG_EVT_CLOSE_REQ:
                printf("[gbrowser] close requested\n");
                goto done;
            case AG_EVT_KEY_DOWN:
                if (ev.key == 'q' || ev.key == 'Q' || ev.key == 27 /* Esc */) {
                    printf("[gbrowser] quit via key\n");
                    goto done;
                }
                if (ev.key == '\r' || ev.key == '\n') {
                    if (addr_len > 0) {
                        addr_buf[addr_len] = 0;
                        navigate(addr_buf);
                    }
                    break;
                }
                if (ev.key == 8 /* backspace */) {
                    if (addr_len > 0) addr_len--;
                    addr_buf[addr_len] = 0;
                    repaint();
                    break;
                }
                if (ev.key >= 0x20 && ev.key < 0x7F && addr_len < WV_URL_MAX_URL - 1) {
                    addr_buf[addr_len++] = (char)ev.key;
                    addr_buf[addr_len] = 0;
                    repaint();
                }
                if (ev.key == 0x102 /* arrow up */) { scroll_to(scroll_y - 16); }
                if (ev.key == 0x103 /* arrow down */) { scroll_to(scroll_y + 16); }
                break;
            case AG_EVT_MOUSE_DOWN: {
                int32_t mx = ev.x, my = ev.y;
                if (chrome_back_hit(mx, my)) {
                    nav_back();
                } else if (chrome_fwd_hit(mx, my)) {
                    nav_forward();
                } else if (chrome_home_hit(mx, my)) {
                    go_home();
                } else if (chrome_go_hit(mx, my)) {
                    if (addr_len > 0) { addr_buf[addr_len] = 0; navigate(addr_buf); }
                } else if (chrome_url_hit(mx, my)) {
                    /* clicking the address bar focuses it */
                } else if (my >= PAGE_OFF_Y) {
                    /* page area: hit-test links */
                    hit_test_link(mx, my - PAGE_OFF_Y);
                }
                break;
            }
            case AG_EVT_MOUSE_MOVE: {
                /* hover: show the link's target in the status strip */
                int32_t mx = ev.x, my = ev.y;
                const char *new_hover = "";
                if (my >= PAGE_OFF_Y) {
                    const char *h = link_under(mx, my - PAGE_OFF_Y);
                    if (h) new_hover = h;
                }
                if (strcmp(hover_url, new_hover) != 0) {
                    strncpy(hover_url, new_hover, sizeof(hover_url) - 1);
                    draw_chrome("");      /* chrome + status only */
                    ag_render_now();
                }
                break;
            }
            case AG_EVT_MOUSE_WHEEL: {
                /* The kernel GUI ABI delivers the wheel delta in ev.key
                 * (gui.c: gui_event_t.key = ev->wheel). */
                int delta = (int)(int32_t)ev.key;
                scroll_to(scroll_y - delta * 24);
                break;
            }
            case AG_EVT_PAINT:
                repaint();
                break;
            default:
                break;
            }
        }

        /* W6 test hook: /tmp/gbrowser.cmd is polled; a changed line is
         * executed ("nav <url>", "back", "link <n>", "https"). */
        {
            int fd = open("/tmp/gbrowser.cmd", O_RDONLY);
            if (fd >= 0) {
                char cmd[WV_URL_MAX_URL + 64];
                int n = read(fd, cmd, sizeof(cmd) - 1);
                close(fd);
                if (n > 0) {
                    cmd[n] = 0;
                    while (n > 0 && (cmd[n-1] == '\n' || cmd[n-1] == '\r')) cmd[--n] = 0;
                    /* the init shell does not strip quotes */
                    while (n > 0 && (cmd[0] == '"' || cmd[0] == 0x27)) {
                        for (int q = 0; q + 1 < (int)n; q++) cmd[q] = cmd[q + 1];
                        n--;
                    }
                    while (n > 0 && (cmd[n-1] == '"' || cmd[n-1] == 0x27)) cmd[--n] = 0;
                    if (strcmp(cmd, last_cmd) != 0) {
                        strncpy(last_cmd, cmd, sizeof(last_cmd) - 1);
                        if (strncmp(cmd, "nav ", 4) == 0) {
                            navigate(cmd + 4);
                        } else if (strcmp(cmd, "back") == 0) {
                            nav_back();
                        } else if (strcmp(cmd, "https") == 0) {
                            navigate("https://example.com/");
                        } else if (strncmp(cmd, "link ", 5) == 0) {
                            int ln = 0;
                            for (const char *c = cmd + 5; *c >= '0' && *c <= '9'; c++)
                                ln = ln * 10 + (*c - '0');
                            follow_link(ln);
                        }
                    }
                }
            }
        }

        if (limit && frames_done >= limit) {
            printf("[gbrowser] PASS: %u frames rendered, exiting\n", frames_done);
            goto done;
        }

        /* Pulse: repaint even with no input, so the frame counter is live
         * and the paint path is exercised continuously (the same cadence
         * the W4 renderer will use). */
        repaint();

        /* Yield so the compositor and other threads get CPU time. */
        struct timespec ts = { 0, 5000000 };   /* 5 ms */
        nanosleep(&ts, 0);
    }

done:
    printf("[gbrowser] W0 scaffold complete\n");
    ag_window_destroy(wid);
    free(page);
    return 0;
}

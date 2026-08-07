/*
 * webview.c — AuraLite OS web view, Phase W0 (WEBVIEW_PLAN.md).
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
 *   - /tmp/webview.frames holds an optional decimal frame count (same
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

#include "auragui.h"
#include "wv_html.h"
#include "wv_dom.h"
#include "wv_layout.h"

/* The page surface.  VIEW_W x VIEW_H x 4 bytes = 1.92 MiB on the heap. */
#define VIEW_W 800
#define VIEW_H 600

/* Window is the viewport plus room for a status strip below it. */
#define WIN_W  800
#define WIN_H  (VIEW_H + 40)

/* How many present() calls the startup benchmark runs. */
#define BENCH_FRAMES 200

static uint32_t *page;              /* the pixel buffer being presented */
static int wid;                     /* our window id */
static int scroll_y = 0;            /* vertical scroll offset, in pixels */
static uint32_t frames_done = 0;    /* rendered frames */

/* Read the optional /tmp/webview.frames frame limit.  Returns 0 when the
 * file is absent or unreadable, meaning "run forever". */
static uint32_t frame_limit(void) {
    int fd = open("/tmp/webview.frames", O_RDONLY);
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

/* ---- the "document" this phase can render ---------------------------------
 *
 * W0 draws a standing page, not a parsed one: a header band, some block
 * rectangles standing in for paragraphs, and (through the window API, which
 * has the PSF rasteriser) the honest text the plan requires.  From W4 the
 * text will live in this buffer, rasterised by webview's own glyph path; the
 * window-API text here is scaffold, not the renderer.
 */

static void draw_page(uint32_t *buf, int w, int h, int scroll) {
    /* Paper background. */
    for (int y = 0; y < h; y++) {
        uint32_t *row = buf + (size_t)y * w;
        for (int x = 0; x < w; x++) row[x] = 0x00FDFDF7;
    }

    /* Header band (moves with the page). */
    int hb_y = -scroll;
    if (hb_y < h && hb_y > -48) {
        int top = hb_y < 0 ? 0 : hb_y;
        int bot = hb_y + 48 < h ? hb_y + 48 : h;
        for (int y = top; y < bot; y++) {
            uint32_t *row = buf + (size_t)y * w;
            for (int x = 0; x < w; x++) row[x] = 0x002F60C0;
        }
    }

    /* Block "paragraphs": rectangles at increasing depth, each with a
     * contrasting rule so scrolling is visibly a page move. */
    static const uint32_t cols[5] = {
        0x00E8E0D0, 0x00DDE8F0, 0x00E8F0E0, 0x00F0E8E8, 0x00E0E8E8,
    };
    for (int i = 0; i < 5; i++) {
        int py = 64 + i * 90 - scroll;          /* paragraph top */
        int ph = 60;
        if (py + ph < 0 || py >= h) continue;   /* clipped out entirely */
        int top = py < 0 ? 0 : py;
        int bot = py + ph < h ? py + ph : h;
        int pw = 720 - i * 40;
        for (int y = top; y < bot; y++) {
            uint32_t *row = buf + (size_t)y * w;
            for (int x = 40; x < 40 + pw && x < w; x++) row[x] = cols[i];
        }
        /* rule under the block: full width so scroll motion is obvious */
        if (py + ph >= 0 && py + ph < h) {
            uint32_t *row = buf + (size_t)(py + ph) * w;
            for (int x = 0; x < w; x++) row[x] = 0x00B0A898;
        }
    }

    /* Scroll position indicator on the paper margin. */
    if (h > 16) {
        for (int y = 0; y < h; y += 8) {
            buf[(size_t)y * w + 12] = 0x00C0B0A0;
            buf[(size_t)y * w + 13] = 0x00C0B0A0;
        }
    }
}

/* Present the page buffer into the window. */
static void present(void) {
    ag_blit(wid, 0, 0, VIEW_W, VIEW_H, page, VIEW_W);
    frames_done++;
}

/* Repaint: redraw the page, present it, update the status strip. */
static void repaint(void) {
    draw_page(page, VIEW_W, VIEW_H, scroll_y);
    present();

    char status[96];
    snprintf(status, sizeof(status),
             "W0 scaffold - scroll %d px - frames %u",
             scroll_y, frames_done);
    ag_fill_rect(wid, 0, VIEW_H, WIN_W, 40, 0x00202028);
    ag_draw_text(wid, 8, VIEW_H + 12, status, 0x00DDEEFF);
    ag_render_now();
}

/* The honest statement the plan's W0 objective asks the window to make. */
static void paint_limitations(void) {
    ag_fill_rect(wid, 0, 0, WIN_W, WIN_H, 0x00FFFFFF);
    ag_draw_text(wid, 16, 12, "AuraLite WebView - W0 scaffold", 0x00000000);
    ag_draw_text(wid, 16, 32, "This build can:", 0x00000000);
    ag_draw_text(wid, 16, 52, "  - open a window and run an event loop", 0x00303030);
    ag_draw_text(wid, 16, 72, "  - render a pixel buffer and present it with ag_blit()", 0x00303030);
    ag_draw_text(wid, 16, 92, "  - scroll a standing page (wheel) and quit (q / Esc / close)", 0x00303030);
    ag_draw_text(wid, 16, 116, "This build cannot:", 0x00000000);
    ag_draw_text(wid, 16, 136, "  - no HTTPS (TLS is INTERNET_PLAN; see docs/webview.md)", 0x00603020);
    ag_draw_text(wid, 16, 156, "  - no JavaScript (WEBVIEW_PLAN decision D5, permanent)", 0x00603020);
    ag_draw_text(wid, 16, 176, "  - no images (PNG/JPEG decoders are future phases)", 0x00603020);
    ag_draw_text(wid, 16, 196, "  - no proportional fonts (PSF 8x16 monospace only)", 0x00603020);
    ag_draw_text(wid, 16, 220, "Scrolling in 5 s; then the blit benchmark runs.", 0x00404040);
}

int main(void) {
    printf("[webview] starting W0 scaffold\n");

    /* The page buffer goes on the heap, not the 64 KiB user stack. */
    page = malloc((size_t)VIEW_W * VIEW_H * sizeof(uint32_t));
    if (!page) {
        printf("[webview] FAIL: cannot allocate %d x %d page buffer\n",
               VIEW_W, VIEW_H);
        return 1;
    }

    wid = ag_window_create(40, 40, WIN_W, WIN_H, "WebView", AG_WIN_DEFAULT);
    if (wid < 0) {
        printf("[webview] FAIL: window create returned %d\n", wid);
        free(page);
        return 1;
    }
    ag_window_show(wid);
    printf("[webview] window created (id %d, %dx%d)\n", wid, WIN_W, WIN_H);

    paint_limitations();
    ag_render_now();
    printf("[webview] limitations painted\n");

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
        printf("[webview] blit %ux%u: %ld us/frame (%d frames, total %ld us)\n",
               VIEW_W, VIEW_H, total_us / BENCH_FRAMES, BENCH_FRAMES, total_us);
        printf("[webview] PASS: blit benchmark completed\n");
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
        printf("[webview] tokeniser smoke: %d tokens, truncated=%d\n",
               n, wa.truncated);
        if (n == 9 && !wa.truncated) {
            /* START p, TEXT, START b, TEXT, END b, TEXT, START br,
             * END p, EOF */
            printf("[webview] tokeniser smoke: PASS\n");
        } else {
            printf("[webview] tokeniser smoke: FAIL\n");
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
        printf("[webview] dom smoke: %s (nodes=%d)\n",
               ok ? "PASS" : "FAIL", nn);
    }

    {
        /* 10 000 nested <div>s: tokeniser arena on the heap (20 001
         * tokens), DOM with a depth cap of 512.  On the 64 KiB user stack
         * this must not overflow — that is the W2 QEMU gate. */
        size_t doc_len = 10000 * 5 + 10000 * 6;
        char *doc = malloc(doc_len);
        if (!doc) {
            printf("[webview] dom deep test: FAIL (malloc)\n");
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
                printf("[webview] dom deep test: FAIL (malloc arenas)\n");
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
                printf("[webview] dom deep test: %s (tokens=%d nodes=%d maxdepth=%u truncated=%d)\n",
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
            printf("[webview] layout smoke: FAIL (malloc)\n");
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
                printf("[webview] layout smoke: FAIL (malloc arenas)\n");
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
                int ni = wv_layout_run(&lay, &dom, 800);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                long us = (t1.tv_sec - t0.tv_sec) * 1000000L +
                          (t1.tv_nsec - t0.tv_nsec) / 1000L;
                int ok = (ni == 10001 && !lay.truncated);
                printf("[webview] layout smoke: %s (items=%d, 5000 boxes in %ld us, page_h=%u)\n",
                       ok ? "PASS" : "FAIL", ni, us, lay.content_h);
            }
            free(tt); free(ta); free(tp);
            free(dn); free(dda); free(dpp); free(stk);
            free(di); free(lpo); free(lb); free(li); free(lw);
            free(doc);
        }
    }

    /* ---- Event loop.  The W4 renderer will replace draw_page(); the loop
     * shape (poll, handle, repaint on demand) is the one it will keep. */
    uint32_t limit = frame_limit();
    if (limit) printf("[webview] frame limit: %u frames\n", limit);

    for (;;) {
        ag_event_t ev;
        while (ag_poll_event(wid, &ev) > 0) {
            switch (ev.type) {
            case AG_EVT_CLOSE_REQ:
                printf("[webview] close requested\n");
                goto done;
            case AG_EVT_KEY_DOWN:
                if (ev.key == 'q' || ev.key == 'Q' || ev.key == 27 /* Esc */) {
                    printf("[webview] quit via key\n");
                    goto done;
                }
                if (ev.key == 0x102 /* arrow up */) { scroll_y -= 16; repaint(); }
                if (ev.key == 0x103 /* arrow down */) { scroll_y += 16; repaint(); }
                break;
            case AG_EVT_MOUSE_WHEEL: {
                /* The kernel GUI ABI delivers the wheel delta in ev.key
                 * (gui.c: gui_event_t.key = ev->wheel). */
                int delta = (int)(int32_t)ev.key;
                int new_scroll = scroll_y - delta * 24;
                if (new_scroll < 0) new_scroll = 0;
                if (new_scroll > 600) new_scroll = 600;
                if (new_scroll != scroll_y) { scroll_y = new_scroll; repaint(); }
                break;
            }
            case AG_EVT_PAINT:
                repaint();
                break;
            default:
                break;
            }
        }

        if (limit && frames_done >= limit) {
            printf("[webview] PASS: %u frames rendered, exiting\n", frames_done);
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
    printf("[webview] W0 scaffold complete\n");
    ag_window_destroy(wid);
    free(page);
    return 0;
}

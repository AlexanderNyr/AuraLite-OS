/*
 * test_wv_canvas.c — host unit tests for the web view <canvas> renderer
 * (WEBVIEW_PLAN phase W7).
 *
 * Links the REAL userspace/apps/webview/wv_canvas.c against the REAL
 * libgl sources (LIBGL_TEST_SRCS + the auragui stub — the same harness
 * the GL phase tests use), and checks the plan's gate:
 *   - a page containing a canvas renders BOTH the text and the 3D
 *     content (the canvas buffer differs from its clear colour);
 *   - the canvas is clipped with the page (wv_canvas_blit: off-screen
 *     boxes cost a bounds check only; edge clipping works);
 *   - the render cost is measured and recorded (the number the plan's §1
 *     table was missing);
 *   - determinism: two renders of the same scene are byte-identical.
 *
 * Built/run by `make test-unit` under -std=c11 -Wall -Wextra -Werror.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "wv_canvas.h"

static int failures = 0;
#define CK(c) do { if (c) printf("PASS: %s\n", #c); \
    else { printf("FAIL: %s\n", #c); failures++; } } while (0)

#define CW 64
#define CH 48
static uint32_t buf[CW * CH];

static uint32_t frng = 0xCAFEBABEu;
static uint32_t frand(void) {
    frng ^= frng << 13; frng ^= frng >> 17; frng ^= frng << 5;
    return frng;
}

static void test_render_basic(void) {
    memset(buf, 0, sizeof(buf));
    long us = -1;
    int rc = wv_canvas_render_cube(buf, CW, CH, 1, &us);
    CK(rc == 0);
    CK(us >= 0);
    printf("  (canvas 64x48 cube rendered in %ld us)\n", us);

    /* the buffer must not be the clear colour everywhere: the cube covers
     * the centre, the clear colour is (0.08, 0.10, 0.16) = 0x141a29 */
    int nonclear = 0;
    int in_center = 0;
    for (int y = CH / 4; y < 3 * CH / 4; y++) {
        for (int x = CW / 4; x < 3 * CW / 4; x++) {
            uint32_t p = buf[y * CW + x];
            if (p != 0xFF141A29u) {
                nonclear++;
                if (x > CW / 3 && x < 2 * CW / 3 && y > CH / 3 && y < 2 * CH / 3)
                    in_center++;
            }
        }
    }
    CK(nonclear > 0);
    CK(in_center > 0);      /* the cube is drawn in the middle */
    /* at least two distinct face colours are visible under the fixed
     * camera (the 28/33-degree view shows green, blue and purple faces;
     * the red face is hidden behind them) */
    int colors = 0;
    for (int y = 0; y < CH; y++) {
        for (int x = 0; x < CW; x++) {
            uint32_t p = buf[y * CW + x];
            unsigned r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            if (g > 200 && r < 100 && b < 100) { colors |= 1; }   /* green */
            if (b > 200 && r < 120 && g < 150) { colors |= 2; }   /* blue */
            if (r > 180 && g < 130 && b > 180) { colors |= 4; }   /* purple */
        }
    }
    CK((colors & 3) == 3);  /* green AND blue faces visible */
    CK((colors & 4) != 0);  /* the purple face too */
}

static void test_determinism(void) {
    static uint32_t buf2[CW * CH];
    long us1 = 0, us2 = 0;
    CK(wv_canvas_render_cube(buf, CW, CH, 1, &us1) == 0);
    CK(wv_canvas_render_cube(buf2, CW, CH, 1, &us2) == 0);
    CK(memcmp(buf, buf2, sizeof(buf)) == 0);   /* byte-identical scene */
}

static void test_render_sizes(void) {
    /* different canvas sizes produce different, valid buffers */
    static uint32_t small[32 * 24];
    static uint32_t big[96 * 72];
    long us = 0;
    CK(wv_canvas_render_cube(small, 32, 24, 1, &us) == 0);
    CK(wv_canvas_render_cube(big, 96, 72, 1, &us) == 0);
    CK(memcmp(small, big, 32 * 24 * 4) != 0);

    /* invalid sizes refused */
    CK(wv_canvas_render_cube(buf, 0, 0, 1, &us) == -1);
    CK(wv_canvas_render_cube(NULL, CW, CH, 1, &us) == -1);
}

static void test_blit_clip(void) {
    /* page 200x150; canvas at (10, 20); scroll 0 */
    enum { PW = 200, PH = 150 };
    static uint32_t page[PW * PH];
    memset(page, 0, sizeof(page));
    wv_canvas_blit(page, PW, PH, buf, CW, CH, 10, 20, 0);

    /* the canvas pixels land exactly where the box says */
    uint32_t center = buf[(CH / 2) * CW + (CW / 2)];
    CK(page[(20 + CH / 2) * PW + (10 + CW / 2)] == center);
    /* outside the box: untouched */
    CK(page[5 * PW + 5] == 0);
    CK(page[(20 + CH) * PW + 10] == 0);

    /* scroll moves the canvas up with the page: the centre pixel lands
     * 15 px higher, and the strip vacated at the bottom is empty */
    memset(page, 0, sizeof(page));
    wv_canvas_blit(page, PW, PH, buf, CW, CH, 10, 20, 15);
    CK(page[(20 + CH / 2 - 15) * PW + (10 + CW / 2)] == center);
    CK(page[(20 + CH - 1) * PW + (10 + CW / 2)] == 0);   /* vacated row */

    /* scrolled fully off the top: nothing painted, and no faults */
    memset(page, 0xAA, sizeof(page));
    wv_canvas_blit(page, PW, PH, buf, CW, CH, 10, 20, 1000);
    for (int i = 0; i < PW * PH; i++)
        if (page[i] != 0xAAAAAAAAu) { CK(0); printf("  (off-screen wrote a pixel)\n"); return; }
    CK(1);

    /* right-edge clipping */
    memset(page, 0, sizeof(page));
    wv_canvas_blit(page, PW, PH, buf, CW, CH, PW - 10, 20, 0);
    CK(page[20 * PW + PW - 5] != 0);            /* inside */
    CK(page[20 * PW + PW - 1] != 0);            /* last column */
    /* nothing beyond the page (would have overflowed the array) */
}

static void test_fuzz_blit(void) {
    for (int iter = 0; iter < 2000; iter++) {
        int px = (int)(frand() % 400) - 100;
        int py = (int)(frand() % 300) - 100;
        int sc = (int)(frand() % 800);
        static uint32_t page[160 * 120];
        memset(page, 0, sizeof(page));
        wv_canvas_blit(page, 160, 120, buf, CW, CH, px, py, sc);
    }
    CK(1);
    printf("PASS: 2000 fuzz blits (random positions/scrolls) did not fault\n");
}

int main(void) {
    printf("== webview <canvas> renderer (WEBVIEW_PLAN W7) ==\n");

    test_render_basic();
    test_determinism();
    test_render_sizes();
    test_blit_clip();
    test_fuzz_blit();

    printf("== %s: %d failures ==\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

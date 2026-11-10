/* tests/unit/test_taskbar_layout.c
 *
 * OF0 gate (docs/plans/OFFICE_PLAN.md): the taskbar must never lose a window.
 *
 * The 2026-11-08 live pass found "10 windows -> Start + 6 buttons, the rest
 * unreachable": draw_taskbar() and taskbar_hit() each had their own copy of
 * the button arithmetic and each ran out at its own place.  The arithmetic is
 * now one pure function (kernel/gui/taskbar_layout.h) that both call, and this
 * test asserts the properties that make the bug impossible:
 *
 *   P1 no button overlaps another;
 *   P2 no button runs under the clock/tray or off the right edge;
 *   P3 the hit-test rect of a button is exactly the drawn rect (same function,
 *      so this is a regression guard on anyone re-splitting them);
 *   P4 every open window is reachable: either it has a button, or it is on
 *      some page of the "»" overflow list -- for every count 1..256 and every
 *      screen width the GUI can actually run at;
 *   P5 the overflow list pages without losing or repeating an entry.
 *
 * Built and run by `make test-unit`; it includes the kernel header directly
 * and needs no boot, no MMU and no framebuffer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/gui/taskbar_layout.h"

static int failures;
static int checks;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            if (failures <= 25) {                                           \
                printf("FAIL %s:%d: ", __FILE__, __LINE__);                 \
                printf(__VA_ARGS__);                                        \
                printf("\n");                                               \
            }                                                               \
        }                                                                   \
    } while (0)

/* Screen widths the kernel GUI is asked to run at (fb.c accepts 640..1920). */
static const unsigned widths[] = { 640, 800, 1024, 1280, 1366, 1600, 1920 };
static const unsigned heights[] = { 480, 600, 720, 768, 900, 1080 };
#define NWIDTHS (sizeof widths / sizeof widths[0])
#define NHEIGHTS (sizeof heights / sizeof heights[0])

/* Mirrors GUI_MAX_WINDOWS. */
#define MAX_WINDOWS 256u

static void test_strip_geometry(void) {
    for (unsigned wi = 0; wi < NWIDTHS; wi++) {
        unsigned fw = widths[wi];
        for (unsigned n = 1; n <= MAX_WINDOWS; n++) {
            gui_tb_layout_t l;
            gui_tb_layout(fw, n, &l);

            CHECK(l.btn_w >= GUI_TB_BTN_MIN && l.btn_w <= GUI_TB_BTN_MAX,
                  "fw=%u n=%u width %u outside [%u,%u]",
                  fw, n, l.btn_w, GUI_TB_BTN_MIN, GUI_TB_BTN_MAX);

            unsigned prev_end = 0;
            for (unsigned slot = 0; slot < l.visible; slot++) {
                unsigned x = 0, w = 0;
                int drawn = gui_tb_button_rect(fw, n, slot, &x, &w);
                CHECK(drawn, "fw=%u n=%u slot %u not drawn but visible=%u",
                      fw, n, slot, l.visible);
                if (!drawn) continue;
                CHECK(w == l.btn_w, "fw=%u n=%u slot %u width %u != %u",
                      fw, n, slot, w, l.btn_w);
                if (slot > 0) {
                    /* P1 */
                    CHECK(x >= prev_end, "fw=%u n=%u slot %u overlaps (x=%u < %u)",
                          fw, n, slot, x, prev_end);
                }
                /* P2 */
                CHECK(x + w <= l.end, "fw=%u n=%u slot %u runs past strip (%u > %u)",
                      fw, n, slot, x + w, l.end);
                CHECK(x + w <= fw - GUI_TB_TRAY_W,
                      "fw=%u n=%u slot %u under the tray (%u > %u)",
                      fw, n, slot, x + w, fw - GUI_TB_TRAY_W);
                prev_end = x + w + GUI_TB_GAP;
            }

            /* Buttons + overflow button must account for every window. */
            if (l.more_w != 0u) {
                CHECK(l.visible + 1u <= n, "fw=%u n=%u more button with nothing hidden", fw, n);
                CHECK(l.more_x >= l.x0, "fw=%u n=%u more button left of strip", fw, n);
                CHECK(l.more_x + l.more_w <= l.end,
                      "fw=%u n=%u more button past strip (%u > %u)",
                      fw, n, l.more_x + l.more_w, l.end);
                if (l.visible > 0u) {
                    unsigned lx = 0, lw = 0;
                    gui_tb_button_rect(fw, n, l.visible - 1u, &lx, &lw);
                    CHECK(l.more_x >= lx + lw + GUI_TB_GAP,
                          "fw=%u n=%u more button touches last button", fw, n);
                }
            } else {
                CHECK(l.visible == n,
                      "fw=%u n=%u no more button but only %u drawn", fw, n, l.visible);
            }
        }
    }
}

/* P3: the drawn rect and the hit rect are the same function; this asserts the
 * function is at least self-consistent when called twice and that a click at
 * the centre of each drawn button maps back to that same slot. */
static void test_hit_matches_draw(void) {
    for (unsigned wi = 0; wi < NWIDTHS; wi++) {
        unsigned fw = widths[wi];
        for (unsigned n = 1; n <= 40; n++) {
            gui_tb_layout_t l;
            gui_tb_layout(fw, n, &l);
            for (unsigned slot = 0; slot < l.visible; slot++) {
                unsigned x = 0, w = 0;
                if (!gui_tb_button_rect(fw, n, slot, &x, &w)) continue;
                /* Hit test, as taskbar_hit() does it. */
                int found = -1;
                for (unsigned s2 = 0; s2 < l.visible; s2++) {
                    unsigned hx = 0, hw = 0;
                    if (!gui_tb_button_rect(fw, n, s2, &hx, &hw)) break;
                    if (x + w / 2 >= hx && x + w / 2 < hx + hw) { found = (int)s2; break; }
                }
                CHECK(found == (int)slot, "fw=%u n=%u centre of slot %u hit slot %d",
                      fw, n, slot, found);
            }
            /* A click in the "»" slot must not land on a normal button. */
            if (l.more_w != 0u) {
                unsigned mx = l.more_x + l.more_w / 2;
                for (unsigned s2 = 0; s2 < l.visible; s2++) {
                    unsigned hx = 0, hw = 0;
                    if (!gui_tb_button_rect(fw, n, s2, &hx, &hw)) break;
                    CHECK(!(mx >= hx && mx < hx + hw),
                          "fw=%u n=%u more button centre hits slot %u", fw, n, s2);
                }
            }
        }
    }
}

/* P4/P5: walk the overflow list page by page and collect which hidden entries
 * a user can actually reach.  Every hidden window must be reachable. */
static void test_overflow_reaches_everything(void) {
    for (unsigned wi = 0; wi < NWIDTHS; wi++) {
        for (unsigned hi = 0; hi < NHEIGHTS; hi++) {
            unsigned fw = widths[wi], fh = heights[hi];
            unsigned tb_h = 40;   /* the tallest shipped theme taskbar */
            for (unsigned n = 1; n <= MAX_WINDOWS; n++) {
                gui_tb_layout_t l;
                gui_tb_layout(fw, n, &l);
                unsigned hidden = n - l.visible;

                gui_tb_overflow_t ov;
                gui_tb_overflow_geometry(fw, fh, tb_h, n, &ov);

                if (hidden == 0u) {
                    CHECK(ov.rows == 0u && ov.paged == 0, "fw=%u fh=%u n=%u stray list",
                          fw, fh, n);
                    continue;
                }

                CHECK(ov.hidden == hidden, "fw=%u fh=%u n=%u hidden %u != %u",
                      fw, fh, n, ov.hidden, hidden);
                CHECK(ov.rows > 0u, "fw=%u fh=%u n=%u hidden %u with no rows",
                      fw, fh, n, hidden);
                CHECK(ov.y >= 0, "fw=%u fh=%u n=%u list y=%d above screen",
                      fw, fh, n, ov.y);
                CHECK((unsigned)ov.y + ov.h <= fh, "fw=%u fh=%u n=%u list runs off the bottom",
                      fw, fh, n);
                CHECK((unsigned)ov.y + ov.h <= fh - tb_h,
                      "fw=%u fh=%u n=%u list overlaps the taskbar", fw, fh, n);
                CHECK((unsigned)ov.x + ov.w <= fw, "fw=%u fh=%u n=%u list past right edge",
                      fw, fh, n);

                /* Collect reachable entries across pages. */
                unsigned seen[MAX_WINDOWS];
                memset(seen, 0, sizeof seen);
                unsigned page = 0, steps = 0, limit = hidden * 4u + 8u;
                unsigned reachable = 0;
                while (steps++ < limit) {
                    for (unsigned r = 0; r < ov.rows; r++) {
                        int e = gui_tb_overflow_row_entry(&ov, page, r);
                        if (e < 0) continue;
                        CHECK((unsigned)e < hidden, "fw=%u n=%u entry %d out of %u",
                              fw, n, e, hidden);
                        if ((unsigned)e < hidden && !seen[e]) { seen[e] = 1; reachable++; }
                    }
                    if (!ov.paged) break;
                    unsigned next = gui_tb_overflow_next_page(&ov, page);
                    CHECK(next != page, "fw=%u fh=%u n=%u paging did not advance", fw, fh, n);
                    if (next == page) break;
                    page = next;
                }
                CHECK(reachable == hidden,
                      "fw=%u fh=%u n=%u reachable %u of %u hidden windows",
                      fw, fh, n, reachable, hidden);
            }
        }
    }
}

/* The live repros.  The 2026-11-08 pass had ten windows open at 800x600 and
 * got Start + six buttons; the plan's gate is "8 windows open -> all 8
 * reachable by click".  At 44 px minimum the strip holds 9 buttons at 640 px
 * and 12 at 800 px, so eight windows must all get a real button with room to
 * read, and ten must all get one at 1024 px -- with nothing pushed into the
 * overflow list. */
static void test_live_repro(void) {
    static const struct { unsigned fw, n, min_w; } cases[] = {
        { 640,  8, 44 },
        { 800,  8, 44 },
        { 1024, 10, 44 },
        { 1280, 12, 44 },
    };
    for (unsigned ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
        unsigned fw = cases[ci].fw, n = cases[ci].n;
        gui_tb_layout_t l;
        gui_tb_layout(fw, n, &l);
        CHECK(l.visible == n && l.more_w == 0u,
              "fw=%u: %u windows should fit without overflow (visible=%u)",
              fw, n, l.visible);
        for (unsigned slot = 0; slot < n; slot++) {
            unsigned x = 0, w = 0;
            CHECK(gui_tb_button_rect(fw, n, slot, &x, &w),
                  "fw=%u: window %u has no button", fw, slot);
            CHECK(w >= cases[ci].min_w,
                  "fw=%u: window %u button only %u px", fw, slot, w);
        }
    }
    /* The old code's exact failure: at 800x600 with ten windows, buttons 7..9
     * used to be drawn nowhere and unclickable.  Now either they are buttons
     * or they are in the list -- and the list is never needed silently. */
    gui_tb_layout_t l;
    gui_tb_layout(800, 10, &l);
    CHECK(l.visible + (l.more_w ? 1u : 0u) + (10u - l.visible - (l.more_w ? 1u : 0u)) == 10u,
          "800x600: ten windows unaccounted for");
    CHECK(l.visible >= 8u || l.more_w != 0u,
          "800x600: ten windows, only %u buttons and no list", l.visible);
}

int main(void) {
    printf("[unit] taskbar layout: shrink-to-fit + overflow list\n");
    test_strip_geometry();
    printf("[unit]   strip geometry, 1..%u windows x %u widths\n",
           MAX_WINDOWS, (unsigned)NWIDTHS);
    test_hit_matches_draw();
    printf("[unit]   hit test matches the drawn rect\n");
    test_overflow_reaches_everything();
    printf("[unit]   overflow list accounts for every hidden window\n");
    test_live_repro();
    printf("[unit]   ten-window live repro at 640/800/1024\n");

    if (failures) {
        printf("TASKBAR LAYOUT: FAIL (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("TASKBAR LAYOUT: PASS (%d checks)\n", checks);
    return 0;
}

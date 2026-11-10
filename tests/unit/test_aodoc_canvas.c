/* tests/unit/test_aodoc_canvas.c — the dirty-rect canvas (OFFICE_PLAN.md OF0).
 *
 * The canvas is the reason a keystroke costs 10 µs instead of 3 ms: it turns
 * "what did I paint?" into a small set of rects that the compositor is asked
 * to re-composite.  It is also the part of the suite that fails SILENTLY when
 * it is wrong — a missing rect is a stale pixel, and a rect that is too small
 * is a half-drawn letter, neither of which shows up as a crash.
 *
 * So the cases below are about the rules, not the drawing: clipping to the
 * content box, merging touching rects (a caret move must be ONE rect, not
 * two), refusing to merge rects with a gap, saturating to the whole box when
 * the set cannot shrink, and never letting a plan disturb the canvas that is
 * being drawn.
 *
 * All arithmetic is on small numbers, so every expected value below is written
 * out literally and can be checked by reading it.
 */

#include <stdio.h>
#include <string.h>

#include "userspace/office/libaodoc/aodoc.h"

static int checks = 0, failures = 0;

static void ck(const char *what, int cond) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static int rect_is(const ao_rect_t *r, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    return r->x == x && r->y == y && r->w == w && r->h == h;
}

/* ---- the empty canvas and the first mark --------------------------------- */
static void test_empty_and_one(void) {
    ao_canvas_t c;
    ao_rect_t out[AO_CANVAS_MAX_RECTS];

    ao_canvas_init(&c, 100, 80);
    ck("empty: nothing to plan", ao_canvas_plan(&c, out, 8) == 0);
    ck("empty: no marks counted", c.marks == 0);
    ck("empty: not saturated", c.saturated == 0);

    ao_canvas_mark(&c, 10, 20, 30, 6);
    ck("one: a single mark is one rect", ao_canvas_plan(&c, out, 8) == 1);
    ck("one: the rect is the mark, unchanged", rect_is(&out[0], 10, 20, 30, 6));
    ck("one: the mark counter moved", c.marks == 1);

    /* The plan must not consume the canvas: two calls in a row are identical,
     * because painting is what clears it. */
    ao_rect_t again[AO_CANVAS_MAX_RECTS];
    ck("one: the plan is repeatable", ao_canvas_plan(&c, again, 8) == 1 &&
                                      rect_is(&again[0], 10, 20, 30, 6));

    ao_canvas_clear(&c);
    ck("clear: nothing left to plan", ao_canvas_plan(&c, out, 8) == 0);
    ck("clear: the mark counter keeps counting (it is a counter, not a set)",
       c.marks == 1);
}

/* ---- clipping ------------------------------------------------------------ */
static void test_clipping(void) {
    ao_canvas_t c;
    ao_rect_t out[AO_CANVAS_MAX_RECTS];

    ao_canvas_init(&c, 100, 80);

    /* Off the left/top edge: clipped to the box, not dropped. */
    ao_canvas_mark(&c, -10, -4, 20, 10);
    ck("clip: a rect over the top-left corner becomes 10x6 at 0,0",
       ao_canvas_plan(&c, out, 8) == 1 && rect_is(&out[0], 0, 0, 10, 6));

    /* Off the right/bottom edge. */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 90, 70, 40, 40);
    ck("clip: a rect over the bottom-right corner becomes 10x10 at 90,70",
       ao_canvas_plan(&c, out, 8) == 1 && rect_is(&out[0], 90, 70, 10, 10));

    /* Entirely outside: nothing recorded, but the call still counted. */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 200, 200, 10, 10);
    ck("clip: a rect outside the box plans nothing", ao_canvas_plan(&c, out, 8) == 0);
    ck("clip: ... but the call was still counted", c.marks == 3);

    /* Zero and negative extents are not marks. */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 10, 10, 0, 10);
    ao_canvas_mark(&c, 10, 10, 10, 0);
    ao_canvas_mark(&c, 10, 10, (uint32_t)-5, 10);
    ck("clip: zero/negative extents record nothing", ao_canvas_plan(&c, out, 8) == 0);

    /* A rect that ends exactly on the edge is fully inside, not clipped away. */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 0, 0, 100, 80);
    ck("clip: the whole box is exactly the whole box",
       ao_canvas_plan(&c, out, 8) == 1 && rect_is(&out[0], 0, 0, 100, 80));

    /* A zero-sized box has nothing to paint. */
    ao_canvas_init(&c, 0, 0);
    ao_canvas_mark(&c, 0, 0, 10, 10);
    ck("clip: a zero-sized box plans nothing", ao_canvas_plan(&c, out, 8) == 0);
    ao_canvas_mark_all(&c);
    ck("clip: ... even when saturated", ao_canvas_plan(&c, out, 8) == 0);
}

/* ---- merging ------------------------------------------------------------- */
static void test_merging(void) {
    ao_canvas_t c;
    ao_rect_t out[AO_CANVAS_MAX_RECTS];

    ao_canvas_init(&c, 200, 100);

    /* The case the canvas exists for: the caret left one cell and entered the
     * next one.  Two marks, ONE rect, because they share an edge. */
    ao_canvas_mark(&c, 10, 10, 2, 14);
    ao_canvas_mark(&c, 12, 10, 2, 14);
    ck("merge: a caret move is one rect, not two", ao_canvas_plan(&c, out, 8) == 1);
    ck("merge: ... the union of both cells", rect_is(&out[0], 10, 10, 4, 14));

    /* A one-pixel gap must NOT merge: the pixels between were never painted. */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 10, 10, 2, 14);
    ao_canvas_mark(&c, 13, 10, 2, 14);
    ck("merge: a one-pixel gap keeps two rects", ao_canvas_plan(&c, out, 8) == 2);

    /* Overlapping rects coalesce into their bounding box. */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 10, 10, 20, 20);
    ao_canvas_mark(&c, 25, 25, 20, 20);
    ck("merge: overlap becomes one rect", ao_canvas_plan(&c, out, 8) == 1);
    ck("merge: ... the bounding box of both", rect_is(&out[0], 10, 10, 35, 35));

    /* Touching at a corner counts as touching, which is the conservative
     * choice: the corner pixel is composited twice, never zero times. */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 10, 10, 5, 5);
    ao_canvas_mark(&c, 15, 15, 5, 5);
    ck("merge: a shared corner merges", ao_canvas_plan(&c, out, 8) == 1);
    ck("merge: ... into 10x10", rect_is(&out[0], 10, 10, 10, 10));

    /* A chain of three: A touches B, B touches C, so all three become one. */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 0, 0, 10, 10);
    ao_canvas_mark(&c, 10, 0, 10, 10);
    ao_canvas_mark(&c, 20, 0, 10, 10);
    ck("merge: a chain collapses to one rect", ao_canvas_plan(&c, out, 8) == 1);
    ck("merge: ... spanning all three", rect_is(&out[0], 0, 0, 30, 10));

    /* Vertical neighbours, the line-joined case. */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 5, 10, 40, 16);
    ao_canvas_mark(&c, 5, 26, 40, 16);
    ck("merge: two stacked lines become one band", ao_canvas_plan(&c, out, 8) == 1);
    ck("merge: ... 40x32", rect_is(&out[0], 5, 10, 40, 32));

    /* Disjoint rects stay disjoint: the plan is not allowed to over-paint when
     * it can avoid it (that is the whole point of the phase's budget). */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 0, 0, 10, 10);
    ao_canvas_mark(&c, 90, 60, 10, 10);
    ck("merge: far-apart rects stay two", ao_canvas_plan(&c, out, 8) == 2);
    int seen_a = 0, seen_b = 0;
    for (int i = 0; i < 2; i++) {
        if (rect_is(&out[i], 0, 0, 10, 10)) seen_a = 1;
        if (rect_is(&out[i], 90, 60, 10, 10)) seen_b = 1;
    }
    ck("merge: ... unchanged, not just counted", seen_a && seen_b);
}

/* ---- saturation and the slot cap ----------------------------------------- */
static void test_cap_and_saturation(void) {
    ao_canvas_t c;
    ao_rect_t out[AO_CANVAS_MAX_RECTS];

    ao_canvas_init(&c, 200, 100);

    ao_canvas_mark_all(&c);
    ck("saturate: the whole box is planned",
       ao_canvas_plan(&c, out, 8) == 1 && rect_is(&out[0], 0, 0, 200, 100));
    ck("saturate: a saturated canvas is flagged", c.saturated == 1);

    /* Later marks are absorbed: no point in tracking rects under a full repaint. */
    ao_canvas_mark(&c, 10, 10, 2, 2);
    ck("saturate: a mark after saturation still plans the whole box",
       ao_canvas_plan(&c, out, 8) == 1 && rect_is(&out[0], 0, 0, 200, 100));

    ao_canvas_clear(&c);
    ck("saturate: clear un-saturates", c.saturated == 0);

    /* Fill every slot with disjoint rects, then add one more.  The set must
     * stay bounded AND still cover the new rect: the ninth merges into the
     * last slot rather than being dropped (a dropped rect is a stale pixel). */
    for (int i = 0; i < AO_CANVAS_MAX_RECTS; i++)
        ao_canvas_mark(&c, i * 20, 0, 4, 4);
    ck("cap: eight disjoint rects fit", ao_canvas_plan(&c, out, 16) == 8);
    ao_canvas_mark(&c, 170, 90, 4, 4);
    ck("cap: the canvas never grows past its slots",
       c.n == AO_CANVAS_MAX_RECTS);
    int covered = 0;
    int n = ao_canvas_plan(&c, out, 16);
    for (int i = 0; i < n; i++)
        if (out[i].x <= 170 && out[i].x + (int32_t)out[i].w >= 174 &&
            out[i].y <= 90 && out[i].y + (int32_t)out[i].h >= 94) covered = 1;
    ck("cap: the overflow mark is still covered by the plan", covered);

    /* max is a budget, not a hint: when the merged set does not fit, the plan
     * is ONE rect that covers everything. */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 0, 0, 10, 10);
    ao_canvas_mark(&c, 50, 0, 10, 10);
    ao_canvas_mark(&c, 100, 0, 10, 10);
    ck("max: three rects against a budget of two",
       ao_canvas_plan(&c, out, 2) == 1);
    ck("max: ... collapse to a covering rect", rect_is(&out[0], 0, 0, 110, 10));
    ck("max: ... which is what 'correct but slower' looks like",
       ao_canvas_plan(&c, out, 3) == 3);

    /* A budget of zero plans nothing rather than writing out of bounds. */
    ck("max: a zero budget plans nothing", ao_canvas_plan(&c, out, 0) == 0);
    ck("max: a negative budget plans nothing", ao_canvas_plan(&c, out, -1) == 0);
    ck("max: a null output plans nothing", ao_canvas_plan(&c, NULL, 8) == 0);
}

/* ---- ao_rect_touches ----------------------------------------------------- */
static void test_touches(void) {
    ao_rect_t a = { 0, 0, 10, 10 };
    ao_rect_t b = { 10, 0, 10, 10 };     /* shares the right edge */
    ao_rect_t c = { 11, 0, 10, 10 };     /* one pixel to the right */
    ao_rect_t d = { 0, 10, 10, 10 };     /* shares the bottom edge */
    ao_rect_t e = { 10, 10, 10, 10 };    /* shares the corner */
    ao_rect_t f = { 5, 5, 10, 10 };      /* overlaps */
    ao_rect_t g = { -10, -10, 10, 10 };  /* touches at 0,0 */

    ck("touches: an edge is touching", ao_rect_touches(&a, &b));
    ck("touches: symmetry holds", ao_rect_touches(&b, &a));
    ck("touches: a gap is not touching", !ao_rect_touches(&a, &c));
    ck("touches: the bottom edge counts", ao_rect_touches(&a, &d));
    ck("touches: a corner counts", ao_rect_touches(&a, &e));
    ck("touches: overlap counts", ao_rect_touches(&a, &f));
    ck("touches: a rect ending where this one starts counts",
       ao_rect_touches(&a, &g));
    ck("touches: a rect always touches itself", ao_rect_touches(&a, &a));
    ck("touches: NULL is not a touch", !ao_rect_touches(&a, NULL));
}

/* ---- the shapes the editor actually produces ----------------------------- */
static void test_editor_shapes(void) {
    ao_canvas_t c;
    ao_rect_t out[AO_CANVAS_MAX_RECTS];

    /* A keystroke in the middle of a line: the caret moves two columns, and
     * the line it is on is marked.  Deliberately NOT one rect: the band and
     * the caret can be far apart vertically... except here they overlap, so
     * this is one rect — the point is that the plan is bounded and covers
     * both. */
    ao_canvas_init(&c, 560, 300);
    ao_canvas_mark(&c, 40, 76, 480, 16);   /* the line band */
    ao_canvas_mark(&c, 122, 78, 2, 14);    /* the caret inside it */
    int n = ao_canvas_plan(&c, out, 8);
    ck("editor: a keystroke plans one rect", n == 1);
    ck("editor: ... the line band", rect_is(&out[0], 40, 76, 480, 16));

    /* A caret move: two cells, one rect of two cells' width. */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 120, 76, 2, 14);
    ao_canvas_mark(&c, 122, 76, 2, 14);
    ck("editor: a caret move plans 4x14, not the page",
       ao_canvas_plan(&c, out, 8) == 1 && rect_is(&out[0], 120, 76, 4, 14));

    /* A newline: two lines plus the caret on the new one, all touching. */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 40, 92, 480, 16);   /* new line band */
    ao_canvas_mark(&c, 40, 94, 2, 14);     /* caret on it */
    ck("editor: a newline plans its own band",
       ao_canvas_plan(&c, out, 8) == 1 && rect_is(&out[0], 40, 92, 480, 16));

    /* Scrolling by one line: the whole band moves, which is the expensive
     * frame — still one rect, and still not the window. */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 40, 60, 480, 240);
    ck("editor: a page scroll is one band",
       ao_canvas_plan(&c, out, 8) == 1 && rect_is(&out[0], 40, 60, 480, 240));

    /* And the number the budget gate quotes: a caret rect is 2x14 = 28 px of
     * a 560x300 content box. */
    ao_canvas_clear(&c);
    ao_canvas_mark(&c, 100, 100, 2, 14);
    n = ao_canvas_plan(&c, out, 8);
    ck("editor: the caret rect is 2x14", n == 1 && out[0].w == 2 && out[0].h == 14);
    ck("editor: ... 28 pixels of 168000 (< 0.02 %)",
       (long)out[0].w * (long)out[0].h == 28 && 560L * 300L == 168000L);
}

int main(void) {
    test_empty_and_one();
    test_clipping();
    test_merging();
    test_cap_and_saturation();
    test_touches();
    test_editor_shapes();

    printf("AO CANVAS: %s (%d checks)\n", failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}

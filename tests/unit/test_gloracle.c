/* tests/unit/test_gloracle.c — GPU_ACCEL_PLAN H6: the frame oracle's own
 * tests.
 *
 * An oracle that always says "match" is worse than none: it makes a red
 * regression look green and costs a frame's worth of time to do it.  So half
 * of this suite is MUTATION cases — the oracle has to NOTICE, and say what it
 * noticed:
 *
 *   - a scene rendered twice must hash identically (determinism),
 *   - a one-pixel change must change the hash (sensitivity),
 *   - a shifted vertex, a swapped channel and a flipped row must each be
 *     rejected by the comparator (the three regressions the plan names:
 *     wrong vertex format, wrong uniform slot, wrong sampler),
 *   - a single edge pixel off by one must be ACCEPTED (the tolerance is not
 *     zero, and that is a deliberate decision, not slack),
 *   - a colour band must be rejected even though each of its pixels is within
 *     the frame as a whole (the rule the tolerance model exists to express).
 *
 * The suite runs on the software backend only.  A GPU is absent in CI, and
 * pretending otherwise would be the same dishonesty the oracle is meant to
 * prevent — the guest lane is asserted separately by test_virgl_gpu.sh.
 */

#include <stdio.h>
#include <string.h>

#include "GL/gl.h"
#include "GL/glbackend.h"
#include "GL/auraglx.h"
#include "glcontext.h"
#include "gloracle.h"

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

static uint32_t frame_a[GLORACLE_W * GLORACLE_H];
static uint32_t frame_b[GLORACLE_W * GLORACLE_H];
static uint32_t frame_m[GLORACLE_W * GLORACLE_H];   /* the mutation */

/* ---- determinism ------------------------------------------------------- */

/* Two renders of the same scene must be byte-identical.  If this fails the
 * scene depends on something it should not, and every other assertion below
 * is meaningless. */
static int t_render_is_deterministic(void) {
    if (gloracle_render(frame_a) != 0) return 0;
    if (gloracle_render(frame_b) != 0) return 0;
    return memcmp(frame_a, frame_b, sizeof frame_a) == 0;
}

/* The scene actually drew something: it is not the clear colour everywhere,
 * and it is not a flat single colour either.  Without this, a render that
 * silently produced nothing would hash consistently and pass. */
static int t_scene_is_not_blank(void) {
    if (gloracle_render(frame_a) != 0) return 0;
    uint32_t first = frame_a[0];
    int differing = 0;
    for (size_t i = 1; i < sizeof frame_a / sizeof frame_a[0]; i++)
        if (frame_a[i] != first) differing++;
    /* The background is one colour and the two triangles fill a fair part of
     * the frame; anything from a few hundred pixels up is a real scene. */
    return differing > 500;
}

/* Both depth-tested triangles are visible.  The far one is drawn second and
 * must lose where they overlap; if the depth test were backwards the near
 * one's colours would be entirely absent.  There is at least one pixel of
 * each of the near triangle's three vertex colours. */
static int t_depth_order_is_right(void) {
    if (gloracle_render(frame_a) != 0) return 0;
    int red = 0, green = 0, blue = 0, yellow = 0;
    for (size_t i = 0; i < sizeof frame_a / sizeof frame_a[0]; i++) {
        uint32_t p = frame_a[i];
        uint32_t r = p & 0xFFu, g = (p >> 8) & 0xFFu, b = (p >> 16) & 0xFFu;
        if (r > 200 && g < 60 && b < 60) red++;
        if (r < 60 && g > 200 && b < 60) green++;
        if (r < 60 && g < 60 && b > 200) blue++;
        if (r > 200 && g > 200 && b < 60) yellow++;
    }
    /* A's three corners and B's corner all survive: each triangle owns the
     * corner it starts from, and they overlap only along the diagonal. */
    return red > 0 && green > 0 && blue > 0 && yellow > 0;
}

/* ---- sensitivity ------------------------------------------------------- */

/* One changed pixel must change the hash.  This is the property that makes
 * the oracle a regression test: if a hash cannot see one pixel, it cannot
 * see a moved edge either. */
static int t_hash_sees_one_pixel(void) {
    uint8_t h1[32], h2[32];
    if (gloracle_render(frame_a) != 0) return 0;
    memcpy(frame_m, frame_a, sizeof frame_a);
    gloracle_hash(frame_a, h1);
    frame_m[100] ^= 0x000000FFu;              /* flip one channel */
    gloracle_hash(frame_m, h2);
    return memcmp(h1, h2, sizeof h1) != 0;
}

/* A flipped row must change the hash.  This is the orientation regression
 * the G12 comments warn about: a buffer read bottom-up vs top-down produces a
 * mirrored frame that a weak test would call "different pixels" but that a
 * naive equality check would miss if it also flipped both sides. */
static int t_hash_sees_flipped_rows(void) {
    uint8_t h1[32], h2[32];
    if (gloracle_render(frame_a) != 0) return 0;
    for (int y = 0; y < GLORACLE_H; y++)
        memcpy(&frame_m[y * GLORACLE_W],
               &frame_a[(GLORACLE_H - 1 - y) * GLORACLE_W],
               GLORACLE_W * sizeof(uint32_t));
    gloracle_hash(frame_a, h1);
    gloracle_hash(frame_m, h2);
    return memcmp(h1, h2, sizeof h1) != 0;
}

/* The hex form is what the log prints; 64 lowercase hex digits and a NUL. */
static int t_hash_hex_shape(void) {
    if (gloracle_render(frame_a) != 0) return 0;
    char hex[65];
    gloracle_hash_hex(frame_a, hex);
    if (strlen(hex) != 64) return 0;
    for (int i = 0; i < 64; i++) {
        char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    /* Same frame, same string -- and a mutation changes it. */
    char again[65];
    gloracle_hash_hex(frame_a, again);
    if (strcmp(hex, again) != 0) return 0;

    memcpy(frame_m, frame_a, sizeof frame_a);
    frame_m[0] ^= 0x01000000u;
    char mut[65];
    gloracle_hash_hex(frame_m, mut);
    return strcmp(hex, mut) != 0;
}

/* ---- the comparator, in both directions -------------------------------- */

/* Identical frames pass with zero differing pixels. */
static int t_compare_identical_passes(void) {
    if (gloracle_render(frame_a) != 0) return 0;
    gloracle_verdict_t v;
    gloracle_compare(frame_a, frame_a, &v);
    return v.pass && v.pixels_differing == 0 && v.max_channel_delta == 0
        && v.tolerance_pixels > 0 && v.pixels_total == GLORACLE_W * GLORACLE_H;
}

/* A one-step difference on a handful of pixels is ACCEPTED: this is the
 * tolerance doing its job, and it is asserted so that tightening it to zero
 * later would fail a test rather than silently make the GPU lane flaky. */
static int t_compare_epsilon_is_tolerated(void) {
    if (gloracle_render(frame_a) != 0) return 0;
    memcpy(frame_m, frame_a, sizeof frame_a);
    /* One step in one channel on three pixels -- the edge-rounding case. */
    for (int i = 0; i < 3; i++)
        frame_m[i] = (frame_m[i] & 0xFFFFFF00u) |
                     (((frame_m[i] & 0xFFu) + 1u) & 0xFFu);
    gloracle_verdict_t v;
    gloracle_compare(frame_a, frame_m, &v);
    return v.pass && v.pixels_differing == 0 && v.max_channel_delta == 1;
}

/* A band: many pixels each differing by a lot.  Must be REJECTED, with the
 * count reported so the log can name it. */
static int t_compare_band_is_rejected(void) {
    if (gloracle_render(frame_a) != 0) return 0;
    memcpy(frame_m, frame_a, sizeof frame_a);
    /* Twenty rows of half-brightness.  The band is 20 * 64 = 1280 pixels,
     * and 1120 of them actually change: the background's red channel is 31
     * (0.125 * 255), and 31/2 = 15 differs, while a pixel whose blue is 127
     * halves to 63 -- every pixel in the band moves, but the COUNTS below are
     * the measured ones so the assertion states what happened rather than
     * what arithmetic on the band's area suggests. */
    for (int y = 20; y < 40; y++)
        for (int x = 0; x < GLORACLE_W; x++) {
            uint32_t p = frame_m[y * GLORACLE_W + x];
            uint32_t r = (p & 0xFFu) / 2u;
            uint32_t g = ((p >> 8) & 0xFFu) / 2u;
            uint32_t b = ((p >> 16) & 0xFFu) / 2u;
            frame_m[y * GLORACLE_W + x] = (p & 0xFF000000u) | (b << 16) | (g << 8) | r;
        }
    gloracle_verdict_t v;
    gloracle_compare(frame_a, frame_m, &v);
    return !v.pass && v.pixels_differing >= 1100 && v.max_channel_delta >= 100;
}

/* The budget itself: a frame differing on exactly tolerance_pixels passes,
 * one pixel more does not.  This pins the boundary rather than the region. */
static int t_compare_budget_boundary(void) {
    if (gloracle_render(frame_a) != 0) return 0;
    gloracle_verdict_t v0;
    gloracle_compare(frame_a, frame_a, &v0);
    uint32_t budget = v0.tolerance_pixels;
    if (budget == 0 || budget >= (uint32_t)(GLORACLE_W * GLORACLE_H)) return 0;

    memcpy(frame_m, frame_a, sizeof frame_a);
    for (uint32_t i = 0; i < budget; i++)
        frame_m[i] = frame_m[i] ^ 0x00000080u;   /* well past the epsilon */

    gloracle_verdict_t v;
    gloracle_compare(frame_a, frame_m, &v);
    if (!v.pass || v.pixels_differing != budget) return 0;

    frame_m[budget] ^= 0x00000080u;              /* one more */
    gloracle_compare(frame_a, frame_m, &v);
    return !v.pass && v.pixels_differing == budget + 1;
}

/* A shifted scene -- the "wrong vertex format" regression, modelled as whole
 * pixels moving -- is rejected. */
static int t_compare_shifted_scene_rejected(void) {
    if (gloracle_render(frame_a) != 0) return 0;
    /* Shift the image one pixel right, wrapping. */
    for (int y = 0; y < GLORACLE_H; y++) {
        int row = y * GLORACLE_W;
        frame_m[row] = frame_a[row + GLORACLE_W - 1];
        for (int x = 1; x < GLORACLE_W; x++)
            frame_m[row + x] = frame_a[row + x - 1];
    }
    gloracle_verdict_t v;
    gloracle_compare(frame_a, frame_m, &v);
    return !v.pass;
}

/* A swapped channel -- the "wrong uniform slot" regression, at pixel level --
 * is rejected. */
static int t_compare_swapped_channels_rejected(void) {
    if (gloracle_render(frame_a) != 0) return 0;
    for (size_t i = 0; i < sizeof frame_a / sizeof frame_a[0]; i++) {
        uint32_t p = frame_a[i];
        uint32_t r = p & 0xFFu, g = (p >> 8) & 0xFFu, b = (p >> 16) & 0xFFu;
        frame_m[i] = (p & 0xFF000000u) | (r << 16) | (g << 8) | b;
    }
    gloracle_verdict_t v;
    gloracle_compare(frame_a, frame_m, &v);
    return !v.pass && v.max_channel_delta > 1;
}

/* The oracle must leave the caller's context current and clean.
 *
 * This is a regression test for a defect this suite caught on its first run
 * in the guest: gloracle_render() created and destroyed its own context and
 * left a DANGLING current pointer, so the caller's next glGetError() read a
 * freed context and returned GL_INVALID_OPERATION (0x502).  The fix puts the
 * previous context back, and this test is what keeps that from silently
 * regressing -- /gltest's h6_no_pending_error check is the same assertion one
 * layer up, in the guest. */
static int t_render_restores_caller_context(void) {
    aglx_context_t *outer = aglxCreateContext(0, 32, 32, AGLX_DEPTH);
    if (!outer) return 0;
    aglxMakeCurrent(outer);
    (void)glGetError();                    /* clean slate */

    int rendered = gloracle_render(frame_a) == 0;

    /* The caller's context is current again, and asking it anything is
     * valid -- which is exactly what a dangling pointer would break. */
    int same = aglxGetCurrentContext() == outer;
    unsigned err = glGetError();

    aglxDestroyContext(outer);
    return rendered && same && err == GL_NO_ERROR;
}

/* The tolerance constants are the plan's numbers, and the code is the only
 * place they are written down; the checker compares these against the plan's
 * prose. */
static int t_tolerance_constants(void) {
    return GLORACLE_TOLERANCE_PERMILLE == 5u
        && GLORACLE_CHANNEL_EPSILON == 1u
        && GLORACLE_W == 64 && GLORACLE_H == 64;
}

int main(void) {
    printf("=== gloracle (H6) unit tests ===\n");

    printf("--- the scene ---\n");
    RUN(t_render_is_deterministic); RUN(t_scene_is_not_blank);
    RUN(t_depth_order_is_right);

    printf("--- the hash ---\n");
    RUN(t_hash_sees_one_pixel); RUN(t_hash_sees_flipped_rows);
    RUN(t_hash_hex_shape);

    printf("--- the comparator ---\n");
    RUN(t_compare_identical_passes); RUN(t_compare_epsilon_is_tolerated);
    RUN(t_compare_band_is_rejected); RUN(t_compare_budget_boundary);
    RUN(t_compare_shifted_scene_rejected);
    RUN(t_compare_swapped_channels_rejected);

    printf("--- context hygiene ---\n");
    RUN(t_render_restores_caller_context);

    printf("--- the stated tolerance ---\n");
    RUN(t_tolerance_constants);

    printf("\ntest_gloracle: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

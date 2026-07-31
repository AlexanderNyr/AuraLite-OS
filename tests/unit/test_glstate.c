/*
 * test_glstate.c — host-side unit tests for the GL context and state machine.
 *
 * Links the REAL libgl/src/auraglx.c and libgl/src/glstate.c (see the Makefile
 * rule), with a recording stub standing in for libauragui so the presentation
 * path can be observed without a kernel.
 *
 * Focus areas:
 *   - context lifecycle and buffer ownership
 *   - GL's "first error wins, reading clears" contract (§2.5)
 *   - glClear semantics, including the invalid-mask case that must clear
 *     nothing at all
 *   - what aglxSwapBuffers() actually hands to the window
 */

#include <stdio.h>
#include <string.h>

#include "GL/gl.h"
#include "GL/auraglx.h"
#include "glcontext.h"
#include "auragui.h"

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    ag_stub_reset();                                    \
    aglxMakeCurrent(NULL);                              \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

#define W 64
#define H 32

/* ------------------------------------------------------------- lifecycle -- */

static int t_create_destroy(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    int ok = aglxGetWidth(c) == W && aglxGetHeight(c) == H
          && aglxGetColorBuffer(c) != NULL
          && aglxGetDepthBuffer(c) != NULL;
    aglxDestroyContext(c);
    return ok;
}

/* Without AGLX_DEPTH there must be no depth buffer — that is the whole point
 * of the flag, since depth costs width*height*4 bytes. */
static int t_create_no_depth(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, 0);
    if (!c) return 0;
    int ok = aglxGetColorBuffer(c) != NULL && aglxGetDepthBuffer(c) == NULL;
    aglxDestroyContext(c);
    return ok;
}

static int t_create_rejects_bad_args(void) {
    return aglxCreateContext(1, 0, H, AGLX_DEFAULT) == NULL
        && aglxCreateContext(1, W, 0, AGLX_DEFAULT) == NULL
        && aglxCreateContext(1, -4, H, AGLX_DEFAULT) == NULL
        && aglxCreateContext(-1, W, H, AGLX_DEFAULT) == NULL
        && aglxCreateContext(1, AGLX_MAX_DIM + 1, H, AGLX_DEFAULT) == NULL;
}

/* Buffers must start defined: colour cleared to black, depth to the far
 * plane.  An application that swaps before drawing must not see heap junk. */
static int t_buffers_initialised(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    const uint32_t *col = aglxGetColorBuffer(c);
    const float *dep = aglxGetDepthBuffer(c);
    int ok = 1;
    for (int i = 0; i < W * H; i++) {
        if (col[i] != 0) { ok = 0; break; }
        if (dep[i] != 1.0f) { ok = 0; break; }
    }
    aglxDestroyContext(c);
    return ok;
}

static int t_destroy_null_safe(void) {
    aglxDestroyContext(NULL);   /* must not crash */
    return 1;
}

/* Destroying the current context must unbind it, otherwise later GL calls
 * would dereference freed memory. */
static int t_destroy_unbinds(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    aglxDestroyContext(c);
    return aglxGetCurrentContext() == NULL;
}

static int t_make_current(void) {
    aglx_context_t *c = aglxCreateContext(3, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    int ok = aglxMakeCurrent(c) == 0 && aglxGetCurrentContext() == c;
    ok = ok && aglxMakeCurrent(NULL) == 0 && aglxGetCurrentContext() == NULL;
    aglxDestroyContext(c);
    return ok;
}

static int t_resize(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    int ok = aglxResize(c, 128, 96) == 0
          && aglxGetWidth(c) == 128 && aglxGetHeight(c) == 96
          && aglxGetColorBuffer(c) != NULL;
    /* The new buffers must also be initialised. */
    const uint32_t *col = aglxGetColorBuffer(c);
    for (int i = 0; ok && i < 128 * 96; i++) if (col[i] != 0) ok = 0;
    aglxDestroyContext(c);
    return ok;
}

static int t_resize_rejects_bad(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    int ok = aglxResize(c, 0, 10) != 0
          && aglxResize(c, 10, -1) != 0
          && aglxResize(c, AGLX_MAX_DIM + 1, 10) != 0
          /* A rejected resize must leave the context untouched. */
          && aglxGetWidth(c) == W && aglxGetHeight(c) == H;
    aglxDestroyContext(c);
    return ok;
}

/* Resizing to the current size is a no-op that must still succeed. */
static int t_resize_same_size(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    int ok = aglxResize(c, W, H) == 0 && aglxGetWidth(c) == W;
    aglxDestroyContext(c);
    return ok;
}

/* ----------------------------------------------------------------- errors -- */

static int t_error_initially_none(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    int ok = glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* An undefined bit in the clear mask is GL_INVALID_VALUE. */
static int t_error_invalid_clear_mask(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClear(0xDEADBEEF);
    int ok = glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

/* Reading the error clears it (§2.5). */
static int t_error_cleared_on_read(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClear(0xDEADBEEF);
    (void)glGetError();
    int ok = glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* The FIRST error is kept, so later noise cannot mask the original cause. */
static int t_error_first_wins(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClear(0xDEADBEEF);              /* GL_INVALID_VALUE  */
    (void)glGetString(0x9999);        /* GL_INVALID_ENUM   */
    int ok = glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

static int t_error_invalid_viewport(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glViewport(0, 0, -1, 10);
    int ok = glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

/* Calling GL with no current context must be diagnosable, not a crash. */
static int t_error_no_context(void) {
    aglxMakeCurrent(NULL);
    glClear(GL_COLOR_BUFFER_BIT);     /* must not crash */
    glClearColor(1, 1, 1, 1);         /* must not crash */
    glViewport(0, 0, 10, 10);         /* must not crash */
    return glGetError() == GL_INVALID_OPERATION;
}

/* ---------------------------------------------------------------- strings -- */

static int t_get_string(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *ver    = (const char *)glGetString(GL_VERSION);
    const char *rend   = (const char *)glGetString(GL_RENDERER);
    const char *ext    = (const char *)glGetString(GL_EXTENSIONS);
    int ok = vendor && strcmp(vendor, "AuraLite OS") == 0
          && ver && strstr(ver, "1.1") != NULL
          && rend && strlen(rend) > 0
          /* Must be an empty string, never NULL: callers tokenise this. */
          && ext && ext[0] == '\0'
          && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_get_string_invalid(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    const GLubyte *s = glGetString(0x1234);
    int ok = s == NULL && glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

/* ----------------------------------------------------------------- clear -- */

static int t_clear_color_black(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const uint32_t *col = aglxGetColorBuffer(c);
    int ok = 1;
    for (int i = 0; i < W * H; i++) if (col[i] != 0x000000) { ok = 0; break; }
    aglxDestroyContext(c);
    return ok;
}

/* 1.0f must round to 255, not 254 — the classic off-by-one in colour packing. */
static int t_clear_color_white(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const uint32_t *col = aglxGetColorBuffer(c);
    int ok = 1;
    for (int i = 0; i < W * H; i++) if (col[i] != 0xFFFFFF) { ok = 0; break; }
    aglxDestroyContext(c);
    return ok;
}

/* Channel order must be 0x00RRGGBB, matching the framebuffer layout. */
static int t_clear_channel_order(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int red_ok = aglxGetColorBuffer(c)[0] == 0xFF0000;
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int green_ok = aglxGetColorBuffer(c)[0] == 0x00FF00;
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int blue_ok = aglxGetColorBuffer(c)[0] == 0x0000FF;
    aglxDestroyContext(c);
    return red_ok && green_ok && blue_ok;
}

/* glClearColor clamps at specification time (GLclampf). */
static int t_clear_color_clamped(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearColor(5.0f, -3.0f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    uint32_t p = aglxGetColorBuffer(c)[0];
    /* 5.0 -> 255, -3.0 -> 0, 0.5 -> 127 or 128 depending on rounding. */
    uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
    int ok = r == 255 && g == 0 && (b == 127 || b == 128);
    aglxDestroyContext(c);
    return ok;
}

static int t_clear_depth(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearDepth(0.25);
    glClear(GL_DEPTH_BUFFER_BIT);
    const float *d = aglxGetDepthBuffer(c);
    int ok = 1;
    for (int i = 0; i < W * H; i++) if (d[i] != 0.25f) { ok = 0; break; }
    aglxDestroyContext(c);
    return ok;
}

/* Clearing colour must not disturb depth, and vice versa. */
static int t_clear_independent(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearDepth(0.5);
    glClear(GL_DEPTH_BUFFER_BIT);
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);      /* depth must survive this */
    const float *d = aglxGetDepthBuffer(c);
    int ok = d[0] == 0.5f && aglxGetColorBuffer(c)[0] == 0xFFFFFF;
    aglxDestroyContext(c);
    return ok;
}

/* An invalid mask must clear NOTHING (§4.2.3). */
static int t_clear_invalid_mask_no_effect(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | 0x8000);   /* invalid bit present */
    int ok = aglxGetColorBuffer(c)[0] == 0x000000;   /* still the initial black */
    aglxDestroyContext(c);
    return ok;
}

/* Clearing depth on a context without a depth buffer is a no-op, not a crash
 * and not an error. */
static int t_clear_depth_without_buffer(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, 0);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClear(GL_DEPTH_BUFFER_BIT);
    int ok = glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Clearing both buffers in one call must work. */
static int t_clear_combined_mask(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 1, 1);
    glClearDepth(0.75);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    int ok = aglxGetColorBuffer(c)[0] == 0x0000FF
          && aglxGetDepthBuffer(c)[0] == 0.75f
          && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* The clear must cover every pixel, including the very last one — catches
 * off-by-one errors in the unrolled loop. */
static int t_clear_covers_last_pixel(void) {
    /* 67 is deliberately not a multiple of 8 so the unrolled loop has a
     * remainder to handle. */
    aglx_context_t *c = aglxCreateContext(1, 67, 5, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const uint32_t *col = aglxGetColorBuffer(c);
    int ok = col[67 * 5 - 1] == 0xFFFFFF && col[0] == 0xFFFFFF;
    aglxDestroyContext(c);
    return ok;
}

/* -------------------------------------------------------------- viewport -- */

static int t_viewport_default_is_full_size(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    int ok = c->viewport_w == W && c->viewport_h == H
          && c->viewport_x == 0 && c->viewport_y == 0;
    aglxDestroyContext(c);
    return ok;
}

static int t_viewport_set(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glViewport(4, 6, 20, 10);
    int ok = c->viewport_x == 4 && c->viewport_y == 6
          && c->viewport_w == 20 && c->viewport_h == 10
          && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* A resize must reset the viewport to the new full size. */
static int t_viewport_reset_on_resize(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glViewport(1, 2, 3, 4);
    aglxResize(c, 100, 50);
    int ok = c->viewport_w == 100 && c->viewport_h == 50
          && c->viewport_x == 0 && c->viewport_y == 0;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------------ presentation -- */

static int t_swap_presents_buffer(void) {
    aglx_context_t *c = aglxCreateContext(7, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int rc = aglxSwapBuffers(c);
    int ok = rc == 0
          && ag_stub.calls == 1
          && ag_stub.last_wid == 7
          && ag_stub.last_w == (uint32_t)W
          && ag_stub.last_h == (uint32_t)H
          /* stride must equal width: the buffer is tightly packed */
          && ag_stub.last_stride == (uint32_t)W
          && ag_stub.last_x == 0 && ag_stub.last_y == 0
          /* and it must be the CLEARED contents that were handed over */
          && ag_stub.last_first_pixel == 0x0000FF
          /* the compositor must be nudged to show it */
          && ag_stub.renders == 1;
    aglxDestroyContext(c);
    return ok;
}

static int t_swap_null_safe(void) {
    return aglxSwapBuffers(NULL) != 0 && ag_stub.calls == 0;
}

/* A failing blit must be reported, not swallowed. */
static int t_swap_propagates_failure(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    ag_stub.fail_next = 1;
    int ok = aglxSwapBuffers(c) != 0;
    aglxDestroyContext(c);
    return ok;
}

/* Swapping must not require a current context: it is a window-system
 * operation on an explicit context, like glXSwapBuffers. */
static int t_swap_without_current(void) {
    aglx_context_t *c = aglxCreateContext(2, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(NULL);
    int ok = aglxSwapBuffers(c) == 0 && ag_stub.calls == 1;
    aglxDestroyContext(c);
    return ok;
}

/* Two contexts must be independent: drawing into one must not affect the
 * other.  This is what makes the context-based design worth having. */
static int t_two_contexts_independent(void) {
    aglx_context_t *a = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    aglx_context_t *b = aglxCreateContext(2, W, H, AGLX_DEFAULT);
    if (!a || !b) { aglxDestroyContext(a); aglxDestroyContext(b); return 0; }

    aglxMakeCurrent(a);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    aglxMakeCurrent(b);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    int ok = aglxGetColorBuffer(a)[0] == 0xFF0000
          && aglxGetColorBuffer(b)[0] == 0x00FF00;

    aglxDestroyContext(a);
    aglxDestroyContext(b);
    return ok;
}

/* Errors are per-context, not global. */
static int t_error_is_per_context(void) {
    aglx_context_t *a = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    aglx_context_t *b = aglxCreateContext(2, W, H, AGLX_DEFAULT);
    if (!a || !b) { aglxDestroyContext(a); aglxDestroyContext(b); return 0; }

    aglxMakeCurrent(a);
    glClear(0xDEADBEEF);            /* poison context a only */

    aglxMakeCurrent(b);
    int b_clean = glGetError() == GL_NO_ERROR;

    aglxMakeCurrent(a);
    int a_dirty = glGetError() == GL_INVALID_VALUE;

    aglxDestroyContext(a);
    aglxDestroyContext(b);
    return b_clean && a_dirty;
}

/* ------------------------------------------------------------------------ */

int main(void) {
    printf("=== glstate / auraglx unit tests ===\n");

    printf("--- context lifecycle ---\n");
    RUN(t_create_destroy); RUN(t_create_no_depth);
    RUN(t_create_rejects_bad_args); RUN(t_buffers_initialised);
    RUN(t_destroy_null_safe); RUN(t_destroy_unbinds); RUN(t_make_current);
    RUN(t_resize); RUN(t_resize_rejects_bad); RUN(t_resize_same_size);

    printf("--- errors ---\n");
    RUN(t_error_initially_none); RUN(t_error_invalid_clear_mask);
    RUN(t_error_cleared_on_read); RUN(t_error_first_wins);
    RUN(t_error_invalid_viewport); RUN(t_error_no_context);

    printf("--- strings ---\n");
    RUN(t_get_string); RUN(t_get_string_invalid);

    printf("--- clear ---\n");
    RUN(t_clear_color_black); RUN(t_clear_color_white);
    RUN(t_clear_channel_order); RUN(t_clear_color_clamped);
    RUN(t_clear_depth); RUN(t_clear_independent);
    RUN(t_clear_invalid_mask_no_effect); RUN(t_clear_depth_without_buffer);
    RUN(t_clear_combined_mask); RUN(t_clear_covers_last_pixel);

    printf("--- viewport ---\n");
    RUN(t_viewport_default_is_full_size); RUN(t_viewport_set);
    RUN(t_viewport_reset_on_resize);

    printf("--- presentation ---\n");
    RUN(t_swap_presents_buffer); RUN(t_swap_null_safe);
    RUN(t_swap_propagates_failure); RUN(t_swap_without_current);
    RUN(t_two_contexts_independent); RUN(t_error_is_per_context);

    printf("\ntest_glstate: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

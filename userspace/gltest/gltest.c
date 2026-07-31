/* gltest.c — regression test for the AuraLite OpenGL stack.
 *
 * Runs headless-style checks and prints "[gl] PASS/FAIL" markers to stdout so
 * the QEMU integration tests (tests/integration/cases/test_opengl.sh) can grep
 * the serial log, matching the convention used by /selftest and /p10test.
 *
 * Phase G0 scope: the bulk-pixel presentation path (ag_blit) that the whole GL
 * stack depends on, plus the negative cases that must be rejected by the
 * kernel WITHOUT crashing it.
 * Phase G1 scope: AuraGLX context lifecycle, glClear, the GL error machinery
 * and end-to-end frame presentation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "auragui.h"
#include "GL/gl.h"
#include "GL/auraglx.h"

static int checks = 0, fails = 0;

static void check(int cond, const char *name) {
    checks++;
    if (cond) {
        printf("[gl] PASS %s\n", name);
    } else {
        fails++;
        printf("[gl] FAIL %s\n", name);
    }
}

#define TEST_W 64
#define TEST_H 48

/* GL context size for the phase G1 checks.  Deliberately small: a software
 * rasterizer under emulation should not be asked to fill a large buffer just
 * to prove the plumbing works. */
#define GL_W 160
#define GL_H 120

/* ---- Phase G1: context, glClear, error machinery ---- */
static void test_gl_context(int wid) {
    printf("[gl] --- G1: context and clear ---\n");

    aglx_context_t *ctx = aglxCreateContext(wid, GL_W, GL_H, AGLX_DEFAULT);
    check(ctx != NULL, "ctx_create");
    if (!ctx) return;

    check(aglxGetWidth(ctx) == GL_W && aglxGetHeight(ctx) == GL_H,
          "ctx_dimensions");
    check(aglxGetDepthBuffer(ctx) != NULL, "ctx_has_depth");
    check(aglxMakeCurrent(ctx) == 0, "ctx_make_current");
    check(aglxGetCurrentContext() == ctx, "ctx_is_current");

    /* Strings identify the implementation. */
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *ver    = (const char *)glGetString(GL_VERSION);
    check(vendor && vendor[0], "gl_vendor_string");
    check(ver && ver[0], "gl_version_string");
    printf("[gl] renderer: %s / %s / %s\n", vendor ? vendor : "?",
           (const char *)glGetString(GL_RENDERER), ver ? ver : "?");

    check(glGetError() == GL_NO_ERROR, "gl_no_error_initially");

    /* Clear to a known colour and verify the actual pixels. */
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const uint32_t *cb = aglxGetColorBuffer(ctx);
    int blue_ok = cb && cb[0] == 0x0000FF && cb[GL_W * GL_H - 1] == 0x0000FF;
    check(blue_ok, "gl_clear_blue");

    const float *db = aglxGetDepthBuffer(ctx);
    check(db && db[0] == 1.0f, "gl_clear_depth_far");

    /* An invalid mask must raise GL_INVALID_VALUE and clear nothing. */
    glClear(0xDEADBEEF);
    check(glGetError() == GL_INVALID_VALUE, "gl_invalid_clear_mask");
    check(cb[0] == 0x0000FF, "gl_invalid_clear_no_effect");

    /* Errors are sticky until read, then cleared. */
    check(glGetError() == GL_NO_ERROR, "gl_error_cleared_on_read");

    /* Present the frame through the real syscall path. */
    check(aglxSwapBuffers(ctx) == 0, "gl_swap_buffers");

    /* A second clear + swap in a different colour: proves the context can be
     * reused frame after frame, which is what a real render loop does. */
    glClearColor(1.0f, 0.5f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    check(cb[0] == 0xFF8000 || cb[0] == 0xFF7F00, "gl_clear_orange");
    check(aglxSwapBuffers(ctx) == 0, "gl_swap_second_frame");

    /* Resize must keep the context usable. */
    check(aglxResize(ctx, GL_W / 2, GL_H / 2) == 0, "gl_resize");
    check(aglxGetWidth(ctx) == GL_W / 2, "gl_resize_dimensions");
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    check(aglxGetColorBuffer(ctx)[0] == 0x00FF00, "gl_clear_after_resize");
    check(aglxSwapBuffers(ctx) == 0, "gl_swap_after_resize");

    aglxDestroyContext(ctx);
    check(aglxGetCurrentContext() == NULL, "ctx_destroy_unbinds");

    /* GL calls with no current context must be safe. */
    glClear(GL_COLOR_BUFFER_BIT);
    check(glGetError() == GL_INVALID_OPERATION, "gl_no_context_is_error");
}

int main(void) {
    printf("[gl] === AuraLite GL test (phases G0-G1) ===\n");

    /* ---- A window is required as the blit destination. ---- */
    int wid = ag_window_create(40, 40, TEST_W + 20, TEST_H + 20,
                               "gltest", AG_WIN_DEFAULT);
    check(wid >= 0, "window_create");
    if (wid < 0) {
        printf("[gl] SUMMARY %d checks, %d failed\n", checks, fails);
        return 1;
    }
    ag_window_show(wid);

    /* ---- Allocate a pixel buffer and fill it with a known pattern. ---- */
    uint32_t *buf = (uint32_t *)malloc((size_t)TEST_W * TEST_H * 4);
    check(buf != NULL, "buffer_alloc");
    if (!buf) {
        ag_window_destroy(wid);
        printf("[gl] SUMMARY %d checks, %d failed\n", checks, fails);
        return 1;
    }

    for (int y = 0; y < TEST_H; y++) {
        for (int x = 0; x < TEST_W; x++) {
            /* Horizontal red ramp, vertical blue ramp — visually obvious and
             * cheap to verify by eye in a screenshot. */
            uint32_t r = (uint32_t)(x * 255 / (TEST_W - 1));
            uint32_t b = (uint32_t)(y * 255 / (TEST_H - 1));
            buf[y * TEST_W + x] = (r << 16) | (0x40 << 8) | b;
        }
    }

    /* ---- Positive case: a well-formed blit must succeed. ---- */
    int rc = ag_blit(wid, 10, 10, TEST_W, TEST_H, buf, TEST_W);
    check(rc == 0, "blit_basic");

    /* Tightly packed shorthand: stride 0 means "stride == w". */
    rc = ag_blit(wid, 10, 10, TEST_W, TEST_H, buf, 0);
    check(rc == 0, "blit_stride_zero");

    /* Alpha blit over the same area. */
    rc = ag_blit_alpha(wid, 10, 10, TEST_W, TEST_H, buf, TEST_W);
    check(rc == 0, "blit_alpha");

    /* Partially off-window blits must be clipped by the kernel, not rejected
     * and above all not fault. */
    rc = ag_blit(wid, -20, -20, TEST_W, TEST_H, buf, TEST_W);
    check(rc == 0, "blit_clip_negative");
    rc = ag_blit(wid, TEST_W, TEST_H, TEST_W, TEST_H, buf, TEST_W);
    check(rc == 0, "blit_clip_offscreen");

    /* Zero-sized blit is a no-op, not an error. */
    rc = ag_blit(wid, 0, 0, 0, 0, buf, 0);
    check(rc == 0, "blit_zero_size");

    /* ---- Negative cases: these MUST be rejected and MUST NOT panic. ---- */

    /* NULL source. */
    rc = ag_blit(wid, 0, 0, TEST_W, TEST_H, NULL, TEST_W);
    check(rc != 0, "blit_reject_null");

    /* Kernel-space pointer: the classic privilege-escalation attempt. */
    rc = ag_blit(wid, 0, 0, 8, 8, (const uint32_t *)0xFFFFFFFF80000000ULL, 8);
    check(rc != 0, "blit_reject_kernel_ptr");

    /* Non-canonical / unmapped user pointer. */
    rc = ag_blit(wid, 0, 0, 8, 8, (const uint32_t *)0x00007FFFFFFF0000ULL, 8);
    check(rc != 0, "blit_reject_unmapped");

    /* stride < w is inconsistent and must be refused. */
    rc = ag_blit(wid, 0, 0, TEST_W, TEST_H, buf, TEST_W / 2);
    check(rc != 0, "blit_reject_bad_stride");

    /* Absurd dimensions must be clamped away rather than overflowing. */
    rc = ag_blit(wid, 0, 0, 0x10000, 0x10000, buf, 0x10000);
    check(rc != 0, "blit_reject_huge");

    /* Blitting into a window we do not own must fail. */
    rc = ag_blit(9999, 0, 0, TEST_W, TEST_H, buf, TEST_W);
    check(rc != 0, "blit_reject_bad_wid");

    /* ---- The window must still be usable after all the rejects. ---- */
    rc = ag_blit(wid, 10, 10, TEST_W, TEST_H, buf, TEST_W);
    check(rc == 0, "blit_after_rejects");

    ag_render_now();

    /* ---- Phase G1 checks ---- */
    test_gl_context(wid);

    free(buf);
    ag_window_destroy(wid);

    printf("[gl] SUMMARY %d checks, %d failed\n", checks, fails);
    if (fails == 0) printf("[gl] ALL TESTS PASSED\n");
    return fails ? 1 : 0;
}

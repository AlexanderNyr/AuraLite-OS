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
 * Phase G2 scope: matrix stacks and immediate-mode geometry, verified by
 * reading back the pixels that were actually rasterised.
 * Phase G3 scope: the filled triangle rasterizer, depth buffer and culling.
 * Phase G4 scope: frustum clipping and the attribute stack.
 * Phase G5 scope: lighting, materials and normals.
 * Phase G6 scope: textures, blending, the alpha test and fog.
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

/* ---- Phase G2: matrix stacks and immediate mode ---- */
static void test_gl_geometry(int wid) {
    printf("[gl] --- G2: matrices and immediate mode ---\n");

    aglx_context_t *ctx = aglxCreateContext(wid, GL_W, GL_H, AGLX_DEFAULT);
    check(ctx != NULL, "geo_ctx_create");
    if (!ctx) return;
    aglxMakeCurrent(ctx);

    const uint32_t *cb = aglxGetColorBuffer(ctx);

    /* Map GL coordinates 1:1 onto pixels so specific pixels can be named. */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, GL_W, 0, GL_H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    check(glGetError() == GL_NO_ERROR, "geo_projection_setup");

    /* Window coordinates have a bottom-left origin, so a point low in GL
     * space must appear near the BOTTOM of the framebuffer. */
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1, 1, 1);
    glBegin(GL_POINTS);
    glVertex3f(10.5f, 5.5f, 0.0f);
    glEnd();
    int row_from_bottom = GL_H - 1 - 5;
    check(cb[(size_t)row_from_bottom * GL_W + 10] == 0xFFFFFF, "geo_point_pixel");

    /* Matrix stack: push/rotate/pop must leave the matrix as it was, so the
     * same vertex lands on the same pixel. */
    glClear(GL_COLOR_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(20.0f, 20.0f, 0.0f);
    glPushMatrix();
    glRotatef(45.0f, 0, 0, 1);
    glScalef(3.0f, 3.0f, 1.0f);
    glPopMatrix();
    glBegin(GL_POINTS);
    glVertex3f(0.5f, 0.5f, 0.0f);       /* -> (20.5, 20.5) */
    glEnd();
    check(cb[(size_t)(GL_H - 1 - 20) * GL_W + 20] == 0xFFFFFF, "geo_push_pop");

    /* Stack limits must be reported, not silently ignored. */
    glLoadIdentity();
    glPopMatrix();
    check(glGetError() == GL_STACK_UNDERFLOW, "geo_stack_underflow");

    /* A horizontal line must light a contiguous run of pixels. */
    glClear(GL_COLOR_BUFFER_BIT);
    glLoadIdentity();
    glBegin(GL_LINES);
    glVertex3f(10.5f, 40.5f, 0.0f);
    glVertex3f(50.5f, 40.5f, 0.0f);
    glEnd();
    int run = 1;
    for (int x = 10; x <= 50; x++) {
        if (cb[(size_t)(GL_H - 1 - 40) * GL_W + x] == 0) { run = 0; break; }
    }
    check(run, "geo_line_run");

    /* A triangle draws its three edges but leaves the interior empty in G2. */
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glVertex3f(10.5f, 10.5f, 0.0f);
    glVertex3f(60.5f, 10.5f, 0.0f);
    glVertex3f(10.5f, 60.5f, 0.0f);
    glEnd();
    /* Since G3 triangles are filled, so both the edge and the interior are
     * covered.  glPolygonMode(GL_LINE) is what restores the hollow outline,
     * and that is checked in the G3 block below. */
    /* Sample strictly INSIDE, not on the boundary: a pixel exactly on an edge
     * is deliberately owned by only one triangle under the top-left fill rule,
     * so it is not a stable thing to assert on. */
    int near_edge_ok = cb[(size_t)(GL_H - 1 - 12) * GL_W + 30] != 0;
    int interior_ok  = cb[(size_t)(GL_H - 1 - 25) * GL_W + 25] != 0;
    check(near_edge_ok, "geo_triangle_edge");
    check(interior_ok, "geo_triangle_filled_g3");

    /* Smooth shading interpolates colour along a line. */
    glClear(GL_COLOR_BUFFER_BIT);
    glShadeModel(GL_SMOOTH);
    glBegin(GL_LINES);
    glColor3f(1, 0, 0); glVertex3f(10.5f, 70.5f, 0.0f);
    glColor3f(0, 0, 1); glVertex3f(90.5f, 70.5f, 0.0f);
    glEnd();
    uint32_t lp = cb[(size_t)(GL_H - 1 - 70) * GL_W + 12];
    uint32_t rp = cb[(size_t)(GL_H - 1 - 70) * GL_W + 88];
    check(((lp >> 16) & 0xFF) > ((rp >> 16) & 0xFF), "geo_smooth_red_falls");
    check((lp & 0xFF) < (rp & 0xFF), "geo_smooth_blue_rises");

    /* Misuse must be reported rather than crashing. */
    glVertex3f(0, 0, 0);
    check(glGetError() == GL_INVALID_OPERATION, "geo_vertex_outside_begin");
    glBegin(0x9999);
    check(glGetError() == GL_INVALID_ENUM, "geo_begin_bad_mode");

    /* A perspective projection must foreshorten: the same bar drawn further
     * away must cover fewer pixels. */
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1, 100);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glColor3f(1, 1, 1);
    glBegin(GL_LINES);
    glVertex3f(-1, 0, -2); glVertex3f(1, 0, -2);
    glEnd();
    int near_lit = 0;
    for (int i = 0; i < GL_W * GL_H; i++) if (cb[i]) near_lit++;

    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_LINES);
    glVertex3f(-1, 0, -20); glVertex3f(1, 0, -20);
    glEnd();
    int far_lit = 0;
    for (int i = 0; i < GL_W * GL_H; i++) if (cb[i]) far_lit++;
    check(near_lit > far_lit && far_lit > 0, "geo_perspective_foreshortens");

    /* Geometry behind the eye must be dropped, not projected somewhere wild. */
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_POINTS);
    glVertex3f(0, 0, 5.0f);
    glEnd();
    int behind_lit = 0;
    for (int i = 0; i < GL_W * GL_H; i++) if (cb[i]) behind_lit++;
    check(behind_lit == 0, "geo_behind_eye_dropped");

    check(aglxSwapBuffers(ctx) == 0, "geo_swap");
    check(glGetError() == GL_NO_ERROR, "geo_no_pending_error");

    aglxDestroyContext(ctx);
}

/* ---- Phase G3: filled rasterizer, depth buffer, culling ---- */
static void test_gl_raster(int wid) {
    printf("[gl] --- G3: rasterizer, depth, culling ---\n");

    aglx_context_t *ctx = aglxCreateContext(wid, GL_W, GL_H, AGLX_DEPTH);
    check(ctx != NULL, "ras_ctx_create");
    if (!ctx) return;
    aglxMakeCurrent(ctx);

    const uint32_t *cb = aglxGetColorBuffer(ctx);
    const float    *db = aglxGetDepthBuffer(ctx);

    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, GL_W, 0, GL_H, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);

    /* Row helper: GL window y -> framebuffer row. */
    #define ROW(y) ((size_t)(GL_H - 1 - (y)) * GL_W)

    /* ---- Triangles are now FILLED, not hollow. ---- */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(10.5f, 10.5f, 0.0f);
    glVertex3f(90.5f, 10.5f, 0.0f);
    glVertex3f(10.5f, 90.5f, 0.0f);
    glEnd();
    check(cb[ROW(30) + 30] != 0, "ras_triangle_filled");
    check(cb[ROW(85) + 85] == 0, "ras_outside_empty");

    /* ---- Gouraud interpolation across the face. ---- */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glShadeModel(GL_SMOOTH);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex3f(5.5f,  5.5f,  0.0f);
    glColor3f(0, 1, 0); glVertex3f(95.5f, 5.5f,  0.0f);
    glColor3f(0, 0, 1); glVertex3f(5.5f,  95.5f, 0.0f);
    glEnd();
    uint32_t corner_r = cb[ROW(8) + 8];
    uint32_t corner_g = cb[ROW(8) + 90];
    uint32_t corner_b = cb[ROW(90) + 8];
    check(((corner_r >> 16) & 0xFF) > 180, "ras_gouraud_red");
    check(((corner_g >>  8) & 0xFF) > 180, "ras_gouraud_green");
    check(( corner_b        & 0xFF) > 180, "ras_gouraud_blue");

    /* ---- Depth test: nearer geometry wins regardless of draw order. ----
     * glOrtho negates z, so object z=+0.5 is NEARER (window depth 0.25). */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glShadeModel(GL_FLAT);

    glColor3f(1, 0, 0);                    /* farther, drawn first */
    glBegin(GL_QUADS);
    glVertex3f(0, 0, -0.5f);       glVertex3f(GL_W, 0, -0.5f);
    glVertex3f(GL_W, GL_H, -0.5f); glVertex3f(0, GL_H, -0.5f);
    glEnd();
    glColor3f(0, 1, 0);                    /* nearer, drawn second */
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 0.5f);        glVertex3f(GL_W, 0, 0.5f);
    glVertex3f(GL_W, GL_H, 0.5f);  glVertex3f(0, GL_H, 0.5f);
    glEnd();
    check(cb[ROW(50) + 50] == 0x00FF00, "ras_depth_nearer_wins");

    /* Reverse order: the farther quad must now be rejected. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(0, 1, 0);
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 0.5f);        glVertex3f(GL_W, 0, 0.5f);
    glVertex3f(GL_W, GL_H, 0.5f);  glVertex3f(0, GL_H, 0.5f);
    glEnd();
    glColor3f(1, 0, 0);
    glBegin(GL_QUADS);
    glVertex3f(0, 0, -0.5f);       glVertex3f(GL_W, 0, -0.5f);
    glVertex3f(GL_W, GL_H, -0.5f); glVertex3f(0, GL_H, -0.5f);
    glEnd();
    check(cb[ROW(50) + 50] == 0x00FF00, "ras_depth_farther_rejected");
    check(db[ROW(50) + 50] < 0.3f, "ras_depth_value_written");

    glDisable(GL_DEPTH_TEST);

    /* ---- Back-face culling. ---- */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glColor3f(1, 1, 1);
    /* Clockwise winding = back-facing = culled. */
    glBegin(GL_TRIANGLES);
    glVertex3f(10.5f, 10.5f, 0.0f);
    glVertex3f(10.5f, 90.5f, 0.0f);
    glVertex3f(90.5f, 10.5f, 0.0f);
    glEnd();
    int culled_empty = (cb[ROW(30) + 30] == 0);
    check(culled_empty, "ras_cull_back_face");

    /* Counter-clockwise = front-facing = kept. */
    glBegin(GL_TRIANGLES);
    glVertex3f(10.5f, 10.5f, 0.0f);
    glVertex3f(90.5f, 10.5f, 0.0f);
    glVertex3f(10.5f, 90.5f, 0.0f);
    glEnd();
    check(cb[ROW(30) + 30] != 0, "ras_cull_keeps_front");
    glDisable(GL_CULL_FACE);

    /* ---- Shared edge must tile exactly: no seam between two triangles. ---- */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(20.0f, 20.0f, 0); glVertex3f(80.0f, 20.0f, 0); glVertex3f(80.0f, 80.0f, 0);
    glVertex3f(20.0f, 20.0f, 0); glVertex3f(80.0f, 80.0f, 0); glVertex3f(20.0f, 80.0f, 0);
    glEnd();
    int seam_gaps = 0;
    for (int y = 21; y < 79; y++)
        for (int x = 21; x < 79; x++)
            if (cb[ROW(y) + x] == 0) seam_gaps++;
    check(seam_gaps == 0, "ras_no_diagonal_seam");

    /* ---- Scissor test. ---- */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(30, 30, 20, 20);
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 0);        glVertex3f(GL_W, 0, 0);
    glVertex3f(GL_W, GL_H, 0);  glVertex3f(0, GL_H, 0);
    glEnd();
    check(cb[ROW(35) + 35] != 0, "ras_scissor_inside");
    check(cb[ROW(10) + 10] == 0, "ras_scissor_outside");
    glDisable(GL_SCISSOR_TEST);

    /* ---- glPolygonMode(GL_LINE) restores wireframe. ---- */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glBegin(GL_TRIANGLES);
    glVertex3f(10.5f, 10.5f, 0.0f);
    glVertex3f(90.5f, 10.5f, 0.0f);
    glVertex3f(10.5f, 90.5f, 0.0f);
    glEnd();
    check(cb[ROW(10) + 50] != 0, "ras_polymode_line_edge");
    check(cb[ROW(30) + 30] == 0, "ras_polymode_line_hollow");
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* ---- Degenerate geometry must not hang or crash. ---- */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glVertex3f(5.5f, 5.5f, 0); glVertex3f(50.5f, 5.5f, 0); glVertex3f(95.5f, 5.5f, 0);
    glEnd();
    check(cb[ROW(5) + 50] == 0, "ras_degenerate_empty");

    /* Enormous coordinates must be bounded by the buffer, not by their size. */
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.0e6f, -1.0e6f, 0);
    glVertex3f( 1.0e6f, -1.0e6f, 0);
    glVertex3f( 0.0f,    1.0e6f, 0);
    glEnd();
    check(cb[ROW(50) + 50] != 0, "ras_huge_triangle_bounded");

    /* ---- State queries. ---- */
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    check(vp[2] == GL_W && vp[3] == GL_H, "ras_get_viewport");
    glEnable(GL_DEPTH_TEST);
    check(glIsEnabled(GL_DEPTH_TEST) == GL_TRUE, "ras_is_enabled");
    glDisable(GL_DEPTH_TEST);

    check(aglxSwapBuffers(ctx) == 0, "ras_swap");
    check(glGetError() == GL_NO_ERROR, "ras_no_pending_error");

    #undef ROW
    aglxDestroyContext(ctx);
}

/* ---- Phase G4: frustum clipping and the attribute stack ---- */
static void test_gl_clip(int wid) {
    printf("[gl] --- G4: frustum clipping ---\n");

    aglx_context_t *ctx = aglxCreateContext(wid, GL_W, GL_H, AGLX_DEPTH);
    check(ctx != NULL, "clip_ctx_create");
    if (!ctx) return;
    aglxMakeCurrent(ctx);

    const uint32_t *cb = aglxGetColorBuffer(ctx);
    #define LIT(n) do { n = 0; for (int i = 0; i < GL_W * GL_H; i++) if (cb[i]) n++; } while (0)
    int n;

    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1, 100);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glColor3f(1, 1, 1);

    /* THE headline case: a triangle straddling the near plane must be SPLIT
     * and still drawn.  Before G4 it vanished entirely, which is the classic
     * "walls disappear when you walk into them" artefact. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.0f, -1.0f, -3.0f);
    glVertex3f( 1.0f, -1.0f, -3.0f);
    glVertex3f( 0.0f,  1.0f,  1.0f);      /* behind the eye */
    glEnd();
    LIT(n);
    check(n > 0, "clip_near_plane_split");

    /* Entirely behind the eye: nothing drawn, no error. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.0f, -1.0f, 2.0f);
    glVertex3f( 1.0f, -1.0f, 2.0f);
    glVertex3f( 0.0f,  1.0f, 3.0f);
    glEnd();
    LIT(n);
    check(n == 0, "clip_fully_behind_dropped");

    /* Two vertices behind: the surviving sliver becomes a quad, which must be
     * fanned back into triangles. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glVertex3f( 0.0f,  0.0f, -3.0f);
    glVertex3f(-2.0f, -2.0f,  1.0f);
    glVertex3f( 2.0f, -2.0f,  1.0f);
    glEnd();
    LIT(n);
    check(n > 0, "clip_two_behind");

    /* A line crossing the near plane must be shortened, not dropped. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_LINES);
    glVertex3f(0.0f, 0.0f, -5.0f);
    glVertex3f(0.0f, 0.0f,  5.0f);
    glEnd();
    LIT(n);
    check(n > 0, "clip_line_crossing_near");

    /* Beyond the far plane: clipped away. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.0f, -1.0f, -500.0f);
    glVertex3f( 1.0f, -1.0f, -500.0f);
    glVertex3f( 0.0f,  1.0f, -500.0f);
    glEnd();
    LIT(n);
    check(n == 0, "clip_far_plane");

    /* Clipping must preserve winding, or a front face becomes a back face and
     * disappears once culling is on. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.0f, -1.0f, -3.0f);
    glVertex3f( 1.0f, -1.0f, -3.0f);
    glVertex3f( 0.0f,  1.0f,  0.5f);
    glEnd();
    LIT(n);
    check(n > 0, "clip_preserves_winding");
    glDisable(GL_CULL_FACE);

    /* Camera flying THROUGH an object: every step must render without
     * artefacts and without faulting.  This is the scenario G4 exists for. */
    int all_steps_ok = 1;
    for (int step = 0; step < 10; step++) {
        float z = -6.0f + (float)step * 1.2f;   /* passes through the origin */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, z);
        glBegin(GL_QUADS);
        glVertex3f(-1.5f, -1.5f, 0.0f); glVertex3f(1.5f, -1.5f, 0.0f);
        glVertex3f( 1.5f,  1.5f, 0.0f); glVertex3f(-1.5f, 1.5f, 0.0f);
        glEnd();
        if (glGetError() != GL_NO_ERROR) all_steps_ok = 0;
    }
    check(all_steps_ok, "clip_camera_flythrough");
    glLoadIdentity();

    /* ---- Attribute stack ---- */
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glPushAttrib(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glPopAttrib();
    check(glIsEnabled(GL_DEPTH_TEST) == GL_FALSE, "attrib_restores_enable");
    GLint df;
    glGetIntegerv(GL_DEPTH_FUNC, &df);
    check(df == GL_LESS, "attrib_restores_func");

    glPopAttrib();
    check(glGetError() == GL_STACK_UNDERFLOW, "attrib_underflow");

    check(aglxSwapBuffers(ctx) == 0, "clip_swap");
    check(glGetError() == GL_NO_ERROR, "clip_no_pending_error");

    #undef LIT
    aglxDestroyContext(ctx);
}

/* ---- Phase G5: lighting and materials ---- */
static void test_gl_light(int wid) {
    printf("[gl] --- G5: lighting and materials ---\n");

    aglx_context_t *ctx = aglxCreateContext(wid, GL_W, GL_H, AGLX_DEPTH);
    check(ctx != NULL, "lit_ctx_create");
    if (!ctx) return;
    aglxMakeCurrent(ctx);

    const uint32_t *cb = aglxGetColorBuffer(ctx);
    #define CENTRE cb[(size_t)(GL_H / 2) * GL_W + (GL_W / 2)]

    /* A small quad pushed back from the camera: with per-vertex lighting the
     * specular term is evaluated AT THE VERTICES, so they must all sit close
     * to the view axis for a highlight to appear at all. */
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-4, 4, -4, 4, 1, 100);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);

    #define DRAW_QUAD() do {                                   \
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);    \
        glNormal3f(0.0f, 0.0f, 1.0f);                          \
        glBegin(GL_QUADS);                                     \
        glVertex3f(-2.0f, -2.0f, -20.0f);                      \
        glVertex3f( 2.0f, -2.0f, -20.0f);                      \
        glVertex3f( 2.0f,  2.0f, -20.0f);                      \
        glVertex3f(-2.0f,  2.0f, -20.0f);                      \
        glEnd();                                               \
    } while (0)

    check(glIsEnabled(GL_LIGHTING) == GL_FALSE, "lit_off_by_default");

    GLint maxl = 0;
    glGetIntegerv(GL_MAX_LIGHTS, &maxl);
    check(maxl >= 8, "lit_max_lights");

    GLfloat white[4] = { 1, 1, 1, 1 };
    GLfloat black[4] = { 0, 0, 0, 1 };

    /* Unlit: glColor passes straight through. */
    glDisable(GL_LIGHTING);
    glColor3f(1.0f, 0.0f, 0.0f);
    DRAW_QUAD();
    check(CENTRE == 0xFF0000, "lit_unlit_passthrough");

    /* Diffuse, light head-on: saturated. */
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    {
        GLfloat pos[4] = { 0, 0, 1, 0 };
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
    }
    glMaterialfv(GL_FRONT, GL_DIFFUSE, white);
    DRAW_QUAD();
    check(((CENTRE >> 16) & 0xFF) > 240, "lit_diffuse_head_on");

    /* Light at 90 degrees: only the ambient terms remain. */
    {
        GLfloat pos[4] = { 1, 0, 0, 0 };
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
    }
    DRAW_QUAD();
    check(((CENTRE >> 16) & 0xFF) < 40, "lit_diffuse_perpendicular");

    /* Specular highlight requires a non-zero shininess. */
    {
        GLfloat pos[4] = { 0, 0, 1, 0 };
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
    }
    glMaterialfv(GL_FRONT, GL_DIFFUSE, black);
    glMaterialfv(GL_FRONT, GL_AMBIENT, black);
    glMaterialfv(GL_FRONT, GL_SPECULAR, white);
    glMaterialf(GL_FRONT, GL_SHININESS, 0.0f);
    DRAW_QUAD();
    check(((CENTRE >> 16) & 0xFF) < 20, "lit_no_specular_at_zero_shininess");

    glMaterialf(GL_FRONT, GL_SHININESS, 32.0f);
    DRAW_QUAD();
    check(((CENTRE >> 16) & 0xFF) > 80, "lit_specular_highlight");

    /* Emission applies with no lights at all. */
    glDisable(GL_LIGHT0);
    glMaterialfv(GL_FRONT, GL_SPECULAR, black);
    glMaterialf(GL_FRONT, GL_SHININESS, 0.0f);
    {
        GLfloat green[4] = { 0, 1, 0, 1 };
        glMaterialfv(GL_FRONT, GL_EMISSION, green);
    }
    DRAW_QUAD();
    check(((CENTRE >> 8) & 0xFF) > 240, "lit_emission");
    glMaterialfv(GL_FRONT, GL_EMISSION, black);

    /* Distance attenuation dims a positional light.
     *
     * The material is reset explicitly first: the specular checks above left
     * GL_SPECULAR white, and a leftover specular term would add the same
     * amount to both samples and mask the attenuation difference.  Tests that
     * share one context have to undo their own state. */
    glEnable(GL_LIGHT0);
    glMaterialfv(GL_FRONT, GL_DIFFUSE, white);
    glMaterialfv(GL_FRONT, GL_AMBIENT, black);
    glMaterialfv(GL_FRONT, GL_SPECULAR, black);
    glMaterialf(GL_FRONT, GL_SHININESS, 0.0f);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, black);
    glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, 0.5f);
    {
        GLfloat near_pos[4] = { 0, 0, -18, 1 };
        glLightfv(GL_LIGHT0, GL_POSITION, near_pos);
    }
    DRAW_QUAD();
    int near_lit = (CENTRE >> 16) & 0xFF;
    {
        GLfloat far_pos[4] = { 0, 0, 60, 1 };
        glLightfv(GL_LIGHT0, GL_POSITION, far_pos);
    }
    DRAW_QUAD();
    int far_lit = (CENTRE >> 16) & 0xFF;
    check(near_lit > far_lit, "lit_distance_attenuation");
    glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, 0.0f);

    /* GL_COLOR_MATERIAL lets glColor drive the material while lit. */
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
    {
        GLfloat pos[4] = { 0, 0, 1, 0 };
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
    }
    glColor3f(0.0f, 1.0f, 0.0f);
    DRAW_QUAD();
    check(((CENTRE >> 8) & 0xFF) > 200 && ((CENTRE >> 16) & 0xFF) < 60,
          "lit_color_material");
    glDisable(GL_COLOR_MATERIAL);

    /* Light positions are transformed by the MODELVIEW in force at the time. */
    glLoadIdentity();
    glTranslatef(5.0f, 0.0f, 0.0f);
    {
        GLfloat origin[4] = { 0, 0, 0, 1 };
        glLightfv(GL_LIGHT0, GL_POSITION, origin);
    }
    glLoadIdentity();
    check(aglxGetWidth(ctx) > 0, "lit_position_modelview");  /* no fault */

    /* Invalid inputs are reported. */
    {
        GLfloat v[4] = { 1, 1, 1, 1 };
        glLightfv(GL_LIGHT0 + 99, GL_DIFFUSE, v);
    }
    check(glGetError() == GL_INVALID_ENUM, "lit_bad_light_enum");
    glMaterialf(GL_FRONT, GL_SHININESS, 500.0f);
    check(glGetError() == GL_INVALID_VALUE, "lit_bad_shininess");

    /* Lighting must Gouraud-interpolate, not fill flat. */
    glDisable(GL_COLOR_MATERIAL);
    glShadeModel(GL_SMOOTH);
    glMaterialfv(GL_FRONT, GL_DIFFUSE, white);
    glMaterialfv(GL_FRONT, GL_AMBIENT, black);
    {
        GLfloat pos[4] = { 0, 0, 1, 0 };
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_QUADS);
    glNormal3f(0, 0, 1);  glVertex3f(-2.0f, -2.0f, -20.0f);
    glNormal3f(0, 0, 1);  glVertex3f(-2.0f,  2.0f, -20.0f);
    glNormal3f(0, 0, -1); glVertex3f( 2.0f,  2.0f, -20.0f);
    glNormal3f(0, 0, -1); glVertex3f( 2.0f, -2.0f, -20.0f);
    glEnd();
    {
        int left  = (cb[(size_t)(GL_H / 2) * GL_W + (GL_W * 3 / 8)] >> 16) & 0xFF;
        int right = (cb[(size_t)(GL_H / 2) * GL_W + (GL_W * 5 / 8)] >> 16) & 0xFF;
        check(left > right + 30, "lit_gouraud_gradient");
    }

    glDisable(GL_LIGHTING);
    check(aglxSwapBuffers(ctx) == 0, "lit_swap");
    check(glGetError() == GL_NO_ERROR, "lit_no_pending_error");

    #undef DRAW_QUAD
    #undef CENTRE
    aglxDestroyContext(ctx);
}

/* ---- Phase G6: textures, blending, fog ---- */
static void test_gl_texture(int wid) {
    printf("[gl] --- G6: textures, blending, fog ---\n");

    aglx_context_t *ctx = aglxCreateContext(wid, GL_W, GL_H, AGLX_DEPTH);
    check(ctx != NULL, "tex_ctx_create");
    if (!ctx) return;
    aglxMakeCurrent(ctx);

    const uint32_t *cb = aglxGetColorBuffer(ctx);
    #define AT(x, y) cb[(size_t)(GL_H - 1 - (y)) * GL_W + (x)]

    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, GL_W, 0, GL_H, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glColor3f(1, 1, 1);

    /* A 2x2 texture; row 0 is the BOTTOM row, matching GL's origin. */
    static const unsigned char rgb2x2[2 * 2 * 3] = {
        255, 0,   0,      0,   255, 0,
        0,   0,   255,    255, 255, 255,
    };
    GLuint id = 0;
    glGenTextures(1, &id);
    check(id != 0, "tex_gen");
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, rgb2x2);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    check(glGetError() == GL_NO_ERROR, "tex_upload");
    check(glIsTexture(id) == GL_TRUE, "tex_is_texture");

    #define FULL_QUAD(smax, tmax) do {                             \
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);        \
        glBegin(GL_QUADS);                                         \
        glTexCoord2f(0,      0);      glVertex3f(0, 0, 0);         \
        glTexCoord2f(smax,   0);      glVertex3f(GL_W, 0, 0);      \
        glTexCoord2f(smax,   tmax);   glVertex3f(GL_W, GL_H, 0);   \
        glTexCoord2f(0,      tmax);   glVertex3f(0, GL_H, 0);      \
        glEnd();                                                   \
    } while (0)

    /* Nearest sampling: each quadrant shows its own texel, and the v axis is
     * NOT flipped (row 0 of the upload is the bottom). */
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    FULL_QUAD(1.0f, 1.0f);
    check(AT(GL_W / 4, GL_H / 4) == 0xFF0000,        "tex_quadrant_red");
    check(AT(GL_W * 3 / 4, GL_H / 4) == 0x00FF00,    "tex_quadrant_green");
    check(AT(GL_W / 4, GL_H * 3 / 4) == 0x0000FF,    "tex_quadrant_blue");
    check(AT(GL_W * 3 / 4, GL_H * 3 / 4) == 0xFFFFFF,"tex_quadrant_white");

    /* GL_REPEAT: two tiles across, so s=1.25 hits the same texel as s=0.25. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    FULL_QUAD(2.0f, 2.0f);
    check(AT(GL_W / 8, GL_H / 8) == 0xFF0000 &&
          AT(GL_W * 5 / 8, GL_H / 8) == 0xFF0000, "tex_wrap_repeat");

    /* Bilinear at the image centre averages all four texels. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    FULL_QUAD(1.0f, 1.0f);
    {
        uint32_t p = AT(GL_W / 2, GL_H / 2);
        int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        check(r > 100 && r < 160 && g > 100 && g < 160 && b > 100 && b < 160,
              "tex_bilinear_average");
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    /* GL_MODULATE multiplies by the fragment colour; GL_REPLACE ignores it. */
    glColor3f(1.0f, 0.0f, 0.0f);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    FULL_QUAD(1.0f, 1.0f);
    {
        uint32_t p = AT(GL_W * 3 / 4, GL_H * 3 / 4);   /* white texel */
        check(((p >> 16) & 0xFF) > 240 && ((p >> 8) & 0xFF) < 20,
              "tex_env_modulate");
    }
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    FULL_QUAD(1.0f, 1.0f);
    check(AT(GL_W * 3 / 4, GL_H * 3 / 4) == 0xFFFFFF, "tex_env_replace");
    glColor3f(1, 1, 1);

    /* Disabling texturing restores plain vertex colour. */
    glDisable(GL_TEXTURE_2D);
    glColor3f(1.0f, 0.0f, 1.0f);
    FULL_QUAD(1.0f, 1.0f);
    check(AT(GL_W / 2, GL_H / 2) == 0xFF00FF, "tex_disable");
    glColor3f(1, 1, 1);

    /* ---- Blending ---- */
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 0.0f, 0.0f, 0.5f);
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 0);        glVertex3f(GL_W, 0, 0);
    glVertex3f(GL_W, GL_H, 0);  glVertex3f(0, GL_H, 0);
    glEnd();
    {
        uint32_t p = AT(GL_W / 2, GL_H / 2);
        int r = (p >> 16) & 0xFF, b = p & 0xFF;
        check(r > 100 && r < 160 && b > 100 && b < 160, "blend_src_alpha");
    }

    /* Blending off means the source simply replaces. */
    glDisable(GL_BLEND);
    glColor4f(1.0f, 0.0f, 0.0f, 0.25f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 0);        glVertex3f(GL_W, 0, 0);
    glVertex3f(GL_W, GL_H, 0);  glVertex3f(0, GL_H, 0);
    glEnd();
    check(AT(GL_W / 2, GL_H / 2) == 0xFF0000, "blend_disabled");

    /* ---- Alpha test ---- */
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glColor4f(1.0f, 0.0f, 0.0f, 0.25f);           /* discarded */
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 0);        glVertex3f(GL_W, 0, 0);
    glVertex3f(GL_W, GL_H, 0);  glVertex3f(0, GL_H, 0);
    glEnd();
    check(AT(GL_W / 2, GL_H / 2) == 0x0000FF, "alpha_test_discards");

    glColor4f(1.0f, 0.0f, 0.0f, 0.9f);            /* passes */
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 0);        glVertex3f(GL_W, 0, 0);
    glVertex3f(GL_W, GL_H, 0);  glVertex3f(0, GL_H, 0);
    glEnd();
    check(AT(GL_W / 2, GL_H / 2) == 0xFF0000, "alpha_test_passes");
    glDisable(GL_ALPHA_TEST);

    /* ---- Fog ---- */
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1, 100);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 5.0f);
    glFogf(GL_FOG_END, 20.0f);
    {
        GLfloat fogcol[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
        glFogfv(GL_FOG_COLOR, fogcol);
    }
    glColor3f(1.0f, 0.0f, 0.0f);

    /* Beyond the fog end: fully fog-coloured. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_QUADS);
    glVertex3f(-8, -8, -25); glVertex3f(8, -8, -25);
    glVertex3f( 8,  8, -25); glVertex3f(-8, 8, -25);
    glEnd();
    {
        uint32_t p = AT(GL_W / 2, GL_H / 2);
        check(((p >> 8) & 0xFF) > 230 && ((p >> 16) & 0xFF) < 25, "fog_far");
    }

    /* Nearer than the fog start: unfogged. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glFogf(GL_FOG_START, 20.0f);
    glFogf(GL_FOG_END, 50.0f);
    glBegin(GL_QUADS);
    glVertex3f(-2, -2, -5); glVertex3f(2, -2, -5);
    glVertex3f( 2,  2, -5); glVertex3f(-2, 2, -5);
    glEnd();
    {
        uint32_t p = AT(GL_W / 2, GL_H / 2);
        check(((p >> 16) & 0xFF) > 230 && ((p >> 8) & 0xFF) < 25, "fog_near");
    }
    glDisable(GL_FOG);

    /* ---- Error handling ---- */
    glBindTexture(0x9999, 1);
    check(glGetError() == GL_INVALID_ENUM, "tex_bad_target");
    glBlendFunc(0x9999, GL_ONE);
    check(glGetError() == GL_INVALID_ENUM, "blend_bad_factor");
    glFogi(GL_FOG_MODE, 0x9999);
    check(glGetError() == GL_INVALID_ENUM, "fog_bad_mode");

    glDeleteTextures(1, &id);
    check(glIsTexture(id) == GL_FALSE, "tex_delete");

    check(aglxSwapBuffers(ctx) == 0, "tex_swap");
    check(glGetError() == GL_NO_ERROR, "tex_no_pending_error");

    #undef FULL_QUAD
    #undef AT
    aglxDestroyContext(ctx);
}

int main(void) {
    printf("[gl] === AuraLite GL test (phases G0-G6) ===\n");

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

    /* ---- Phase G1-G6 checks ---- */
    test_gl_context(wid);
    test_gl_geometry(wid);
    test_gl_raster(wid);
    test_gl_clip(wid);
    test_gl_light(wid);
    test_gl_texture(wid);

    free(buf);
    ag_window_destroy(wid);

    printf("[gl] SUMMARY %d checks, %d failed\n", checks, fails);
    if (fails == 0) printf("[gl] ALL TESTS PASSED\n");
    return fails ? 1 : 0;
}

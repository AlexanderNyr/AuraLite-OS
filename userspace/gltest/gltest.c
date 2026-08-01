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
 * Phase G7 scope: vertex arrays, buffer objects and display lists.
 * Phase G8 scope: the GLU utility layer.
 * Phase G9 scope: the backend seam.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "auragui.h"
#include "GL/gl.h"
#include "GL/auraglx.h"
#include "GL/glu.h"
#include "GL/glbackend.h"

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

    /* ---- Back-face culling. ----
     *
     * The depth test is explicitly disabled first.  The checks above leave it
     * enabled with a primed depth buffer, and the second draw here reuses the
     * same buffer without clearing, so a coplanar triangle at the same depth
     * would be rejected by GL_LESS and the check would fail for a reason that
     * has nothing to do with culling. */
    glDisable(GL_DEPTH_TEST);
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


/* ---- Phase G10: mipmaps, multitexturing, 3D textures, cube maps ----
 *
 * Running these ON THE TARGET rather than only on the host matters: the
 * mipmap path is float-heavy, and G3 already produced one bug (the fill-rule
 * epsilon) that was invisible on the host and visible under AuraLite.
 */
static void test_gl_texture2(int wid) {
    printf("[gl] --- G10: mipmaps, multitexturing, 3D, cube maps ---\n");

    aglx_context_t *ctx = aglxCreateContext(wid, GL_W, GL_H, AGLX_DEPTH);
    check(ctx != NULL, "tex2_ctx_create");
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

    /* ---- Mipmaps ---- */

    /* A 64x64 checkerboard, the classic minification aliasing source. */
    static unsigned char checker[64 * 64 * 3];
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            unsigned char v = ((x + y) & 1) ? 255 : 0;
            checker[((size_t)y * 64 + x) * 3 + 0] = v;
            checker[((size_t)y * 64 + x) * 3 + 1] = v;
            checker[((size_t)y * 64 + x) * 3 + 2] = v;
        }
    }

    GLuint mip = 0;
    glGenTextures(1, &mip);
    glBindTexture(GL_TEXTURE_2D, mip);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, checker);
    glGenerateMipmap(GL_TEXTURE_2D);
    check(glGetError() == GL_NO_ERROR, "mip_generate");

    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    /* Draw the same heavily minified quad twice, once point-sampled and once
     * mipmapped, and compare the variance.  Deliberately 11 pixels wide: at
     * an exact 8:1 ratio every pixel centre would hit the same checkerboard
     * phase and even the un-mipmapped draw would come out flat. */
    #define MINI_QUAD() do {                                        \
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);         \
        glBegin(GL_QUADS);                                          \
        glTexCoord2f(0, 0); glVertex3f(8,  8,  0);                  \
        glTexCoord2f(1, 0); glVertex3f(19, 8,  0);                  \
        glTexCoord2f(1, 1); glVertex3f(19, 19, 0);                  \
        glTexCoord2f(0, 1); glVertex3f(8,  19, 0);                  \
        glEnd();                                                    \
    } while (0)

    #define MINI_VAR(out) do {                                      \
        long sum = 0, sumsq = 0; int n = 0;                         \
        for (int yy = 10; yy < 17; yy++) {                          \
            for (int xx = 10; xx < 17; xx++) {                      \
                long v = (long)((AT(xx, yy) >> 16) & 0xFF);         \
                sum += v; sumsq += v * v; n++;                      \
            }                                                       \
        }                                                           \
        long mean = sum / n;                                        \
        (out) = sumsq / n - mean * mean;                            \
    } while (0)

    long var_plain = 0, var_mip = 0;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    MINI_QUAD();
    MINI_VAR(var_plain);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    MINI_QUAD();
    MINI_VAR(var_mip);

    check(var_plain > 1000, "mip_reference_aliases");
    check(var_mip * 10 < var_plain, "mip_reduces_aliasing");

    /* Magnification must ignore the chain: a magnified checkerboard still
     * shows individual texels, not the grey average. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord2f(0,     0);     glVertex3f(0, 0, 0);
    glTexCoord2f(0.05f, 0);     glVertex3f(GL_W, 0, 0);
    glTexCoord2f(0.05f, 0.05f); glVertex3f(GL_W, GL_H, 0);
    glTexCoord2f(0,     0.05f); glVertex3f(0, GL_H, 0);
    glEnd();
    {
        long sum = 0, sumsq = 0; int n = 0;
        for (int yy = 10; yy < GL_H - 10; yy += 3) {
            for (int xx = 10; xx < GL_W - 10; xx += 3) {
                long v = (long)((AT(xx, yy) >> 16) & 0xFF);
                sum += v; sumsq += v * v; n++;
            }
        }
        long mean = sum / n;
        check(sumsq / n - mean * mean > 1000, "mip_magnification_uses_level0");
    }

    /* All four mipmap filters must be accepted. */
    {
        static const GLenum f[4] = {
            GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST,
            GL_NEAREST_MIPMAP_LINEAR,  GL_LINEAR_MIPMAP_LINEAR
        };
        int all_ok = 1;
        for (int i = 0; i < 4; i++) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)f[i]);
            if (glGetError() != GL_NO_ERROR) all_ok = 0;
        }
        check(all_ok, "mip_all_filters_accepted");
    }

    /* A mipmap enum as the MAGNIFICATION filter is an error. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    check(glGetError() == GL_INVALID_ENUM, "mip_mag_filter_rejected");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    /* gluBuild2DMipmaps must succeed and leave a usable texture. */
    {
        GLuint gid = 0;
        glGenTextures(1, &gid);
        glBindTexture(GL_TEXTURE_2D, gid);
        int rc = gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, 64, 64, GL_RGB,
                                   GL_UNSIGNED_BYTE, checker);
        check(rc == 0, "glu_build_mipmaps");
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        GL_LINEAR_MIPMAP_LINEAR);
        long var_glu = 0;
        MINI_QUAD();
        MINI_VAR(var_glu);
        check(var_glu * 10 < var_plain, "glu_mipmaps_reduce_aliasing");
        glDeleteTextures(1, &gid);
    }

    glDeleteTextures(1, &mip);
    glDisable(GL_TEXTURE_2D);

    /* ---- Multitexturing ---- */
    {
        static const unsigned char c0[3] = { 255, 128, 0 };
        static const unsigned char c1[3] = { 128, 255, 255 };
        GLuint t0 = 0, t1 = 0;

        glActiveTexture(GL_TEXTURE0);
        glGenTextures(1, &t0);
        glBindTexture(GL_TEXTURE_2D, t0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, c0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

        glActiveTexture(GL_TEXTURE1);
        glGenTextures(1, &t1);
        glBindTexture(GL_TEXTURE_2D, t1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, c1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        check(glGetError() == GL_NO_ERROR, "mt_setup");

        {
            GLint act = 0;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &act);
            check(act == GL_TEXTURE1, "mt_active_unit_query");
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBegin(GL_QUADS);
        glMultiTexCoord2f(GL_TEXTURE1, 0, 0); glTexCoord2f(0, 0);
        glVertex3f(10, 10, 0);
        glMultiTexCoord2f(GL_TEXTURE1, 1, 0); glTexCoord2f(1, 0);
        glVertex3f(GL_W - 10, 10, 0);
        glMultiTexCoord2f(GL_TEXTURE1, 1, 1); glTexCoord2f(1, 1);
        glVertex3f(GL_W - 10, GL_H - 10, 0);
        glMultiTexCoord2f(GL_TEXTURE1, 0, 1); glTexCoord2f(0, 1);
        glVertex3f(10, GL_H - 10, 0);
        glEnd();
        {
            /* (255,128,0) * (128,255,255) / 255 = (128,128,0). */
            uint32_t p = AT(GL_W / 2, GL_H / 2);
            int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            check(r > 118 && r < 138 && g > 118 && g < 138 && b < 10,
                  "mt_two_units_modulate");
        }

        /* Disabling unit 1 must leave unit 0's colour alone. */
        glDisable(GL_TEXTURE_2D);          /* unit 1 is still active */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex3f(10, 10, 0);
        glTexCoord2f(1, 0); glVertex3f(GL_W - 10, 10, 0);
        glTexCoord2f(1, 1); glVertex3f(GL_W - 10, GL_H - 10, 0);
        glTexCoord2f(0, 1); glVertex3f(10, GL_H - 10, 0);
        glEnd();
        check(AT(GL_W / 2, GL_H / 2) == 0xFF8000, "mt_disabled_unit_ignored");

        glActiveTexture(GL_TEXTURE0);
        glDisable(GL_TEXTURE_2D);
        glDeleteTextures(1, &t0);
        glDeleteTextures(1, &t1);

        /* Out-of-range unit selection must be refused. */
        glActiveTexture(GL_TEXTURE0 + 15);
        check(glGetError() == GL_INVALID_ENUM, "mt_bad_unit_rejected");
    }

    /* ---- 3D textures ---- */
    {
        static const unsigned char vol[2 * 3] = { 255, 0, 0,   0, 0, 255 };
        GLuint v = 0;
        glGenTextures(1, &v);
        glBindTexture(GL_TEXTURE_3D, v);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB, 1, 1, 2, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, vol);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        check(glGetError() == GL_NO_ERROR, "tex3d_upload");

        glEnable(GL_TEXTURE_3D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBegin(GL_QUADS);
        glTexCoord3f(0.5f, 0.5f, 0.25f); glVertex3f(10, 10, 0);
        glTexCoord3f(0.5f, 0.5f, 0.25f); glVertex3f(GL_W / 2 - 5, 10, 0);
        glTexCoord3f(0.5f, 0.5f, 0.25f); glVertex3f(GL_W / 2 - 5, GL_H - 10, 0);
        glTexCoord3f(0.5f, 0.5f, 0.25f); glVertex3f(10, GL_H - 10, 0);
        glEnd();
        check(AT(GL_W / 4, GL_H / 2) == 0xFF0000, "tex3d_slice0");

        glBegin(GL_QUADS);
        glTexCoord3f(0.5f, 0.5f, 0.75f); glVertex3f(GL_W / 2 + 5, 10, 0);
        glTexCoord3f(0.5f, 0.5f, 0.75f); glVertex3f(GL_W - 10, 10, 0);
        glTexCoord3f(0.5f, 0.5f, 0.75f); glVertex3f(GL_W - 10, GL_H - 10, 0);
        glTexCoord3f(0.5f, 0.5f, 0.75f); glVertex3f(GL_W / 2 + 5, GL_H - 10, 0);
        glEnd();
        check(AT(GL_W * 3 / 4, GL_H / 2) == 0x0000FF, "tex3d_slice1");

        glDisable(GL_TEXTURE_3D);
        glDeleteTextures(1, &v);
    }

    /* ---- Cube maps ---- */
    {
        static const unsigned char face[6][3] = {
            { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 },
            { 255, 255, 0 }, { 255, 0, 255 }, { 0, 255, 255 },
        };
        static const uint32_t want[6] = {
            0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF
        };
        static const float dir[6][3] = {
            {  1, 0, 0 }, { -1, 0, 0 }, { 0,  1, 0 },
            {  0,-1, 0 }, {  0, 0, 1 }, { 0,  0,-1 },
        };

        GLuint cm = 0;
        glGenTextures(1, &cm);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cm);
        for (int f = 0; f < 6; f++) {
            glTexImage2D((GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f), 0,
                         GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, face[f]);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        check(glGetError() == GL_NO_ERROR, "cube_upload");

        glEnable(GL_TEXTURE_CUBE_MAP);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

        int faces_ok = 1;
        for (int f = 0; f < 6; f++) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glBegin(GL_QUADS);
            glTexCoord3f(dir[f][0], dir[f][1], dir[f][2]);
            glVertex3f(10, 10, 0);
            glTexCoord3f(dir[f][0], dir[f][1], dir[f][2]);
            glVertex3f(GL_W - 10, 10, 0);
            glTexCoord3f(dir[f][0], dir[f][1], dir[f][2]);
            glVertex3f(GL_W - 10, GL_H - 10, 0);
            glTexCoord3f(dir[f][0], dir[f][1], dir[f][2]);
            glVertex3f(10, GL_H - 10, 0);
            glEnd();
            if (AT(GL_W / 2, GL_H / 2) != want[f]) faces_ok = 0;
        }
        check(faces_ok, "cube_six_faces");

        glDisable(GL_TEXTURE_CUBE_MAP);
        glDeleteTextures(1, &cm);
    }

    /* ---- GL_CLAMP_TO_BORDER ---- */
    {
        static const unsigned char red[3] = { 255, 0, 0 };
        static const GLfloat blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        GLuint b = 0;
        glGenTextures(1, &b);
        glBindTexture(GL_TEXTURE_2D, b);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, red);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, blue);
        check(glGetError() == GL_NO_ERROR, "border_setup");

        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex3f(10, 10, 0);
        glTexCoord2f(3, 0); glVertex3f(GL_W - 10, 10, 0);
        glTexCoord2f(3, 1); glVertex3f(GL_W - 10, GL_H - 10, 0);
        glTexCoord2f(0, 1); glVertex3f(10, GL_H - 10, 0);
        glEnd();
        check(AT(14, GL_H / 2) == 0xFF0000, "border_inside_is_texel");
        check(AT(GL_W - 14, GL_H / 2) == 0x0000FF, "border_outside_is_border");

        glDisable(GL_TEXTURE_2D);
        glDeleteTextures(1, &b);
    }

    check(aglxSwapBuffers(ctx) == 0, "tex2_swap");
    check(glGetError() == GL_NO_ERROR, "tex2_no_pending_error");

    #undef MINI_VAR
    #undef MINI_QUAD
    #undef AT
    aglxDestroyContext(ctx);
}


/* ---- Phase G12: framebuffer objects, renderbuffers, glReadPixels ----
 *
 * The claim under test is that rendering into a texture and then sampling it
 * returns what was drawn.  Verified on the target as well as on the host
 * because the render target redirection touches the same row-addressing code
 * that has produced an orientation bug once already.
 */
static void test_gl_fbo(int wid) {
    printf("[gl] --- G12: framebuffer objects and glReadPixels ---\n");

    aglx_context_t *ctx = aglxCreateContext(wid, GL_W, GL_H, AGLX_DEPTH);
    check(ctx != NULL, "fbo_ctx_create");
    if (!ctx) return;
    aglxMakeCurrent(ctx);

    const uint32_t *cb = aglxGetColorBuffer(ctx);
    #define AT(x, y) cb[(size_t)(GL_H - 1 - (y)) * GL_W + (x)]

    #define ORTHO_FOR(n) do {                                   \
        glViewport(0, 0, (n), (n));                             \
        glMatrixMode(GL_PROJECTION); glLoadIdentity();          \
        glOrtho(0, (n), 0, (n), -10, 10);                       \
        glMatrixMode(GL_MODELVIEW);  glLoadIdentity();          \
    } while (0)

    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glColor3f(1, 1, 1);

    /* ---- Object management ---- */
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    check(fbo != 0, "fbo_gen");
    check(glIsFramebuffer(fbo) == GL_TRUE, "fbo_is_framebuffer");

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    {
        GLint v = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &v);
        check(v == (GLint)fbo, "fbo_binding_query");
    }

    /* Nothing attached yet: incomplete, and drawing must be refused rather
     * than falling back to the window. */
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER)
          == GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT,
          "fbo_incomplete_when_empty");
    while (glGetError() != GL_NO_ERROR) { }
    glClear(GL_COLOR_BUFFER_BIT);
    check(glGetError() == GL_INVALID_FRAMEBUFFER_OPERATION,
          "fbo_incomplete_refuses_clear");

    /* ---- Render to texture ---- */
    #define FBO_N 32
    GLuint target = 0;
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FBO_N, FBO_N, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, target, 0);
    check(glGetError() == GL_NO_ERROR, "fbo_attach_texture");
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
          "fbo_complete_with_texture");

    /* Paint the window first, so a stray write into it is detectable. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ORTHO_FOR(GL_W);
    glClearColor(0, 0.5f, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Now render off-screen: red background, blue bottom-left quadrant. */
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    ORTHO_FOR(FBO_N);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(0, 0, 1);
    glBegin(GL_QUADS);
    glVertex3f(0,        0,        0);
    glVertex3f(FBO_N/2,  0,        0);
    glVertex3f(FBO_N/2,  FBO_N/2,  0);
    glVertex3f(0,        FBO_N/2,  0);
    glEnd();
    check(glGetError() == GL_NO_ERROR, "fbo_render_offscreen");

    /* glReadPixels must read the FBO, not the window. */
    {
        unsigned char rgb[3];
        glReadPixels(FBO_N / 4, FBO_N / 4, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
        check(rgb[2] == 255 && rgb[0] == 0, "fbo_readpixels_blue_quadrant");
        glReadPixels(FBO_N * 3 / 4, FBO_N * 3 / 4, 1, 1, GL_RGB,
                     GL_UNSIGNED_BYTE, rgb);
        check(rgb[0] == 255 && rgb[2] == 0, "fbo_readpixels_red_background");
    }

    /* The window must be untouched by all of that. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ORTHO_FOR(GL_W);
    {
        uint32_t p = AT(GL_W / 2, GL_H / 2);
        check(((p >> 8) & 0xFF) > 120 && ((p >> 8) & 0xFF) < 136 &&
              ((p >> 16) & 0xFF) == 0, "fbo_window_untouched");
    }

    /* ---- The round trip: sample the rendered texture ---- */
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(1, 1, 1);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(0,    0,    0);
    glTexCoord2f(1, 0); glVertex3f(GL_W, 0,    0);
    glTexCoord2f(1, 1); glVertex3f(GL_W, GL_H, 0);
    glTexCoord2f(0, 1); glVertex3f(0,    GL_H, 0);
    glEnd();

    /* Bottom-left quarter blue, the rest red.  This also proves the row order
     * is right: an inverted target would put the blue at the top. */
    check(AT(GL_W / 4,     GL_H / 4)     == 0x0000FF, "fbo_roundtrip_bottomleft");
    check(AT(GL_W * 3 / 4, GL_H / 4)     == 0xFF0000, "fbo_roundtrip_bottomright");
    check(AT(GL_W / 4,     GL_H * 3 / 4) == 0xFF0000, "fbo_roundtrip_topleft");
    check(AT(GL_W * 3 / 4, GL_H * 3 / 4) == 0xFF0000, "fbo_roundtrip_topright");

    /* A texture rendered into must sample as OPAQUE: the rasterizer writes
     * 0x00RRGGBB and the sampler reads the alpha byte back. */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord2f(0.9f, 0.9f); glVertex3f(0,    0,    0);
    glTexCoord2f(0.9f, 0.9f); glVertex3f(GL_W, 0,    0);
    glTexCoord2f(0.9f, 0.9f); glVertex3f(GL_W, GL_H, 0);
    glTexCoord2f(0.9f, 0.9f); glVertex3f(0,    GL_H, 0);
    glEnd();
    check(AT(GL_W / 2, GL_H / 2) == 0xFF0000, "fbo_rendered_texture_opaque");
    glDisable(GL_TEXTURE_2D);

    /* ---- Depth renderbuffer ---- */
    {
        GLuint rb = 0;
        glGenRenderbuffers(1, &rb);
        check(rb != 0, "rbo_gen");
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                              FBO_N, FBO_N);
        check(glGetError() == GL_NO_ERROR, "rbo_storage");

        GLint v = 0;
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &v);
        check(v == FBO_N, "rbo_width_query");

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, rb);
        check(glCheckFramebufferStatus(GL_FRAMEBUFFER)
              == GL_FRAMEBUFFER_COMPLETE, "fbo_complete_with_depth");

        ORTHO_FOR(FBO_N);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        /* glOrtho negates z, so +z is NEARER.  Near red, then far blue: the
         * blue must lose. */
        glColor3f(1, 0, 0);
        glBegin(GL_QUADS);
        glVertex3f(0, 0, 5); glVertex3f(FBO_N, 0, 5);
        glVertex3f(FBO_N, FBO_N, 5); glVertex3f(0, FBO_N, 5);
        glEnd();
        glColor3f(0, 0, 1);
        glBegin(GL_QUADS);
        glVertex3f(0, 0, -5); glVertex3f(FBO_N, 0, -5);
        glVertex3f(FBO_N, FBO_N, -5); glVertex3f(0, FBO_N, -5);
        glEnd();
        {
            unsigned char rgb[3];
            glReadPixels(FBO_N / 2, FBO_N / 2, 1, 1, GL_RGB,
                         GL_UNSIGNED_BYTE, rgb);
            check(rgb[0] == 255 && rgb[2] == 0, "fbo_depth_test_works");
        }
        glDisable(GL_DEPTH_TEST);

        /* A dimension mismatch must be diagnosed, not silently rendered. */
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 8, 8);
        check(glCheckFramebufferStatus(GL_FRAMEBUFFER)
              == GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS,
              "fbo_dimension_mismatch_detected");

        /* Deleting the renderbuffer must detach it everywhere. */
        glDeleteRenderbuffers(1, &rb);
        check(glCheckFramebufferStatus(GL_FRAMEBUFFER)
              == GL_FRAMEBUFFER_COMPLETE, "rbo_delete_detaches");
    }

    /* ---- glReadPixels against the window ---- */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ORTHO_FOR(GL_W);
    glClearColor(0.2f, 0.4f, 0.6f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    {
        unsigned char rgba[4];
        glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        check(rgba[0] > 46 && rgba[0] < 56 &&
              rgba[1] > 97 && rgba[1] < 107 &&
              rgba[2] > 148 && rgba[2] < 158 &&
              rgba[3] == 255, "readpixels_rgba");

        unsigned char bgr[3];
        glReadPixels(10, 10, 1, 1, GL_BGR, GL_UNSIGNED_BYTE, bgr);
        check(bgr[0] == rgba[2] && bgr[2] == rgba[0], "readpixels_bgr_swaps");

        /* Rows come back bottom-first: a band along the bottom must read as
         * present at low y and absent higher up. */
        glColor3f(1, 1, 0);
        glBegin(GL_QUADS);
        glVertex3f(0, 0, 0); glVertex3f(GL_W, 0, 0);
        glVertex3f(GL_W, 8, 0); glVertex3f(0, 8, 0);
        glEnd();
        unsigned char band[2 * 3];
        glReadPixels(GL_W / 2, 7, 1, 2, GL_RGB, GL_UNSIGNED_BYTE, band);
        check(band[0] == 255 && band[1] == 255 && band[3] != 255,
              "readpixels_row_order_bottom_first");

        unsigned char d = 0;
        glReadPixels(GL_W / 2, GL_H - 2, 1, 1, GL_DEPTH_COMPONENT,
                     GL_UNSIGNED_BYTE, &d);
        check(d == 255, "readpixels_depth_far");

        /* Out of bounds must be zero-filled, not a fault. */
        unsigned char oob[3] = { 9, 9, 9 };
        glReadPixels(GL_W + 100, GL_H + 100, 1, 1, GL_RGB,
                     GL_UNSIGNED_BYTE, oob);
        check(oob[0] == 0 && oob[1] == 0 && oob[2] == 0,
              "readpixels_out_of_bounds_zero");

        /* Validation. */
        while (glGetError() != GL_NO_ERROR) { }
        glReadPixels(0, 0, 1, 1, GL_RGB, GL_FLOAT, oob);
        check(glGetError() == GL_INVALID_ENUM, "readpixels_bad_type");
    }

    /* ---- Cleanup and the state it leaves behind ---- */
    glDeleteFramebuffers(1, &fbo);
    check(glIsFramebuffer(fbo) == GL_FALSE, "fbo_delete");
    {
        GLint v = -1;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &v);
        check(v == 0, "fbo_delete_reverts_binding");
    }
    glDeleteTextures(1, &target);

    /* The window must still render normally after all of that. */
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    check(AT(GL_W / 2, GL_H / 2) == 0xFF00FF, "fbo_window_still_usable");

    check(aglxSwapBuffers(ctx) == 0, "fbo_swap");
    check(glGetError() == GL_NO_ERROR, "fbo_no_pending_error");

    #undef FBO_N
    #undef ORTHO_FOR
    #undef AT
    aglxDestroyContext(ctx);
}

/* ---- Phase G7: vertex arrays, VBOs, display lists ---- */
static void test_gl_arrays(int wid) {
    printf("[gl] --- G7: arrays, VBOs, display lists ---\n");

    aglx_context_t *ctx = aglxCreateContext(wid, GL_W, GL_H, AGLX_DEPTH);
    check(ctx != NULL, "arr_ctx_create");
    if (!ctx) return;
    aglxMakeCurrent(ctx);

    const uint32_t *cb = aglxGetColorBuffer(ctx);
    static uint32_t snap[GL_W * GL_H];

    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, GL_W, 0, GL_H, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);

    static const GLfloat pos[9] = {
        10.0f, 10.0f, 0.0f,  90.0f, 10.0f, 0.0f,  10.0f, 90.0f, 0.0f,
    };
    static const GLfloat col[9] = {
        1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f,
    };

    #define LIT(n) do { n = 0; for (int i = 0; i < GL_W * GL_H; i++) if (cb[i]) n++; } while (0)
    int n;

    /* Reference: immediate mode. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        glColor3f(col[i*3], col[i*3+1], col[i*3+2]);
        glVertex3f(pos[i*3], pos[i*3+1], pos[i*3+2]);
    }
    glEnd();
    for (int i = 0; i < GL_W * GL_H; i++) snap[i] = cb[i];
    LIT(n);
    check(n > 500, "arr_reference_drawn");

    /* glDrawArrays must match it pixel for pixel. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glColorPointer(3, GL_FLOAT, 0, col);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    {
        int same = 1;
        for (int i = 0; i < GL_W * GL_H; i++) if (cb[i] != snap[i]) { same = 0; break; }
        check(same, "arr_drawarrays_matches_immediate");
    }

    /* glDrawElements must match as well. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    {
        static const unsigned short idx[3] = { 0, 1, 2 };
        glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, idx);
        int same = 1;
        for (int i = 0; i < GL_W * GL_H; i++) if (cb[i] != snap[i]) { same = 0; break; }
        check(same, "arr_drawelements_matches");
    }
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    /* Buffer objects: the same geometry from VRAM-side storage. */
    GLuint vb = 0, cbuf = 0;
    glGenBuffers(1, &vb);
    check(vb != 0 && glIsBuffer(vb) == GL_TRUE, "arr_buffer_gen");
    glBindBuffer(GL_ARRAY_BUFFER, vb);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(pos), pos, GL_STATIC_DRAW);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);

    glGenBuffers(1, &cbuf);
    glBindBuffer(GL_ARRAY_BUFFER, cbuf);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(col), col, GL_STATIC_DRAW);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(3, GL_FLOAT, 0, (const GLvoid *)0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    {
        int same = 1;
        for (int i = 0; i < GL_W * GL_H; i++) if (cb[i] != snap[i]) { same = 0; break; }
        check(same, "arr_vbo_matches_client");
    }
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    /* Deleting a buffer an array still references must disarm it, not leave a
     * dangling offset for the next draw to follow. */
    glEnableClientState(GL_VERTEX_ARRAY);
    glDeleteBuffers(1, &vb);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    LIT(n);
    check(n == 0, "arr_deleted_buffer_disarmed");
    glDisableClientState(GL_VERTEX_ARRAY);
    glDeleteBuffers(1, &cbuf);

    /* Display lists. */
    GLuint list = glGenLists(1);
    check(list != 0 && glIsList(list) == GL_TRUE, "arr_list_gen");

    glNewList(list, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        glColor3f(col[i*3], col[i*3+1], col[i*3+2]);
        glVertex3f(pos[i*3], pos[i*3+1], pos[i*3+2]);
    }
    glEnd();
    glEndList();

    /* GL_COMPILE must not have drawn anything. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    LIT(n);
    check(n == 0, "arr_list_compile_silent");

    glCallList(list);
    {
        int same = 1;
        for (int i = 0; i < GL_W * GL_H; i++) if (cb[i] != snap[i]) { same = 0; break; }
        check(same, "arr_list_matches_immediate");
    }

    /* A list must record matrix operations, not apply them at compile time. */
    GLuint mlist = glGenLists(1);
    glNewList(mlist, GL_COMPILE);
    glPushMatrix();
    glTranslatef(40.0f, 40.0f, 0.0f);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 1, 1);
    glVertex3f(0, 0, 0); glVertex3f(30, 0, 0); glVertex3f(0, 30, 0);
    glEnd();
    glPopMatrix();
    glEndList();

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glCallList(mlist);
    check(cb[(size_t)(GL_H - 1 - 50) * GL_W + 50] != 0, "arr_list_matrix_replayed");
    check(cb[(size_t)(GL_H - 1 - 5) * GL_W + 5] == 0, "arr_list_matrix_not_at_origin");

    /* A self-calling list must terminate rather than exhausting the stack. */
    GLuint rlist = glGenLists(1);
    glNewList(rlist, GL_COMPILE);
    glCallList(rlist);
    glEndList();
    glCallList(rlist);
    check(1, "arr_list_recursion_survived");

    /* Calling an undefined list is a silent no-op. */
    glCallList(60000);
    check(glGetError() == GL_NO_ERROR, "arr_call_undefined_list");

    /* Validation. */
    glDrawArrays(0x9999, 0, 3);
    check(glGetError() == GL_INVALID_ENUM, "arr_bad_mode");
    glEnableClientState(0x9999);
    check(glGetError() == GL_INVALID_ENUM, "arr_bad_client_state");
    glNewList(0, GL_COMPILE);
    check(glGetError() == GL_INVALID_VALUE, "arr_bad_list_name");

    glDeleteLists(list, 1);
    glDeleteLists(mlist, 1);
    glDeleteLists(rlist, 1);
    check(glIsList(list) == GL_FALSE, "arr_list_delete");

    check(aglxSwapBuffers(ctx) == 0, "arr_swap");
    check(glGetError() == GL_NO_ERROR, "arr_no_pending_error");

    #undef LIT
    aglxDestroyContext(ctx);
}

/* ---- Phase G8: the GLU utility layer ---- */
static void test_gl_glu(int wid) {
    printf("[gl] --- G8: GLU utility layer ---\n");

    aglx_context_t *ctx = aglxCreateContext(wid, GL_W, GL_H, AGLX_DEPTH);
    check(ctx != NULL, "glu_ctx_create");
    if (!ctx) return;
    aglxMakeCurrent(ctx);

    const uint32_t *cb = aglxGetColorBuffer(ctx);
    #define LIT(n) do { n = 0; for (int i = 0; i < GL_W * GL_H; i++) if (cb[i]) n++; } while (0)
    int n;

    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glColor3f(1, 1, 1);

    /* gluPerspective must equal the frustum it stands for.  Its fovy is in
     * DEGREES, which is the usual place to go wrong. */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(90.0, 1.0, 1.0, 100.0);
    {
        GLfloat m[16];
        glGetFloatv(GL_PROJECTION_MATRIX, m);
        /* tan(45 deg) == 1, so both scale terms are exactly 1. */
        int ok = (m[0] > 0.99f && m[0] < 1.01f)
              && (m[5] > 0.99f && m[5] < 1.01f)
              && (m[11] < -0.99f && m[11] > -1.01f);
        check(ok, "glu_perspective_90deg");
    }

    /* gluLookAt must put the eye at the view-space origin. */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(0, 0, 10,  0, 0, 0,  0, 1, 0);
    {
        GLfloat m[16];
        glGetFloatv(GL_MODELVIEW_MATRIX, m);
        check(m[14] < -9.9f && m[14] > -10.1f, "glu_lookat_translation");
    }

    /* gluErrorString must always return a usable string. */
    {
        const char *s1 = (const char *)gluErrorString(GL_INVALID_ENUM);
        const char *s2 = (const char *)gluErrorString(0x9999);
        check(s1 != NULL && s1[0] != 0, "glu_error_string");
        check(s2 != NULL && s2[0] != 0, "glu_error_string_unknown");
    }

    /* Quadrics: a filled sphere must cover its projected disk. */
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-2, 2, -2, 2, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    {
        GLUquadric *q = gluNewQuadric();
        check(q != NULL, "glu_quadric_create");
        gluSphere(q, 1.0, 20, 16);
        gluDeleteQuadric(q);
    }
    LIT(n);
    check(n > 400, "glu_sphere_filled");
    check(cb[(size_t)(GL_H / 2) * GL_W + (GL_W / 2)] != 0, "glu_sphere_centre");

    /* GLU_LINE must produce an outline rather than a solid body. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    {
        GLUquadric *q = gluNewQuadric();
        gluQuadricDrawStyle(q, GLU_LINE);
        gluSphere(q, 1.0, 20, 16);
        gluDeleteQuadric(q);
    }
    int wire;
    LIT(wire);
    check(wire > 0 && wire < n, "glu_sphere_wireframe");

    /* An annulus must leave its middle empty. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    {
        GLUquadric *q = gluNewQuadric();
        gluDisk(q, 0.7, 1.0, 24, 2);
        gluDeleteQuadric(q);
    }
    check(cb[(size_t)(GL_H / 2) * GL_W + (GL_W / 2)] == 0, "glu_disk_hole");
    LIT(n);
    check(n > 80, "glu_disk_ring");

    /* A cone is a cylinder with a zero top radius: must not divide by zero. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glPushMatrix();
    glRotatef(90.0f, 1.0f, 0.0f, 0.0f);
    {
        GLUquadric *q = gluNewQuadric();
        gluCylinder(q, 1.0, 0.0, 2.0, 16, 4);
        gluDeleteQuadric(q);
    }
    glPopMatrix();
    LIT(n);
    check(n > 50, "glu_cone");

    /* Degenerate parameters must be refused quietly. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    {
        GLUquadric *q = gluNewQuadric();
        gluSphere(q, -1.0, 16, 12);
        gluSphere(q, 1.0, 1, 12);
        gluSphere(NULL, 1.0, 16, 12);
        gluDeleteQuadric(q);
        gluDeleteQuadric(NULL);
    }
    LIT(n);
    check(n == 0, "glu_degenerate_safe");
    check(glGetError() == GL_NO_ERROR, "glu_degenerate_no_error");

    /* Quadrics must light: they emit real per-vertex normals.  The depth test
     * matters here — a sphere draws both hemispheres, and without it the far,
     * unlit side would cover the near one. */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    {
        GLfloat pos[4] = { 0, 0, 1, 0 };
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
        GLfloat white[4] = { 1, 1, 1, 1 };
        glMaterialfv(GL_FRONT, GL_DIFFUSE, white);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    {
        GLUquadric *q = gluNewQuadric();
        gluSphere(q, 1.0, 24, 18);
        gluDeleteQuadric(q);
    }
    {
        uint32_t centre = cb[(size_t)(GL_H / 2) * GL_W + (GL_W / 2)];
        uint32_t rim    = cb[(size_t)(GL_H / 2) * GL_W + (GL_W / 2 + 22)];
        check(((centre >> 16) & 0xFF) > ((rim >> 16) & 0xFF) + 30,
              "glu_sphere_lit_gradient");
    }
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);

    /* GLU through a display list, which is how the demos use it. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    {
        GLUquadric *q = gluNewQuadric();
        GLuint list = glGenLists(1);
        glNewList(list, GL_COMPILE);
        gluSphere(q, 1.0, 16, 12);
        glEndList();
        gluDeleteQuadric(q);

        LIT(n);
        check(n == 0, "glu_list_compile_silent");
        glCallList(list);
        LIT(n);
        check(n > 400, "glu_list_replays");
        glDeleteLists(list, 1);
    }

    check(aglxSwapBuffers(ctx) == 0, "glu_swap");
    check(glGetError() == GL_NO_ERROR, "glu_no_pending_error");

    #undef LIT
    aglxDestroyContext(ctx);
}

/* ---- Phase G9: the backend seam ---- */
static void test_gl_backend(int wid) {
    printf("[gl] --- G9: backend seam ---\n");

    aglx_context_t *ctx = aglxCreateContext(wid, 64, 64, AGLX_DEPTH);
    check(ctx != NULL, "bk_ctx_create");
    if (!ctx) return;
    aglxMakeCurrent(ctx);

    /* A backend is always active: the software one is the guaranteed
     * fallback, so no caller ever has to handle a NULL. */
    const gl_backend_info_t *bi = gl_backend_info();
    check(bi != NULL, "bk_info_available");
    check(gl_backend_active() != NULL, "bk_active_never_null");
    if (!bi) { aglxDestroyContext(ctx); return; }

    /* On this machine there is no user-space GPU path, so software must win. */
    check(bi->hardware == 0, "bk_software_active");
    check((bi->flags & GL_BACKEND_SOFTWARE) != 0, "bk_software_flag");
    printf("[gl] backend: %s (hardware=%d)\n", bi->name, bi->hardware);

    /* glGetString(GL_RENDERER) must report whichever backend is live, so an
     * application can tell which path it is on. */
    {
        const char *r = (const char *)glGetString(GL_RENDERER);
        int same = (r && bi->name && strcmp(r, bi->name) == 0);
        check(same, "bk_renderer_matches_backend");
    }

    /* The VirGL candidate is registered but declines until the kernel exposes
     * a user-space 3D submission path.  Forcing it must find it (proving it is
     * in the registry) yet still leave a non-VirGL backend active. */
    gl_virgl_register();
    {
        int found = gl_backend_force("AuraLite VirGL (virtio-gpu)");
        check(found == 0, "bk_virgl_registered");
        const gl_backend_info_t *after = gl_backend_info();
        check(after && after->name &&
              strcmp(after->name, "AuraLite VirGL (virtio-gpu)") != 0,
              "bk_virgl_declines");
        gl_backend_force(NULL);
    }

    /* An unknown name must be refused rather than silently ignored. */
    check(gl_backend_force("No Such Backend") != 0, "bk_force_unknown_fails");
    gl_backend_force(NULL);

    /* Rendering must be unaffected by the seam: the whole point is that the
     * pipeline above it does not care which backend is underneath. */
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, 64, 0, 64, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glDisable(GL_LIGHTING); glDisable(GL_TEXTURE_2D);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(8, 8, 0); glVertex3f(56, 8, 0); glVertex3f(8, 56, 0);
    glEnd();
    {
        const uint32_t *cb = aglxGetColorBuffer(ctx);
        int lit = 0;
        for (int i = 0; i < 64 * 64; i++) if (cb[i]) lit++;
        check(lit > 400, "bk_rendering_unaffected");
    }

    check(aglxSwapBuffers(ctx) == 0, "bk_swap");
    check(glGetError() == GL_NO_ERROR, "bk_no_pending_error");

    aglxDestroyContext(ctx);
}

int main(void) {
    printf("[gl] === AuraLite GL test (phases G0-G9) ===\n");

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

    /* ---- Phase G1-G9 checks ---- */
    test_gl_context(wid);
    test_gl_geometry(wid);
    test_gl_raster(wid);
    test_gl_clip(wid);
    test_gl_light(wid);
    test_gl_texture(wid);
    test_gl_texture2(wid);
    test_gl_fbo(wid);
    test_gl_arrays(wid);
    test_gl_glu(wid);
    test_gl_backend(wid);

    free(buf);
    ag_window_destroy(wid);

    printf("[gl] SUMMARY %d checks, %d failed\n", checks, fails);
    if (fails == 0) printf("[gl] ALL TESTS PASSED\n");
    return fails ? 1 : 0;
}

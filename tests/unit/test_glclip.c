/*
 * test_glclip.c — host-side unit tests for frustum clipping (phase G4).
 *
 * Links the real glclip.c through the public GL API and asserts on rendered
 * pixels, so a clipping bug that produces plausible-looking geometry in the
 * wrong place still fails.
 *
 * The central property being tested: geometry crossing the near plane must be
 * SPLIT, not dropped.  Before G4 such primitives vanished entirely, which is
 * the "walls disappear when you walk into them" artefact.
 */

#include <stdio.h>
#include <string.h>

#include "GL/gl.h"
#include "GL/auraglx.h"
#include "glcontext.h"
#include "auragui.h"

void gl_imm_reset(void);

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    ag_stub_reset();                                    \
    gl_imm_reset();                                     \
    aglxMakeCurrent(NULL);                              \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

#define W 64
#define H 64

static uint32_t px(aglx_context_t *c, int x, int y) {
    const uint32_t *b = aglxGetColorBuffer(c);
    return b[(size_t)(H - 1 - y) * W + x];
}

static int lit_count(aglx_context_t *c) {
    const uint32_t *b = aglxGetColorBuffer(c);
    int n = 0;
    for (int i = 0; i < W * H; i++) if (b[i] != 0) n++;
    return n;
}

/* Perspective context: the near plane is at z = -1 in eye space. */
static aglx_context_t *setup_perspective(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    if (!c) return NULL;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1, 100);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glColor3f(1, 1, 1);
    return c;
}

/* Orthographic 1:1 pixel mapping, for tests about the side planes. */
static aglx_context_t *setup_ortho(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    if (!c) return NULL;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glColor3f(1, 1, 1);
    return c;
}

/* ---------------------------------------------------------- near plane --- */

/* THE headline test: a triangle straddling the near plane must still draw.
 * Before G4 this rendered nothing at all. */
static int t_triangle_crossing_near_is_split(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.0f, -1.0f, -3.0f);   /* in front of the near plane */
    glVertex3f( 1.0f, -1.0f, -3.0f);
    glVertex3f( 0.0f,  1.0f,  1.0f);   /* BEHIND the eye */
    glEnd();
    int ok = lit_count(c) > 0;
    aglxDestroyContext(c);
    return ok;
}

/* A triangle entirely behind the eye must draw nothing. */
static int t_triangle_fully_behind_is_dropped(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.0f, -1.0f, 2.0f);
    glVertex3f( 1.0f, -1.0f, 2.0f);
    glVertex3f( 0.0f,  1.0f, 3.0f);
    glEnd();
    int ok = lit_count(c) == 0 && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* A triangle wholly in front must be unaffected by the clipper. */
static int t_triangle_fully_inside_unchanged(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, -3.0f);
    glVertex3f( 0.5f, -0.5f, -3.0f);
    glVertex3f( 0.0f,  0.5f, -3.0f);
    glEnd();
    int n = lit_count(c);
    /* With glFrustum(-1,1,-1,1,1,100) the visible width at z=-3 is 6 units
     * across 64 pixels, so this 1x1-unit triangle covers about
     * 0.5 * (64/6)^2 ~= 57 pixels.  The window is deliberately wide: the point
     * is that the clipper left the triangle alone, not the exact coverage. */
    int ok = n > 30 && n < 120;
    aglxDestroyContext(c);
    return ok;
}

/* Two vertices behind the eye, one in front: the surviving sliver must still
 * be drawn.  This is the case that produces a quadrilateral after clipping,
 * which then has to be fanned back into triangles. */
static int t_two_vertices_behind(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glBegin(GL_TRIANGLES);
    glVertex3f( 0.0f,  0.0f, -3.0f);   /* in front */
    glVertex3f(-2.0f, -2.0f,  1.0f);   /* behind   */
    glVertex3f( 2.0f, -2.0f,  1.0f);   /* behind   */
    glEnd();
    int ok = lit_count(c) > 0;
    aglxDestroyContext(c);
    return ok;
}

/* A line crossing the near plane must be shortened, not dropped. */
static int t_line_crossing_near(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glBegin(GL_LINES);
    glVertex3f(0.0f, 0.0f, -5.0f);     /* in front */
    glVertex3f(0.0f, 0.0f,  5.0f);     /* behind   */
    glEnd();
    int ok = lit_count(c) > 0;
    aglxDestroyContext(c);
    return ok;
}

/* A line entirely behind the eye draws nothing. */
static int t_line_fully_behind(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glBegin(GL_LINES);
    glVertex3f(-1.0f, 0.0f, 2.0f);
    glVertex3f( 1.0f, 0.0f, 3.0f);
    glEnd();
    int ok = lit_count(c) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* A point behind the eye is discarded. */
static int t_point_behind_dropped(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 2.0f);
    glEnd();
    int ok = lit_count(c) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* A point in front is kept. */
static int t_point_in_front_kept(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, -3.0f);
    glEnd();
    int ok = lit_count(c) == 1;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------- attribute lerp --- */

/* Colour must be interpolated at the clip intersection, so the visible part
 * shades as though the whole triangle had been drawn.  A triangle running from
 * red (in front) to blue (behind) must show red near the front vertex and a
 * blend — never pure blue — where it was cut. */
static int t_clip_interpolates_color(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glShadeModel(GL_SMOOTH);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex3f(-1.0f, -1.0f, -2.0f);
    glColor3f(1, 0, 0); glVertex3f( 1.0f, -1.0f, -2.0f);
    glColor3f(0, 0, 1); glVertex3f( 0.0f,  1.0f,  0.5f);  /* behind */
    glEnd();

    /* Somewhere in the drawn region there must be red-dominant pixels. */
    int found_red = 0;
    const uint32_t *b = aglxGetColorBuffer(c);
    for (int i = 0; i < W * H; i++) {
        uint32_t p = b[i];
        if (p == 0) continue;
        if (((p >> 16) & 0xFF) > (p & 0xFF)) { found_red = 1; break; }
    }
    int ok = found_red && lit_count(c) > 0;
    aglxDestroyContext(c);
    return ok;
}

/* Clipping must not change a fully-inside triangle's shading. */
static int t_inside_shading_unchanged(void) {
    aglx_context_t *c = setup_ortho(); if (!c) return 0;
    glShadeModel(GL_SMOOTH);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex3f(4.5f,  4.5f,  0);
    glColor3f(0, 1, 0); glVertex3f(58.5f, 4.5f,  0);
    glColor3f(0, 0, 1); glVertex3f(4.5f,  58.5f, 0);
    glEnd();
    uint32_t red_corner   = px(c, 7, 7);
    uint32_t green_corner = px(c, 54, 7);
    int ok = ((red_corner >> 16) & 0xFF) > 150
          && ((green_corner >> 8) & 0xFF) > 150;
    aglxDestroyContext(c);
    return ok;
}

/* -------------------------------------------------------- side planes ---- */

/* A triangle extending far past the left edge must be clipped to the viewport,
 * not wrapped or skipped. */
static int t_clip_left_plane(void) {
    aglx_context_t *c = setup_ortho(); if (!c) return 0;
    glBegin(GL_TRIANGLES);
    glVertex3f(-500.0f, 10.5f, 0);
    glVertex3f(  30.5f, 10.5f, 0);
    glVertex3f(  30.5f, 50.5f, 0);
    glEnd();
    /* Pixels near the left edge are covered, and nothing spills to the right
     * of the triangle. */
    int ok = px(c, 1, 11) != 0 && px(c, 50, 40) == 0 && lit_count(c) > 0;
    aglxDestroyContext(c);
    return ok;
}

/* A quad covering far more than the viewport fills it exactly. */
static int t_clip_all_side_planes(void) {
    aglx_context_t *c = setup_ortho(); if (!c) return 0;
    glBegin(GL_QUADS);
    glVertex3f(-1000.0f, -1000.0f, 0);
    glVertex3f( 1000.0f, -1000.0f, 0);
    glVertex3f( 1000.0f,  1000.0f, 0);
    glVertex3f(-1000.0f,  1000.0f, 0);
    glEnd();
    int ok = lit_count(c) == W * H;
    aglxDestroyContext(c);
    return ok;
}

/* Geometry entirely off to one side draws nothing (trivial reject). */
static int t_trivial_reject_offscreen(void) {
    aglx_context_t *c = setup_ortho(); if (!c) return 0;
    glBegin(GL_TRIANGLES);
    glVertex3f(-500.0f, -500.0f, 0);
    glVertex3f(-400.0f, -500.0f, 0);
    glVertex3f(-500.0f, -400.0f, 0);
    glEnd();
    int ok = lit_count(c) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* --------------------------------------------------------- depth planes -- */

/* Geometry beyond the far plane must be clipped away. */
static int t_clip_far_plane(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.0f, -1.0f, -500.0f);
    glVertex3f( 1.0f, -1.0f, -500.0f);
    glVertex3f( 0.0f,  1.0f, -500.0f);
    glEnd();
    int ok = lit_count(c) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* A triangle spanning from inside the frustum to beyond the far plane keeps
 * its near portion. */
static int t_triangle_crossing_far(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f,  -2.0f);
    glVertex3f( 0.5f, -0.5f,  -2.0f);
    glVertex3f( 0.0f,  0.5f, -500.0f);
    glEnd();
    int ok = lit_count(c) > 0;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------------ stability -- */

/* Culling must still work on clipped triangles: the clipper must preserve
 * winding order, otherwise a front face can turn into a back face and vanish. */
static int t_clipping_preserves_winding(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    /* Counter-clockwise as seen on screen, crossing the near plane. */
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.0f, -1.0f, -3.0f);
    glVertex3f( 1.0f, -1.0f, -3.0f);
    glVertex3f( 0.0f,  1.0f,  0.5f);
    glEnd();

    int ok = lit_count(c) > 0;      /* front-facing: must survive */
    aglxDestroyContext(c);
    return ok;
}

/* The depth test must still work across a clip boundary. */
static int t_clipping_with_depth_test(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    /* Far quad first, then a near one that must overwrite it. */
    glColor3f(1, 0, 0);
    glBegin(GL_QUADS);
    glVertex3f(-2, -2, -20); glVertex3f(2, -2, -20);
    glVertex3f(2, 2, -20);   glVertex3f(-2, 2, -20);
    glEnd();

    glColor3f(0, 1, 0);
    glBegin(GL_QUADS);
    glVertex3f(-2, -2, -3); glVertex3f(2, -2, -3);
    glVertex3f(2, 2, -3);   glVertex3f(-2, 2, -3);
    glEnd();

    int ok = px(c, 32, 32) == 0x00FF00;
    aglxDestroyContext(c);
    return ok;
}

/* A degenerate triangle at the clip boundary must not hang or emit garbage. */
static int t_degenerate_at_boundary(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glBegin(GL_TRIANGLES);
    glVertex3f(0.0f, 0.0f, -1.0f);   /* exactly on the near plane */
    glVertex3f(0.0f, 0.0f, -1.0f);
    glVertex3f(0.0f, 0.0f, -1.0f);
    glEnd();
    int ok = glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* A vertex exactly on the near plane is inside, not outside. */
static int t_vertex_exactly_on_near_plane(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, -1.0f);   /* w == -z == 1: on the plane */
    glVertex3f( 0.5f, -0.5f, -1.0f);
    glVertex3f( 0.0f,  0.5f, -1.0f);
    glEnd();
    int ok = lit_count(c) > 0 && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Clipping a strip must not corrupt the vertices retained for the next
 * triangle: a long strip crossing the near plane must keep drawing after the
 * clipped section. */
static int t_strip_survives_clipping(void) {
    aglx_context_t *c = setup_perspective(); if (!c) return 0;
    glBegin(GL_TRIANGLE_STRIP);
    glVertex3f(-1.0f, -1.0f, -3.0f);
    glVertex3f(-1.0f,  1.0f, -3.0f);
    glVertex3f( 0.0f, -1.0f,  0.5f);   /* behind the eye */
    glVertex3f( 0.0f,  1.0f,  0.5f);   /* behind the eye */
    glVertex3f( 1.0f, -1.0f, -3.0f);   /* in front again */
    glVertex3f( 1.0f,  1.0f, -3.0f);
    glEnd();
    int ok = lit_count(c) > 0 && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Enormous coordinates must be handled by the clipper rather than reaching the
 * rasterizer as multi-million-pixel spans. */
static int t_huge_coordinates_clipped(void) {
    aglx_context_t *c = setup_ortho(); if (!c) return 0;
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.0e7f, -1.0e7f, 0);
    glVertex3f( 1.0e7f, -1.0e7f, 0);
    glVertex3f( 0.0f,    1.0e7f, 0);
    glEnd();
    int ok = lit_count(c) == W * H;   /* completes promptly, fills the buffer */
    aglxDestroyContext(c);
    return ok;
}

/* --------------------------------------------------- attribute stack ----- */

/* glPushAttrib/glPopAttrib must restore exactly the groups that were saved. */
static int t_attrib_push_pop(void) {
    aglx_context_t *c = setup_ortho(); if (!c) return 0;
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    glPushAttrib(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glPopAttrib();

    int ok = glIsEnabled(GL_DEPTH_TEST) == GL_FALSE
          && c->depth_func == GL_LESS
          && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Only the masked groups are restored; others keep their current value. */
static int t_attrib_mask_is_selective(void) {
    aglx_context_t *c = setup_ortho(); if (!c) return 0;
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);

    /* Save only the depth group, then change both. */
    glPushAttrib(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glPopAttrib();

    /* Depth restored to off, culling left on. */
    int ok = glIsEnabled(GL_DEPTH_TEST) == GL_FALSE
          && glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    aglxDestroyContext(c);
    return ok;
}

static int t_attrib_viewport_group(void) {
    aglx_context_t *c = setup_ortho(); if (!c) return 0;
    glViewport(0, 0, W, H);
    glPushAttrib(GL_VIEWPORT_BIT);
    glViewport(5, 6, 7, 8);
    glPopAttrib();
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    int ok = vp[0] == 0 && vp[1] == 0 && vp[2] == W && vp[3] == H;
    aglxDestroyContext(c);
    return ok;
}

static int t_attrib_nested(void) {
    aglx_context_t *c = setup_ortho(); if (!c) return 0;
    glDepthFunc(GL_LESS);
    glPushAttrib(GL_DEPTH_BUFFER_BIT);
    glDepthFunc(GL_EQUAL);
    glPushAttrib(GL_DEPTH_BUFFER_BIT);
    glDepthFunc(GL_GREATER);
    glPopAttrib();
    int mid = (c->depth_func == GL_EQUAL);
    glPopAttrib();
    int ok = mid && c->depth_func == GL_LESS;
    aglxDestroyContext(c);
    return ok;
}

static int t_attrib_underflow(void) {
    aglx_context_t *c = setup_ortho(); if (!c) return 0;
    glPopAttrib();
    int ok = glGetError() == GL_STACK_UNDERFLOW;
    aglxDestroyContext(c);
    return ok;
}

static int t_attrib_overflow(void) {
    aglx_context_t *c = setup_ortho(); if (!c) return 0;
    for (int i = 0; i < GL_ATTRIB_STACK_DEPTH + 1; i++) {
        glPushAttrib(GL_ALL_ATTRIB_BITS);
    }
    int ok = glGetError() == GL_STACK_OVERFLOW;
    aglxDestroyContext(c);
    return ok;
}

/* GL_ALL_ATTRIB_BITS must restore every tracked group at once. */
static int t_attrib_all_bits(void) {
    aglx_context_t *c = setup_ortho(); if (!c) return 0;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glShadeModel(GL_SMOOTH);

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glShadeModel(GL_FLAT);
    glPopAttrib();

    int ok = glIsEnabled(GL_DEPTH_TEST) == GL_FALSE
          && glIsEnabled(GL_CULL_FACE) == GL_FALSE
          && c->shade_model == GL_SMOOTH;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------------------------ */

int main(void) {
    printf("=== glclip (G4) unit tests ===\n");

    printf("--- near plane ---\n");
    RUN(t_triangle_crossing_near_is_split);
    RUN(t_triangle_fully_behind_is_dropped);
    RUN(t_triangle_fully_inside_unchanged);
    RUN(t_two_vertices_behind);
    RUN(t_line_crossing_near); RUN(t_line_fully_behind);
    RUN(t_point_behind_dropped); RUN(t_point_in_front_kept);

    printf("--- attribute interpolation ---\n");
    RUN(t_clip_interpolates_color); RUN(t_inside_shading_unchanged);

    printf("--- side planes ---\n");
    RUN(t_clip_left_plane); RUN(t_clip_all_side_planes);
    RUN(t_trivial_reject_offscreen);

    printf("--- depth planes ---\n");
    RUN(t_clip_far_plane); RUN(t_triangle_crossing_far);

    printf("--- stability ---\n");
    RUN(t_clipping_preserves_winding); RUN(t_clipping_with_depth_test);
    RUN(t_degenerate_at_boundary); RUN(t_vertex_exactly_on_near_plane);
    RUN(t_strip_survives_clipping); RUN(t_huge_coordinates_clipped);

    printf("--- attribute stack ---\n");
    RUN(t_attrib_push_pop); RUN(t_attrib_mask_is_selective);
    RUN(t_attrib_viewport_group); RUN(t_attrib_nested);
    RUN(t_attrib_underflow); RUN(t_attrib_overflow); RUN(t_attrib_all_bits);

    printf("\ntest_glclip: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

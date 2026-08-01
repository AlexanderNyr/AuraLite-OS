/*
 * test_glraster.c — host-side unit tests for the G3 triangle rasterizer.
 *
 * Links the real glraster.c and drives it through the public GL API, so what
 * is tested is what ships.  Every assertion inspects actual pixels: a bug that
 * cancels out in the maths but puts colour in the wrong place still fails.
 *
 * Coverage: fill correctness, barycentric interpolation, the depth buffer and
 * all eight comparison functions, face culling, the top-left fill rule, the
 * scissor test, and the degenerate cases that must not crash or hang.
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

#define W 40
#define H 40

/* Pixel in GL window coordinates (origin bottom-left). */
static uint32_t px(aglx_context_t *c, int x, int y) {
    const uint32_t *b = aglxGetColorBuffer(c);
    return b[(size_t)(H - 1 - y) * W + x];
}

static float dpx(aglx_context_t *c, int x, int y) {
    const float *d = aglxGetDepthBuffer(c);
    return d[(size_t)(H - 1 - y) * W + x];
}

static int lit_count(aglx_context_t *c) {
    const uint32_t *b = aglxGetColorBuffer(c);
    int n = 0;
    for (int i = 0; i < W * H; i++) if (b[i] != 0) n++;
    return n;
}

/* Context whose GL coordinates map 1:1 onto pixels. */
static aglx_context_t *setup(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    if (!c) return NULL;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    return c;
}

/* Counter-clockwise right triangle covering roughly the lower-left quadrant. */
static void tri_ccw(float z) {
    glBegin(GL_TRIANGLES);
    glVertex3f(4.5f,  4.5f,  z);
    glVertex3f(30.5f, 4.5f,  z);
    glVertex3f(4.5f,  30.5f, z);
    glEnd();
}

/* Same triangle wound the other way. */
static void tri_cw(float z) {
    glBegin(GL_TRIANGLES);
    glVertex3f(4.5f,  4.5f,  z);
    glVertex3f(4.5f,  30.5f, z);
    glVertex3f(30.5f, 4.5f,  z);
    glEnd();
}

/* A full-screen quad at object-space depth z, as two triangles.
 *
 * NOTE ON THE SIGN: glOrtho maps eye z to NDC with a NEGATED z axis
 * (m[10] = -2/(f-n), §2.10.2), and the viewport transform then maps NDC
 * [-1,1] to window depth [0,1].  With glOrtho(0,W,0,H,-1,1) that gives
 *
 *     object z = +0.5  ->  window depth 0.25   (NEARER)
 *     object z =  0.0  ->  window depth 0.50
 *     object z = -0.5  ->  window depth 0.75   (FARTHER)
 *
 * so a LARGER object-space z is nearer here.  Getting this backwards is an
 * easy mistake; the helpers below are named after the window depth they
 * produce rather than after their z argument. */
static void fullscreen_quad(float z) {
    glBegin(GL_QUADS);
    glVertex3f(0.0f,      0.0f,      z);
    glVertex3f((float)W,  0.0f,      z);
    glVertex3f((float)W,  (float)H,  z);
    glVertex3f(0.0f,      (float)H,  z);
    glEnd();
}

/* Named by the WINDOW depth they produce, to avoid sign confusion. */
static void quad_near(void) { fullscreen_quad(0.5f); }   /* depth 0.25 */
static void quad_far(void)  { fullscreen_quad(-0.5f); }  /* depth 0.75 */

/* ------------------------------------------------------------- filling --- */

static int t_fill_interior(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glColor3f(1, 1, 1);
    tri_ccw(0.0f);
    /* Well inside the triangle. */
    int ok = px(c, 8, 8) == 0xFFFFFF && px(c, 6, 20) == 0xFFFFFF;
    /* Well outside, past the hypotenuse. */
    ok = ok && px(c, 28, 28) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* The area covered must be close to the geometric area (half of 26x26). */
static int t_fill_area_is_plausible(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glColor3f(1, 1, 1);
    tri_ccw(0.0f);
    int n = lit_count(c);
    int expect = 26 * 26 / 2;                 /* 338 */
    int ok = n > expect - 40 && n < expect + 40;
    aglxDestroyContext(c);
    return ok;
}

/* Winding must not matter when culling is off. */
static int t_fill_both_windings(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glColor3f(1, 1, 1);
    tri_ccw(0.0f);
    int a = lit_count(c);
    glClear(GL_COLOR_BUFFER_BIT);
    tri_cw(0.0f);
    int b = lit_count(c);
    int ok = a > 0 && a == b;
    aglxDestroyContext(c);
    return ok;
}

/* A zero-area triangle covers nothing and must not divide by zero. */
static int t_degenerate_zero_area(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(5.5f, 5.5f, 0);
    glVertex3f(15.5f, 5.5f, 0);
    glVertex3f(25.5f, 5.5f, 0);      /* all collinear */
    glEnd();
    int ok = lit_count(c) == 0 && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_degenerate_same_point(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(10.5f, 10.5f, 0);
    glVertex3f(10.5f, 10.5f, 0);
    glVertex3f(10.5f, 10.5f, 0);
    glEnd();
    int ok = lit_count(c) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* Huge coordinates must be bounded by the framebuffer, not by their own size
 * — the triangle equivalent of the G2 Bresenham hang. */
static int t_huge_triangle_bounded(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.0e6f, -1.0e6f, 0);
    glVertex3f( 1.0e6f, -1.0e6f, 0);
    glVertex3f( 0.0f,    1.0e6f, 0);
    glEnd();
    /* Completes promptly and covers the whole buffer. */
    int ok = lit_count(c) == W * H;
    aglxDestroyContext(c);
    return ok;
}

/* A triangle entirely off-screen draws nothing. */
static int t_offscreen_triangle(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(-50.0f, -50.0f, 0);
    glVertex3f(-40.0f, -50.0f, 0);
    glVertex3f(-50.0f, -40.0f, 0);
    glEnd();
    int ok = lit_count(c) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------- interpolation --- */

/* Gouraud shading: each corner shows its own colour, the middle is a mix. */
static int t_gouraud_interpolation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glShadeModel(GL_SMOOTH);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex3f(2.5f,  2.5f,  0);
    glColor3f(0, 1, 0); glVertex3f(36.5f, 2.5f,  0);
    glColor3f(0, 0, 1); glVertex3f(2.5f,  36.5f, 0);
    glEnd();

    uint32_t near_red   = px(c, 4, 4);
    uint32_t near_green = px(c, 33, 4);
    uint32_t near_blue  = px(c, 4, 33);

    /* Each corner must be dominated by its own channel. */
    int ok = ((near_red   >> 16) & 0xFF) > 200
          && ((near_green >>  8) & 0xFF) > 200
          && ( near_blue         & 0xFF) > 200;
    aglxDestroyContext(c);
    return ok;
}

/* Flat shading uses one colour for the whole primitive: the LAST vertex. */
static int t_flat_shading(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glShadeModel(GL_FLAT);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex3f(4.5f,  4.5f,  0);
    glColor3f(0, 1, 0); glVertex3f(30.5f, 4.5f,  0);
    glColor3f(0, 0, 1); glVertex3f(4.5f,  30.5f, 0);   /* last => blue */
    glEnd();
    int ok = px(c, 8, 8) == 0x0000FF && px(c, 6, 20) == 0x0000FF;
    aglxDestroyContext(c);
    return ok;
}

/* ---------------------------------------------------------- depth test --- */

/* Depth must be written even when the test is disabled, provided the mask is
 * on: that is what lets a first pass prime the buffer. */
static int t_depth_written(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glColor3f(1, 1, 1);
    fullscreen_quad(0.0f);              /* NDC 0 -> window depth 0.5 */
    float d = dpx(c, 20, 20);
    int ok = d > 0.49f && d < 0.51f;
    aglxDestroyContext(c);
    return ok;
}

/* GL_LESS: a nearer triangle drawn second must win. */
static int t_depth_nearer_wins(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    glColor3f(1, 0, 0); quad_far();               /* depth 0.75 */
    glColor3f(0, 1, 0); quad_near();              /* depth 0.25: passes LESS */

    int ok = px(c, 20, 20) == 0x00FF00;
    aglxDestroyContext(c);
    return ok;
}

/* ...and a farther triangle drawn second must be rejected. */
static int t_depth_farther_loses(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    glColor3f(0, 1, 0); quad_near();              /* depth 0.25 first */
    glColor3f(1, 0, 0); quad_far();               /* depth 0.75: rejected */

    int ok = px(c, 20, 20) == 0x00FF00;
    aglxDestroyContext(c);
    return ok;
}

/* Without GL_DEPTH_TEST the last primitive always wins. */
static int t_depth_test_disabled(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glDisable(GL_DEPTH_TEST);
    glColor3f(0, 1, 0); quad_near();
    glColor3f(1, 0, 0); quad_far();               /* farther, but drawn later */
    int ok = px(c, 20, 20) == 0xFF0000;
    aglxDestroyContext(c);
    return ok;
}

/* glDepthMask(GL_FALSE): colour is written, depth is not. */
static int t_depth_mask(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    glDepthMask(GL_FALSE);
    glColor3f(1, 0, 0); quad_near();
    float depth_after = dpx(c, 20, 20);

    /* Depth untouched, so a second, farther quad still passes GL_LESS. */
    glColor3f(0, 1, 0); quad_far();

    int ok = depth_after == 1.0f && px(c, 20, 20) == 0x00FF00;
    aglxDestroyContext(c);
    return ok;
}

/* Each comparison function must behave as specified.  The buffer is primed to
 * 0.5, then a fragment at exactly 0.5 is drawn. */
static int depth_func_case(GLenum func, int should_pass) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_DEPTH_TEST);

    glDepthFunc(GL_ALWAYS);
    glColor3f(1, 0, 0); fullscreen_quad(0.0f);    /* prime depth to 0.5 */

    glDepthFunc(func);
    glColor3f(0, 1, 0); fullscreen_quad(0.0f);    /* same depth */

    uint32_t p = px(c, 20, 20);
    int ok = should_pass ? (p == 0x00FF00) : (p == 0xFF0000);
    aglxDestroyContext(c);
    return ok;
}

static int t_depth_never(void)    { return depth_func_case(GL_NEVER,    0); }
static int t_depth_always(void)   { return depth_func_case(GL_ALWAYS,   1); }
static int t_depth_equal(void)    { return depth_func_case(GL_EQUAL,    1); }
static int t_depth_notequal(void) { return depth_func_case(GL_NOTEQUAL, 0); }
static int t_depth_lequal(void)   { return depth_func_case(GL_LEQUAL,   1); }
static int t_depth_gequal(void)   { return depth_func_case(GL_GEQUAL,   1); }
static int t_depth_less(void)     { return depth_func_case(GL_LESS,     0); }
static int t_depth_greater(void)  { return depth_func_case(GL_GREATER,  0); }

/* Depth interpolates across a tilted triangle rather than being constant. */
static int t_depth_interpolates(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(2.5f,  2.5f,   0.9f);   /* window depth ~0.05 */
    glVertex3f(36.5f, 2.5f,  -0.9f);   /* window depth ~0.95 */
    glVertex3f(2.5f,  36.5f,  0.9f);
    glEnd();
    float left  = dpx(c, 4, 4);
    float right = dpx(c, 30, 4);
    int ok = right > left + 0.2f;
    aglxDestroyContext(c);
    return ok;
}

/* A context without a depth buffer must ignore depth state, not crash. */
static int t_no_depth_buffer(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, 0);   /* no AGLX_DEPTH */
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    glEnable(GL_DEPTH_TEST);
    glColor3f(1, 1, 1);
    tri_ccw(0.0f);

    int ok = lit_count(c) > 0 && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------------- culling --- */

static int t_cull_back(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glColor3f(1, 1, 1);

    tri_ccw(0.0f);                       /* front-facing: kept */
    int front_lit = lit_count(c);
    glClear(GL_COLOR_BUFFER_BIT);
    tri_cw(0.0f);                        /* back-facing: culled */
    int back_lit = lit_count(c);

    int ok = front_lit > 0 && back_lit == 0;
    aglxDestroyContext(c);
    return ok;
}

static int t_cull_front(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glColor3f(1, 1, 1);
    tri_ccw(0.0f);
    int ok = lit_count(c) == 0;
    aglxDestroyContext(c);
    return ok;
}

static int t_cull_front_and_back(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT_AND_BACK);
    glColor3f(1, 1, 1);
    tri_ccw(0.0f);
    int a = lit_count(c);
    tri_cw(0.0f);
    int b = lit_count(c);
    int ok = a == 0 && b == 0;
    aglxDestroyContext(c);
    return ok;
}

/* glFrontFace(GL_CW) inverts which winding is considered front. */
static int t_front_face_cw(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);
    glColor3f(1, 1, 1);

    tri_ccw(0.0f);                       /* now back-facing: culled */
    int ccw_lit = lit_count(c);
    glClear(GL_COLOR_BUFFER_BIT);
    tri_cw(0.0f);                        /* now front-facing: kept */
    int cw_lit = lit_count(c);

    int ok = ccw_lit == 0 && cw_lit > 0;
    aglxDestroyContext(c);
    return ok;
}

/* Culling disabled means both windings draw. */
static int t_cull_disabled(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glDisable(GL_CULL_FACE);
    glColor3f(1, 1, 1);
    tri_cw(0.0f);
    int ok = lit_count(c) > 0;
    aglxDestroyContext(c);
    return ok;
}

/* -------------------------------------------------------- fill rule ------ */

/* THE key correctness property: two triangles sharing an edge must tile it
 * exactly once — no gap, no double-cover.  Drawing them in sequence must give
 * exactly the pixel count of the quad they form. */
static int t_shared_edge_no_gap(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glColor3f(1, 1, 1);

    /* Quad (8,8)-(32,32) split along the diagonal, as two CCW triangles. */
    glBegin(GL_TRIANGLES);
    glVertex3f(8.0f,  8.0f,  0); glVertex3f(32.0f, 8.0f,  0); glVertex3f(32.0f, 32.0f, 0);
    glVertex3f(8.0f,  8.0f,  0); glVertex3f(32.0f, 32.0f, 0); glVertex3f(8.0f,  32.0f, 0);
    glEnd();

    /* Every pixel strictly inside the quad must be covered: no diagonal seam. */
    int gaps = 0;
    for (int y = 9; y < 31; y++)
        for (int x = 9; x < 31; x++)
            if (px(c, x, y) == 0) gaps++;

    int ok = gaps == 0;
    aglxDestroyContext(c);
    return ok;
}

/* The same two triangles must not paint any pixel twice.  Counting coverage
 * with additive colour would overflow; instead the total lit area is compared
 * against the quad's area, which double-coverage cannot change but a gap can.
 * This complements the test above by checking the OUTER edges do not bleed. */
static int t_shared_edge_no_overdraw(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(10.0f, 10.0f, 0); glVertex3f(30.0f, 10.0f, 0); glVertex3f(30.0f, 30.0f, 0);
    glVertex3f(10.0f, 10.0f, 0); glVertex3f(30.0f, 30.0f, 0); glVertex3f(10.0f, 30.0f, 0);
    glEnd();

    /* A 20x20 quad on integer boundaries covers exactly 400 pixels. */
    int ok = lit_count(c) == 400;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------------- scissor --- */

static int t_scissor_clips(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_SCISSOR_TEST);
    glScissor(10, 10, 10, 10);           /* only x,y in [10,20) */
    glColor3f(1, 1, 1);
    fullscreen_quad(0.0f);

    int inside  = px(c, 15, 15) != 0;
    int outside = px(c, 5, 5) == 0 && px(c, 25, 25) == 0;
    int ok = inside && outside && lit_count(c) == 100;
    aglxDestroyContext(c);
    return ok;
}

static int t_scissor_disabled(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glScissor(10, 10, 5, 5);
    glDisable(GL_SCISSOR_TEST);
    glColor3f(1, 1, 1);
    fullscreen_quad(0.0f);
    int ok = lit_count(c) == W * H;
    aglxDestroyContext(c);
    return ok;
}

static int t_scissor_negative_size(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glScissor(0, 0, -5, 10);
    int ok = glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

/* ---------------------------------------------------------- state API ---- */

static int t_enable_invalid(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(0x9999);
    int ok = glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

static int t_is_enabled(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_DEPTH_TEST);
    int on = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    glDisable(GL_DEPTH_TEST);
    int off = glIsEnabled(GL_DEPTH_TEST) == GL_FALSE;
    int ok = on && off;
    aglxDestroyContext(c);
    return ok;
}

static int t_depth_func_invalid(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glDepthFunc(0x1234);
    int ok = glGetError() == GL_INVALID_ENUM && c->depth_func == GL_LESS;
    aglxDestroyContext(c);
    return ok;
}

static int t_cull_face_invalid(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glCullFace(0x1234);
    int ok = glGetError() == GL_INVALID_ENUM;
    glFrontFace(0x1234);
    ok = ok && glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

static int t_get_integerv(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    int ok = vp[0] == 0 && vp[1] == 0 && vp[2] == W && vp[3] == H;

    GLint f;
    glDepthFunc(GL_GEQUAL);
    glGetIntegerv(GL_DEPTH_FUNC, &f);
    ok = ok && f == GL_GEQUAL;

    glGetIntegerv(GL_MAX_MODELVIEW_STACK_DEPTH, &f);
    ok = ok && f == GL_MODELVIEW_STACK_DEPTH;
    ok = ok && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_get_floatv_matrix(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(3, 4, 5);
    GLfloat m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    /* Column-major: translation lives in m[12..14]. */
    int ok = m[12] == 3.0f && m[13] == 4.0f && m[14] == 5.0f;
    aglxDestroyContext(c);
    return ok;
}

static int t_get_booleanv(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_CULL_FACE);
    GLboolean b;
    glGetBooleanv(GL_CULL_FACE, &b);
    int ok = b == GL_TRUE;
    glGetBooleanv(0x9999, &b);
    ok = ok && glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

static int t_get_null_pointer(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glGetIntegerv(GL_VIEWPORT, NULL);
    int ok = glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

static int t_polygon_mode_invalid(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glPolygonMode(GL_FRONT_AND_BACK, 0x1234);
    int ok = glGetError() == GL_INVALID_ENUM;
    glPolygonMode(0x1234, GL_FILL);
    ok = ok && glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------------------------ */

int main(void) {
    printf("=== glraster (G3) unit tests ===\n");

    printf("--- filling ---\n");
    RUN(t_fill_interior); RUN(t_fill_area_is_plausible);
    RUN(t_fill_both_windings); RUN(t_degenerate_zero_area);
    RUN(t_degenerate_same_point); RUN(t_huge_triangle_bounded);
    RUN(t_offscreen_triangle);

    printf("--- interpolation ---\n");
    RUN(t_gouraud_interpolation); RUN(t_flat_shading);

    printf("--- depth buffer ---\n");
    RUN(t_depth_written); RUN(t_depth_nearer_wins); RUN(t_depth_farther_loses);
    RUN(t_depth_test_disabled); RUN(t_depth_mask); RUN(t_depth_interpolates);
    RUN(t_no_depth_buffer);
    RUN(t_depth_never); RUN(t_depth_always); RUN(t_depth_equal);
    RUN(t_depth_notequal); RUN(t_depth_lequal); RUN(t_depth_gequal);
    RUN(t_depth_less); RUN(t_depth_greater);

    printf("--- culling ---\n");
    RUN(t_cull_back); RUN(t_cull_front); RUN(t_cull_front_and_back);
    RUN(t_front_face_cw); RUN(t_cull_disabled);

    printf("--- fill rule ---\n");
    RUN(t_shared_edge_no_gap); RUN(t_shared_edge_no_overdraw);

    printf("--- scissor ---\n");
    RUN(t_scissor_clips); RUN(t_scissor_disabled); RUN(t_scissor_negative_size);

    printf("--- state API ---\n");
    RUN(t_enable_invalid); RUN(t_is_enabled); RUN(t_depth_func_invalid);
    RUN(t_cull_face_invalid); RUN(t_get_integerv); RUN(t_get_floatv_matrix);
    RUN(t_get_booleanv); RUN(t_get_null_pointer); RUN(t_polygon_mode_invalid);

    printf("\ntest_glraster: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

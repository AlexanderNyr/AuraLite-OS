/*
 * test_glimm.c — host-side unit tests for matrix stacks and immediate mode.
 *
 * Links the REAL glmatrix.c / glimm.c / glraster.c, so what is tested is what
 * ships.  Geometry is verified by inspecting the rendered colour buffer rather
 * than by trusting intermediate values: a transform bug that cancels itself
 * out in the maths but puts pixels in the wrong place still fails here.
 *
 * An orthographic projection over the exact pixel grid is used for most tests
 * (see setup_pixel_ortho), which makes the mapping from GL coordinates to
 * pixels 1:1 and lets assertions name specific pixels.
 */

#include <stdio.h>
#include <string.h>

#include "GL/gl.h"
#include "GL/auraglx.h"
#include "glcontext.h"
#include "glvertex.h"
#include "auragui.h"

void gl_imm_reset(void);
glm_mat4 *gl_current_matrix(struct aglx_context *ctx);

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    ag_stub_reset();                                    \
    gl_imm_reset();                                     \
    aglxMakeCurrent(NULL);                              \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

#define W 32
#define H 32
#define EPS 1e-4f

static int feq(float a, float b) {
    float d = a - b; if (d < 0) d = -d; return d <= EPS;
}

/* Read a pixel in GL window coordinates (origin bottom-left). */
static uint32_t px(aglx_context_t *c, int x, int y) {
    const uint32_t *buf = aglxGetColorBuffer(c);
    return buf[(size_t)(aglxGetHeight(c) - 1 - y) * aglxGetWidth(c) + x];
}

/* Count non-black pixels — a cheap way to assert "something was drawn" or
 * "nothing was drawn" without depending on exact coverage. */
static int lit_count(aglx_context_t *c) {
    const uint32_t *buf = aglxGetColorBuffer(c);
    int n = 0, total = aglxGetWidth(c) * aglxGetHeight(c);
    for (int i = 0; i < total; i++) if (buf[i] != 0) n++;
    return n;
}

/* Set up a context whose GL coordinates map 1:1 onto pixel centres.
 *
 * glOrtho(0,W,0,H,...) maps x=0..W across the viewport, so GL coordinate
 * (i+0.5, j+0.5) lands exactly on the centre of pixel (i,j).  Tests therefore
 * draw at +0.5 offsets to hit pixels unambiguously. */
static aglx_context_t *setup_pixel_ortho(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return NULL;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    return c;
}

/* ======================================================== matrix stacks === */

static int t_matrix_mode_default(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    int ok = c->matrix_mode == GL_MODELVIEW;
    aglxDestroyContext(c);
    return ok;
}

static int t_matrix_mode_invalid(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glMatrixMode(0x1234);
    int ok = glGetError() == GL_INVALID_ENUM && c->matrix_mode == GL_MODELVIEW;
    aglxDestroyContext(c);
    return ok;
}

/* Stacks must start at identity. */
static int t_stack_starts_identity(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glm_mat4 id = glm_mat4_identity();
    glm_mat4 *cur = gl_current_matrix(c);
    int ok = 1;
    for (int i = 0; i < 16; i++) if (!feq(cur->m[i], id.m[i])) ok = 0;
    aglxDestroyContext(c);
    return ok;
}

/* Push/pop must restore the matrix exactly. */
static int t_push_pop_restores(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glLoadIdentity();
    glTranslatef(1, 2, 3);
    glm_mat4 before = *gl_current_matrix(c);

    glPushMatrix();
    glRotatef(37, 0, 1, 0);
    glScalef(2, 2, 2);
    glPopMatrix();

    glm_mat4 after = *gl_current_matrix(c);
    int ok = 1;
    for (int i = 0; i < 16; i++) if (!feq(before.m[i], after.m[i])) ok = 0;
    ok = ok && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_stack_overflow(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    /* One more push than the stack can hold. */
    for (int i = 0; i < GL_MODELVIEW_STACK_DEPTH; i++) glPushMatrix();
    int ok = glGetError() == GL_STACK_OVERFLOW;
    aglxDestroyContext(c);
    return ok;
}

static int t_stack_underflow(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glPopMatrix();            /* nothing pushed yet */
    int ok = glGetError() == GL_STACK_UNDERFLOW;
    aglxDestroyContext(c);
    return ok;
}

/* An overflowing push must not corrupt the current matrix. */
static int t_overflow_preserves_current(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    for (int i = 0; i < GL_MODELVIEW_STACK_DEPTH - 1; i++) glPushMatrix();
    glLoadIdentity();
    glTranslatef(5, 6, 7);
    glm_mat4 before = *gl_current_matrix(c);
    glPushMatrix();                       /* overflows */
    glm_mat4 after = *gl_current_matrix(c);
    int ok = glGetError() == GL_STACK_OVERFLOW;
    for (int i = 0; i < 16; i++) if (!feq(before.m[i], after.m[i])) ok = 0;
    aglxDestroyContext(c);
    return ok;
}

/* MODELVIEW and PROJECTION stacks are independent. */
static int t_stacks_independent(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(9, 0, 0);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();          /* must not touch MODELVIEW */

    glMatrixMode(GL_MODELVIEW);
    int ok = feq(gl_current_matrix(c)->m[12], 9.0f);
    aglxDestroyContext(c);
    return ok;
}

/* glLoadMatrixf takes column-major data, matching glm_mat4 exactly. */
static int t_load_matrix(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    GLfloat m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 4,5,6,1};  /* translate(4,5,6) */
    glLoadMatrixf(m);
    glm_mat4 *cur = gl_current_matrix(c);
    int ok = feq(cur->m[12], 4) && feq(cur->m[13], 5) && feq(cur->m[14], 6);
    aglxDestroyContext(c);
    return ok;
}

static int t_load_matrix_null(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glLoadMatrixf(NULL);
    int ok = glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

/* Transforms post-multiply, so the LAST call is applied FIRST to a vertex.
 * translate(10,0,0) then scale(2) must map (1,0,0) to (12,0,0), not (22,0,0). */
static int t_transform_order(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glLoadIdentity();
    glTranslatef(10, 0, 0);
    glScalef(2, 2, 2);
    glm_vec3 p = glm_mat4_transform_point(*gl_current_matrix(c),
                                          glm_vec3_make(1, 0, 0));
    int ok = feq(p.x, 12.0f);
    aglxDestroyContext(c);
    return ok;
}

/* glRotatef takes degrees. */
static int t_rotatef_degrees(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glLoadIdentity();
    glRotatef(90.0f, 0, 0, 1);
    glm_vec3 p = glm_mat4_transform_point(*gl_current_matrix(c),
                                          glm_vec3_make(1, 0, 0));
    int ok = feq(p.x, 0.0f) && feq(p.y, 1.0f);
    aglxDestroyContext(c);
    return ok;
}

/* glFrustum rejects non-positive near/far. */
static int t_frustum_validation(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glFrustum(-1, 1, -1, 1, 0, 100);      /* near == 0 */
    int e1 = glGetError() == GL_INVALID_VALUE;
    glFrustum(-1, 1, -1, 1, -1, 100);     /* near < 0 */
    int e2 = glGetError() == GL_INVALID_VALUE;
    glFrustum(-1, -1, -1, 1, 1, 100);     /* left == right */
    int e3 = glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return e1 && e2 && e3;
}

static int t_ortho_validation(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glOrtho(0, 0, -1, 1, -1, 1);          /* degenerate width */
    int ok = glGetError() == GL_INVALID_VALUE;
    /* Negative near/far IS legal for ortho. */
    glOrtho(-1, 1, -1, 1, -10, 10);
    ok = ok && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* ====================================================== immediate mode === */

static int t_begin_end_basic(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glBegin(GL_POINTS);
    glEnd();
    int ok = glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_begin_invalid_mode(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glBegin(0x9999);
    int ok = glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

static int t_nested_begin(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glBegin(GL_POINTS);
    glBegin(GL_LINES);                     /* illegal */
    int ok = glGetError() == GL_INVALID_OPERATION;
    glEnd();
    aglxDestroyContext(c);
    return ok;
}

static int t_end_without_begin(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glEnd();
    int ok = glGetError() == GL_INVALID_OPERATION;
    aglxDestroyContext(c);
    return ok;
}

/* glVertex outside glBegin/glEnd must be an error, not a crash. */
static int t_vertex_outside_begin(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glVertex3f(1, 1, 0);
    int ok = glGetError() == GL_INVALID_OPERATION && lit_count(c) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* A point must land on exactly the pixel its coordinates name. */
static int t_point_lands_on_pixel(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_POINTS);
    glVertex3f(10.5f, 20.5f, 0.0f);
    glEnd();
    int ok = px(c, 10, 20) == 0xFFFFFF && lit_count(c) == 1;
    aglxDestroyContext(c);
    return ok;
}

/* Several points in one batch. */
static int t_multiple_points(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_POINTS);
    glVertex3f(1.5f, 1.5f, 0);
    glVertex3f(5.5f, 5.5f, 0);
    glVertex3f(9.5f, 9.5f, 0);
    glEnd();
    int ok = px(c, 1, 1) && px(c, 5, 5) && px(c, 9, 9) && lit_count(c) == 3;
    aglxDestroyContext(c);
    return ok;
}

/* Colour is latched per vertex, so two points in one batch can differ. */
static int t_per_vertex_color(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glBegin(GL_POINTS);
    glColor3f(1, 0, 0); glVertex3f(2.5f, 2.5f, 0);
    glColor3f(0, 1, 0); glVertex3f(4.5f, 4.5f, 0);
    glEnd();
    int ok = px(c, 2, 2) == 0xFF0000 && px(c, 4, 4) == 0x00FF00;
    aglxDestroyContext(c);
    return ok;
}

/* glColor3ub maps [0,255] onto [0,1]. */
static int t_color_ub(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3ub(255, 128, 0);
    glBegin(GL_POINTS);
    glVertex3f(3.5f, 3.5f, 0);
    glEnd();
    uint32_t p = px(c, 3, 3);
    int ok = ((p >> 16) & 0xFF) == 255 && ((p >> 8) & 0xFF) == 128
          && (p & 0xFF) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* A horizontal line must light every pixel between its endpoints. */
static int t_line_horizontal(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_LINES);
    glVertex3f(2.5f, 8.5f, 0);
    glVertex3f(12.5f, 8.5f, 0);
    glEnd();
    int ok = 1;
    for (int x = 2; x <= 12; x++) if (!px(c, x, 8)) ok = 0;
    /* Nothing outside the segment. */
    if (px(c, 1, 8) || px(c, 13, 8)) ok = 0;
    aglxDestroyContext(c);
    return ok;
}

static int t_line_vertical(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_LINES);
    glVertex3f(6.5f, 3.5f, 0);
    glVertex3f(6.5f, 13.5f, 0);
    glEnd();
    int ok = 1;
    for (int y = 3; y <= 13; y++) if (!px(c, 6, y)) ok = 0;
    aglxDestroyContext(c);
    return ok;
}

/* A 45-degree diagonal must hit exactly the diagonal pixels. */
static int t_line_diagonal(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_LINES);
    glVertex3f(2.5f, 2.5f, 0);
    glVertex3f(10.5f, 10.5f, 0);
    glEnd();
    int ok = 1;
    for (int i = 2; i <= 10; i++) if (!px(c, i, i)) ok = 0;
    aglxDestroyContext(c);
    return ok;
}

/* GL_LINES draws independent segments: 4 vertices give 2 disjoint lines. */
static int t_lines_independent(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_LINES);
    glVertex3f(1.5f, 1.5f, 0); glVertex3f(4.5f, 1.5f, 0);
    glVertex3f(1.5f, 6.5f, 0); glVertex3f(4.5f, 6.5f, 0);
    glEnd();
    /* Both segments present, and no segment joining them. */
    int ok = px(c, 2, 1) && px(c, 2, 6) && !px(c, 1, 3) && !px(c, 4, 3);
    aglxDestroyContext(c);
    return ok;
}

/* An odd trailing vertex in GL_LINES is discarded (§2.6.1). */
static int t_lines_odd_vertex_ignored(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_LINES);
    glVertex3f(1.5f, 1.5f, 0); glVertex3f(4.5f, 1.5f, 0);
    glVertex3f(1.5f, 9.5f, 0);            /* unpaired */
    glEnd();
    int ok = px(c, 2, 1) && !px(c, 1, 9);
    aglxDestroyContext(c);
    return ok;
}

/* GL_LINE_STRIP connects consecutive vertices. */
static int t_line_strip(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_LINE_STRIP);
    glVertex3f(2.5f, 2.5f, 0);
    glVertex3f(8.5f, 2.5f, 0);
    glVertex3f(8.5f, 8.5f, 0);
    glEnd();
    int ok = px(c, 5, 2) && px(c, 8, 5) && !px(c, 5, 8);  /* not closed */
    aglxDestroyContext(c);
    return ok;
}

/* GL_LINE_LOOP additionally closes back to the first vertex. */
static int t_line_loop_closes(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_LINE_LOOP);
    glVertex3f(2.5f, 2.5f, 0);
    glVertex3f(10.5f, 2.5f, 0);
    glVertex3f(10.5f, 10.5f, 0);
    glVertex3f(2.5f, 10.5f, 0);
    glEnd();
    /* All four edges present, including the closing one on the left. */
    int ok = px(c, 6, 2) && px(c, 10, 6) && px(c, 6, 10) && px(c, 2, 6);
    aglxDestroyContext(c);
    return ok;
}

/* From G3 a triangle is FILLED, so the interior is covered too. */
static int t_triangle_filled(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(2.5f, 2.5f, 0);
    glVertex3f(20.5f, 2.5f, 0);
    glVertex3f(2.5f, 20.5f, 0);
    glEnd();
    /* Interior covered, and a point outside the hypotenuse is not. */
    int ok = px(c, 6, 6) != 0 && px(c, 5, 3) != 0 && px(c, 18, 18) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* glPolygonMode(GL_LINE) restores the wireframe behaviour. */
static int t_polygon_mode_line(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(2.5f, 2.5f, 0);
    glVertex3f(20.5f, 2.5f, 0);
    glVertex3f(2.5f, 20.5f, 0);
    glEnd();
    /* Edges drawn, interior hollow. */
    int ok = px(c, 10, 2) != 0 && px(c, 2, 10) != 0 && px(c, 6, 6) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* GL_TRIANGLE_STRIP: 4 vertices produce 2 triangles. */
static int t_triangle_strip(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLE_STRIP);
    glVertex3f(2.5f,  2.5f, 0);
    glVertex3f(2.5f, 14.5f, 0);
    glVertex3f(14.5f, 2.5f, 0);
    glVertex3f(14.5f,14.5f, 0);
    glEnd();
    /* Two filled triangles cover the whole quad region. */
    int ok = px(c, 5, 5) && px(c, 11, 11) && px(c, 8, 8);
    aglxDestroyContext(c);
    return ok;
}

/* GL_TRIANGLE_FAN: all triangles share the first vertex. */
static int t_triangle_fan(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(8.5f, 8.5f, 0);            /* hub */
    glVertex3f(2.5f, 2.5f, 0);
    glVertex3f(14.5f, 2.5f, 0);
    glVertex3f(14.5f, 14.5f, 0);
    glEnd();
    int ok = px(c, 8, 5) && lit_count(c) > 40;
    aglxDestroyContext(c);
    return ok;
}

/* GL_QUADS: 4 vertices make one quad; its four edges must appear. */
static int t_quads(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_QUADS);
    glVertex3f(4.5f,  4.5f, 0);
    glVertex3f(16.5f, 4.5f, 0);
    glVertex3f(16.5f,16.5f, 0);
    glVertex3f(4.5f, 16.5f, 0);
    glEnd();
    /* Filled: interior and near-corners covered, outside not. */
    int ok = px(c, 10, 10) && px(c, 5, 5) && px(c, 15, 15) && !px(c, 2, 2);
    aglxDestroyContext(c);
    return ok;
}

/* An incomplete quad (3 vertices) must draw nothing. */
static int t_quads_incomplete(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_QUADS);
    glVertex3f(4.5f, 4.5f, 0);
    glVertex3f(16.5f, 4.5f, 0);
    glVertex3f(16.5f, 16.5f, 0);
    glEnd();
    int ok = lit_count(c) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* GL_POLYGON draws a closed convex outline. */
static int t_polygon(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_POLYGON);
    glVertex3f(4.5f, 4.5f, 0);
    glVertex3f(16.5f, 4.5f, 0);
    glVertex3f(16.5f, 16.5f, 0);
    glVertex3f(4.5f, 16.5f, 0);
    glEnd();
    int ok = px(c, 10, 10) && px(c, 6, 6);
    aglxDestroyContext(c);
    return ok;
}

/* A long strip must not overflow anything: memory use is constant. */
static int t_long_strip_is_bounded(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLE_STRIP);
    for (int i = 0; i < 2000; i++) {
        glVertex3f((GLfloat)((i % 20) + 1) + 0.5f,
                   (GLfloat)((i % 15) + 1) + 0.5f, 0.0f);
    }
    glEnd();
    int ok = glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* ==================================================== transform pipeline === */

/* The MODELVIEW matrix must move geometry. */
static int t_modelview_translates(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glTranslatef(5, 5, 0);
    glBegin(GL_POINTS);
    glVertex3f(2.5f, 2.5f, 0);            /* -> (7.5, 7.5) */
    glEnd();
    int ok = px(c, 7, 7) != 0 && px(c, 2, 2) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* Changing the matrix mid-batch affects only later vertices: the transform
 * runs when the vertex is specified. */
static int t_matrix_change_midbatch(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_POINTS);
    glVertex3f(3.5f, 3.5f, 0);
    glEnd();
    glTranslatef(10, 0, 0);
    glBegin(GL_POINTS);
    glVertex3f(3.5f, 3.5f, 0);            /* -> (13.5, 3.5) */
    glEnd();
    int ok = px(c, 3, 3) && px(c, 13, 3);
    aglxDestroyContext(c);
    return ok;
}

/* The viewport transform must honour a reduced viewport. */
static int t_viewport_maps_geometry(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    /* Viewport covering the left half only. */
    glViewport(0, 0, W / 2, H);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-1, 1, -1, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    glColor3f(1, 1, 1);
    glBegin(GL_POINTS);
    glVertex3f(0.0f, 0.0f, 0.0f);        /* centre of the viewport */
    glEnd();

    /* NDC 0 maps to the middle of the half-width viewport, i.e. x = W/4. */
    int ok = px(c, W / 4, H / 2) != 0;
    aglxDestroyContext(c);
    return ok;
}

/* A vertex behind the eye must be dropped, not projected to a wild location. */
static int t_vertex_behind_eye_dropped(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1, 100);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    glColor3f(1, 1, 1);
    glBegin(GL_POINTS);
    glVertex3f(0, 0, 5.0f);              /* behind the eye: w becomes <= 0 */
    glEnd();

    int ok = lit_count(c) == 0 && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Perspective must make a distant object smaller than a near one. */
static int t_perspective_foreshortening(void) {
    aglx_context_t *c = aglxCreateContext(1, 64, 64, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1, 100);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glColor3f(1, 1, 1);

    /* Same object at two depths; measure the horizontal span it covers. */
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_LINES);
    glVertex3f(-1, 0, -2); glVertex3f(1, 0, -2);
    glEnd();
    int near_span = lit_count(c);

    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_LINES);
    glVertex3f(-1, 0, -20); glVertex3f(1, 0, -20);
    glEnd();
    int far_span = lit_count(c);

    int ok = near_span > far_span && far_span > 0;
    aglxDestroyContext(c);
    return ok;
}

/* Window coordinates use a bottom-left origin: +y in GL must move UP the
 * screen, i.e. towards lower framebuffer rows. */
static int t_y_axis_points_up(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_POINTS);
    glVertex3f(5.5f, 1.5f, 0);           /* near the BOTTOM in GL terms */
    glEnd();

    const uint32_t *buf = aglxGetColorBuffer(c);
    /* Must appear near the LAST framebuffer row, not the first. */
    int ok = buf[(size_t)(H - 2) * W + 5] != 0 && buf[(size_t)1 * W + 5] == 0;
    aglxDestroyContext(c);
    return ok;
}

/* Geometry outside the framebuffer must be clipped, not wrap around. */
static int t_offscreen_is_clipped(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_LINES);
    glVertex3f(-500.0f, 5.5f, 0);
    glVertex3f(500.0f, 5.5f, 0);
    glEnd();
    /* The whole row is covered, but nothing else. */
    int ok = 1;
    for (int x = 0; x < W; x++) if (!px(c, x, 5)) ok = 0;
    if (lit_count(c) != W) ok = 0;
    aglxDestroyContext(c);
    return ok;
}

/* REGRESSION (found in G2 QEMU testing): geometry near the eye plane projects
 * to window coordinates in the millions.  Bresenham walks one pixel per step,
 * so without clipping first this took millions of iterations per line and
 * looked exactly like a hang.  The line must now be clipped to the framebuffer
 * before rasterisation, making the cost bounded by the buffer size. */
static int t_huge_coordinates_are_bounded(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    /* Coordinates far outside any sane range, in both directions. */
    glBegin(GL_LINES);
    glVertex3f(-3.0e6f, 16.5f, 0.0f);
    glVertex3f( 3.0e6f, 16.5f, 0.0f);
    glEnd();
    /* Must complete promptly and cover exactly the on-screen span. */
    int ok = 1;
    for (int x = 0; x < W; x++) if (!px(c, x, 16)) ok = 0;
    if (lit_count(c) != W) ok = 0;
    aglxDestroyContext(c);
    return ok;
}

/* A line entirely off-screen must be discarded outright. */
static int t_fully_offscreen_line(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glColor3f(1, 1, 1);
    glBegin(GL_LINES);
    glVertex3f(-1000.0f, -1000.0f, 0.0f);
    glVertex3f(-900.0f,  -900.0f,  0.0f);
    glEnd();
    int ok = lit_count(c) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* Clipping must preserve the colour gradient: the visible part of a clipped
 * line must be shaded as though the whole line had been drawn. */
static int t_clipped_line_keeps_gradient(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glShadeModel(GL_SMOOTH);
    /* Red at far left (off-screen) to blue at far right (off-screen). */
    glBegin(GL_LINES);
    glColor3f(1, 0, 0); glVertex3f(-32.5f, 24.5f, 0.0f);
    glColor3f(0, 0, 1); glVertex3f(64.5f,  24.5f, 0.0f);
    glEnd();
    uint32_t left  = px(c, 0, 24);
    uint32_t right = px(c, W - 1, 24);
    /* Both ends are mid-gradient, so neither is pure red or pure blue. */
    int ok = left != 0 && right != 0
          && ((left >> 16) & 0xFF) > ((right >> 16) & 0xFF)
          && (left & 0xFF) < (right & 0xFF)
          && ((left >> 16) & 0xFF) < 255;
    aglxDestroyContext(c);
    return ok;
}

/* Drawing with no current context must not crash. */
static int t_draw_without_context(void) {
    aglxMakeCurrent(NULL);
    glBegin(GL_POINTS);
    glVertex3f(1, 1, 0);
    glEnd();
    return glGetError() == GL_INVALID_OPERATION;
}

/* glShadeModel validation. */
static int t_shade_model(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glShadeModel(GL_FLAT);
    int ok = c->shade_model == GL_FLAT && glGetError() == GL_NO_ERROR;
    glShadeModel(0x1234);
    ok = ok && glGetError() == GL_INVALID_ENUM && c->shade_model == GL_FLAT;
    aglxDestroyContext(c);
    return ok;
}

/* Smooth shading must interpolate colour along a line. */
static int t_smooth_line_interpolates(void) {
    aglx_context_t *c = setup_pixel_ortho();
    if (!c) return 0;
    glShadeModel(GL_SMOOTH);
    glBegin(GL_LINES);
    glColor3f(1, 0, 0); glVertex3f(2.5f, 5.5f, 0);
    glColor3f(0, 0, 1); glVertex3f(18.5f, 5.5f, 0);
    glEnd();
    uint32_t left  = px(c, 3, 5);
    uint32_t mid   = px(c, 10, 5);
    uint32_t right = px(c, 18, 5);
    /* Red must fall and blue must rise across the span. */
    int ok = ((left >> 16) & 0xFF) > ((mid >> 16) & 0xFF)
          && ((mid >> 16) & 0xFF) > ((right >> 16) & 0xFF)
          && (left & 0xFF) < (mid & 0xFF)
          && (mid & 0xFF) < (right & 0xFF);
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------------------------ */

int main(void) {
    printf("=== glmatrix / glimm unit tests ===\n");

    printf("--- matrix stacks ---\n");
    RUN(t_matrix_mode_default); RUN(t_matrix_mode_invalid);
    RUN(t_stack_starts_identity); RUN(t_push_pop_restores);
    RUN(t_stack_overflow); RUN(t_stack_underflow);
    RUN(t_overflow_preserves_current); RUN(t_stacks_independent);
    RUN(t_load_matrix); RUN(t_load_matrix_null);
    RUN(t_transform_order); RUN(t_rotatef_degrees);
    RUN(t_frustum_validation); RUN(t_ortho_validation);

    printf("--- immediate mode ---\n");
    RUN(t_begin_end_basic); RUN(t_begin_invalid_mode); RUN(t_nested_begin);
    RUN(t_end_without_begin); RUN(t_vertex_outside_begin);
    RUN(t_point_lands_on_pixel); RUN(t_multiple_points);
    RUN(t_per_vertex_color); RUN(t_color_ub);

    printf("--- primitives ---\n");
    RUN(t_line_horizontal); RUN(t_line_vertical); RUN(t_line_diagonal);
    RUN(t_lines_independent); RUN(t_lines_odd_vertex_ignored);
    RUN(t_line_strip); RUN(t_line_loop_closes);
    RUN(t_triangle_filled); RUN(t_polygon_mode_line);
    RUN(t_triangle_strip); RUN(t_triangle_fan);
    RUN(t_quads); RUN(t_quads_incomplete); RUN(t_polygon);
    RUN(t_long_strip_is_bounded);

    printf("--- transform pipeline ---\n");
    RUN(t_modelview_translates); RUN(t_matrix_change_midbatch);
    RUN(t_viewport_maps_geometry); RUN(t_vertex_behind_eye_dropped);
    RUN(t_perspective_foreshortening); RUN(t_y_axis_points_up);
    RUN(t_offscreen_is_clipped); RUN(t_huge_coordinates_are_bounded);
    RUN(t_fully_offscreen_line); RUN(t_clipped_line_keeps_gradient);
    RUN(t_draw_without_context);
    RUN(t_shade_model); RUN(t_smooth_line_interpolates);

    printf("\ntest_glimm: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

/*
 * test_glu.c — host-side unit tests for the GLU utility layer (phase G8).
 *
 * GLU is built purely on the public GL API, so these tests check it the same
 * way: by driving the real entry points and reading back either the resulting
 * matrix (via glGetFloatv) or the rendered pixels.
 *
 * The matrix tests compare against the reference matrices in the GLU
 * specification rather than against values captured from a previous run —
 * otherwise the test only proves the code is self-consistent.
 */

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "GL/gl.h"
#include "GL/glu.h"
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
#define EPS 1e-4f

static int feq(float a, float b) {
    float d = a - b; if (d < 0) d = -d; return d <= EPS;
}

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

static aglx_context_t *setup(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    if (!c) return NULL;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glColor3f(1, 1, 1);
    return c;
}

/* ------------------------------------------------------- gluPerspective --- */

/* gluPerspective(fovy, aspect, n, f) must equal the frustum it stands for.
 * fovy is in DEGREES — the most common way to get this wrong. */
static int t_perspective_matches_frustum(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const double fovy = 60.0, aspect = 4.0 / 3.0, n = 1.0, f = 100.0;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(fovy, aspect, n, f);
    GLfloat via_glu[16];
    glGetFloatv(GL_PROJECTION_MATRIX, via_glu);

    double top = n * tan(fovy * 0.5 * (3.14159265358979323846 / 180.0));
    double right = top * aspect;
    glLoadIdentity();
    glFrustum(-right, right, -top, top, n, f);
    GLfloat via_frustum[16];
    glGetFloatv(GL_PROJECTION_MATRIX, via_frustum);

    int ok = 1;
    for (int i = 0; i < 16; i++) if (!feq(via_glu[i], via_frustum[i])) ok = 0;
    aglxDestroyContext(c);
    return ok;
}

/* A 90-degree vertical field of view with aspect 1 gives m[0] == m[5] == 1. */
static int t_perspective_90deg(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(90.0, 1.0, 1.0, 100.0);
    GLfloat m[16];
    glGetFloatv(GL_PROJECTION_MATRIX, m);
    int ok = feq(m[0], 1.0f) && feq(m[5], 1.0f) && feq(m[11], -1.0f);
    aglxDestroyContext(c);
    return ok;
}

/* Degenerate parameters must not produce NaNs or touch the matrix. */
static int t_perspective_degenerate(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, 0.0, 1.0, 100.0);      /* aspect == 0 */
    GLfloat m[16];
    glGetFloatv(GL_PROJECTION_MATRIX, m);
    /* Still the identity. */
    int ok = feq(m[0], 1.0f) && feq(m[5], 1.0f) && feq(m[10], 1.0f)
          && feq(m[15], 1.0f);
    aglxDestroyContext(c);
    return ok;
}

/* --------------------------------------------------------- gluLookAt ------ */

/* An eye on +z looking at the origin must place the origin at -distance. */
static int t_lookat_basic(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(0, 0, 10,  0, 0, 0,  0, 1, 0);
    GLfloat m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    /* Column-major: the translation lands in m[12..14]. */
    int ok = feq(m[12], 0.0f) && feq(m[13], 0.0f) && feq(m[14], -10.0f);
    aglxDestroyContext(c);
    return ok;
}

/* The eye position itself must map to the view-space origin. */
static int t_lookat_eye_at_origin(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(3, 4, 5,  0, 0, 0,  0, 1, 0);
    GLfloat m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);

    /* Transform the eye by the matrix by hand: it must come out at (0,0,0). */
    float ex = 3, ey = 4, ez = 5;
    float tx = m[0]*ex + m[4]*ey + m[8] *ez + m[12];
    float ty = m[1]*ex + m[5]*ey + m[9] *ez + m[13];
    float tz = m[2]*ex + m[6]*ey + m[10]*ez + m[14];
    int ok = feq(tx, 0.0f) && feq(ty, 0.0f) && feq(tz, 0.0f);
    aglxDestroyContext(c);
    return ok;
}

/* A non-perpendicular up vector must still work: GLU re-derives true up. */
static int t_lookat_sloppy_up(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(0, 0, 10,  0, 0, 0,  0.3f, 1.0f, 0.2f);   /* not perpendicular */
    GLfloat m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);

    /* The three basis rows must still be unit length. */
    float r0 = sqrtf(m[0]*m[0] + m[4]*m[4] + m[8]*m[8]);
    float r1 = sqrtf(m[1]*m[1] + m[5]*m[5] + m[9]*m[9]);
    float r2 = sqrtf(m[2]*m[2] + m[6]*m[6] + m[10]*m[10]);
    int ok = feq(r0, 1.0f) && feq(r1, 1.0f) && feq(r2, 1.0f);
    aglxDestroyContext(c);
    return ok;
}

/* eye == center has no direction and must leave the matrix untouched. */
static int t_lookat_degenerate(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(1, 1, 1,  1, 1, 1,  0, 1, 0);
    GLfloat m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    int ok = feq(m[0], 1.0f) && feq(m[5], 1.0f) && feq(m[14], 0.0f);
    aglxDestroyContext(c);
    return ok;
}

/* An up vector parallel to the view direction is also degenerate. */
static int t_lookat_parallel_up(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(0, 0, 10,  0, 0, 0,  0, 0, 1);   /* up along the view axis */
    GLfloat m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    int ok = feq(m[0], 1.0f) && feq(m[5], 1.0f);   /* unchanged */
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------- gluOrtho2D ------- */

static int t_ortho2d(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(0, 64, 0, 48);
    GLfloat via_glu[16];
    glGetFloatv(GL_PROJECTION_MATRIX, via_glu);

    glLoadIdentity();
    glOrtho(0, 64, 0, 48, -1, 1);
    GLfloat via_ortho[16];
    glGetFloatv(GL_PROJECTION_MATRIX, via_ortho);

    int ok = 1;
    for (int i = 0; i < 16; i++) if (!feq(via_glu[i], via_ortho[i])) ok = 0;
    aglxDestroyContext(c);
    return ok;
}

/* ---------------------------------------------------- gluErrorString ------ */

static int t_error_strings(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const char *no  = (const char *)gluErrorString(GL_NO_ERROR);
    const char *ie  = (const char *)gluErrorString(GL_INVALID_ENUM);
    const char *iv  = (const char *)gluErrorString(GL_INVALID_VALUE);
    const char *io  = (const char *)gluErrorString(GL_INVALID_OPERATION);
    const char *unk = (const char *)gluErrorString(0x9999);
    int ok = no && ie && iv && io && unk
          && strstr(no, "no error") != NULL
          && strstr(ie, "enum") != NULL
          && strstr(iv, "value") != NULL
          && strstr(io, "operation") != NULL
          /* Never NULL, even for a code GLU does not know. */
          && strlen(unk) > 0;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------------ quadrics ---- */

static int t_quadric_lifecycle(void) {
    GLUquadric *q = gluNewQuadric();
    int ok = q != NULL;
    gluQuadricDrawStyle(q, GLU_LINE);
    gluQuadricNormals(q, GLU_FLAT);
    gluQuadricOrientation(q, GLU_INSIDE);
    gluQuadricTexture(q, GL_TRUE);
    gluDeleteQuadric(q);
    gluDeleteQuadric(NULL);          /* must be safe */
    return ok;
}

/* A sphere must cover roughly the area its projected disk should. */
static int t_sphere_draws(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-2, 2, -2, 2, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    GLUquadric *q = gluNewQuadric();
    gluSphere(q, 1.0, 16, 12);
    gluDeleteQuadric(q);

    int n = lit_count(c);
    /* Radius 1 in a 4-unit-wide ortho box across 64 px is a 32 px diameter
     * circle: about pi*16^2 = 804 px.  Wide bounds, since what matters is
     * that it is a filled disk and not a stray triangle or the whole screen. */
    int ok = n > 500 && n < 1200 && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* The centre of a sphere at the origin must be covered. */
static int t_sphere_centre_covered(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-2, 2, -2, 2, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    GLUquadric *q = gluNewQuadric();
    gluSphere(q, 1.0, 16, 12);
    gluDeleteQuadric(q);
    int ok = px(c, W / 2, H / 2) != 0;
    aglxDestroyContext(c);
    return ok;
}

/* GLU_LINE must draw an outline, not a filled body. */
static int t_sphere_line_style(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-2, 2, -2, 2, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    GLUquadric *q = gluNewQuadric();
    gluSphere(q, 1.0, 16, 12);
    int filled = lit_count(c);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gluQuadricDrawStyle(q, GLU_LINE);
    gluSphere(q, 1.0, 16, 12);
    int wire = lit_count(c);
    gluDeleteQuadric(q);

    int ok = filled > 0 && wire > 0 && wire < filled;
    aglxDestroyContext(c);
    return ok;
}

static int t_cylinder_draws(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-2, 2, -2, 2, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glRotatef(90.0f, 1.0f, 0.0f, 0.0f);   /* lay it across the view */

    GLUquadric *q = gluNewQuadric();
    gluCylinder(q, 0.8, 0.8, 2.0, 16, 4);
    gluDeleteQuadric(q);

    int ok = lit_count(c) > 200 && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* A cone is a cylinder with top radius 0; it must not divide by zero. */
static int t_cone_draws(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-2, 2, -2, 2, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glRotatef(90.0f, 1.0f, 0.0f, 0.0f);

    GLUquadric *q = gluNewQuadric();
    gluCylinder(q, 1.0, 0.0, 2.0, 16, 4);
    gluDeleteQuadric(q);

    int ok = lit_count(c) > 100 && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_disk_draws(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-2, 2, -2, 2, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    GLUquadric *q = gluNewQuadric();
    gluDisk(q, 0.0, 1.0, 24, 2);
    gluDeleteQuadric(q);

    int ok = lit_count(c) > 500 && px(c, W / 2, H / 2) != 0;
    aglxDestroyContext(c);
    return ok;
}

/* An annulus must leave its middle empty. */
static int t_disk_with_hole(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-2, 2, -2, 2, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    GLUquadric *q = gluNewQuadric();
    gluDisk(q, 0.7, 1.0, 24, 2);
    gluDeleteQuadric(q);

    int ok = px(c, W / 2, H / 2) == 0        /* hole */
          && lit_count(c) > 100;             /* but the ring is there */
    aglxDestroyContext(c);
    return ok;
}

/* Degenerate parameters must be rejected quietly, not hang or crash. */
static int t_quadric_degenerate(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLUquadric *q = gluNewQuadric();
    gluSphere(q, 1.0, 1, 12);        /* slices < 2  */
    gluSphere(q, 1.0, 16, 1);        /* stacks < 2  */
    gluSphere(q, -1.0, 16, 12);      /* radius <= 0 */
    gluCylinder(q, 1.0, 1.0, 1.0, 1, 1);
    gluDisk(q, 0.0, -1.0, 16, 1);    /* outer <= 0  */
    gluSphere(NULL, 1.0, 16, 12);    /* NULL quadric */
    gluDeleteQuadric(q);
    int ok = lit_count(c) == 0 && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Quadrics must light like any other geometry: they emit real normals. */
static int t_quadric_lighting(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-2, 2, -2, 2, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    /* The depth test is essential here, not incidental: a sphere draws both
     * its near and far hemispheres, and with culling off and no depth buffer
     * the far side (facing away from the light, so ambient-only) is drawn
     * last and covers the lit near side.  Without this the whole disk reads
     * as flat ambient and the test fails against perfectly good normals. */
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

    GLUquadric *q = gluNewQuadric();
    gluSphere(q, 1.0, 24, 18);
    gluDeleteQuadric(q);

    /* The centre faces the light and must be brighter than the rim. */
    uint32_t centre = px(c, W / 2, H / 2);
    uint32_t rim    = px(c, W / 2 + 14, H / 2);
    int cb = (centre >> 16) & 0xFF;
    int rb = (rim >> 16) & 0xFF;
    int ok = cb > rb + 30;
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    aglxDestroyContext(c);
    return ok;
}

/* GLU must work through a display list, since that is how demos use it. */
static int t_quadric_in_display_list(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-2, 2, -2, 2, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    GLUquadric *q = gluNewQuadric();
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    gluSphere(q, 1.0, 16, 12);
    glEndList();
    gluDeleteQuadric(q);

    int drawn_at_compile = lit_count(c);
    glCallList(list);
    int drawn_at_call = lit_count(c);

    int ok = drawn_at_compile == 0 && drawn_at_call > 500;
    glDeleteLists(list, 1);
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------------------------ */

int main(void) {
    printf("=== glu (G8) unit tests ===\n");

    printf("--- gluPerspective ---\n");
    RUN(t_perspective_matches_frustum); RUN(t_perspective_90deg);
    RUN(t_perspective_degenerate);

    printf("--- gluLookAt ---\n");
    RUN(t_lookat_basic); RUN(t_lookat_eye_at_origin);
    RUN(t_lookat_sloppy_up); RUN(t_lookat_degenerate);
    RUN(t_lookat_parallel_up);

    printf("--- gluOrtho2D / errors ---\n");
    RUN(t_ortho2d); RUN(t_error_strings);

    printf("--- quadrics ---\n");
    RUN(t_quadric_lifecycle); RUN(t_sphere_draws);
    RUN(t_sphere_centre_covered); RUN(t_sphere_line_style);
    RUN(t_cylinder_draws); RUN(t_cone_draws);
    RUN(t_disk_draws); RUN(t_disk_with_hole);
    RUN(t_quadric_degenerate); RUN(t_quadric_lighting);
    RUN(t_quadric_in_display_list);

    printf("\ntest_glu: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

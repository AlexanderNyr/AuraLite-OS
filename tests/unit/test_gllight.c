/*
 * test_gllight.c — host-side unit tests for lighting and materials (phase G5).
 *
 * Links the real gllight.c and drives it through the public GL API.  Most
 * assertions compare the lit result against the value the specification's
 * equation predicts for that configuration, rather than against a number
 * copied out of a previous run — otherwise the test only proves the code is
 * self-consistent, not that it is correct.
 */

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "GL/gl.h"
#include "GL/auraglx.h"
#include "glcontext.h"
#include "glvertex.h"
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

#define W 32
#define H 32

static int feq(float a, float b, float eps) {
    float d = a - b; if (d < 0) d = -d; return d <= eps;
}

static uint32_t px(aglx_context_t *c, int x, int y) {
    const uint32_t *b = aglxGetColorBuffer(c);
    return b[(size_t)(H - 1 - y) * W + x];
}

static aglx_context_t *setup(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    if (!c) return NULL;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    /* Centred on the origin: eye space then has the quad centre at (0,0,0),
     * so the direction to the viewer is +z.  An off-centre ortho box would put
     * the vertex metres to one side in eye space and the specular and spot
     * geometry would be measured against a viewer that is not where the test
     * assumes. */
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(-4, 4, -4, 4, 1, 100);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    return c;
}

/* Draw a small quad facing +z and sample its centre.
 *
 * TWO THINGS MATTER HERE, both consequences of GL 1.1 lighting being
 * per-VERTEX rather than per-fragment:
 *
 *  - The quad is SMALL and pushed back to z = -20.  Every vertex must be close
 *    to the view axis, otherwise the direction to the viewer (V = -eye) points
 *    sideways at each corner and the specular term, which depends on N.H,
 *    collapses.  A large quad at z = 0 has all four corners lying in the
 *    viewer's plane, giving N.H ~ 0.707 and 0.707^32 ~ 0 — no highlight, and
 *    correctly so.
 *
 *  - The sample is taken at the centre, where the interpolated value is the
 *    average of the four lit corners.
 */
static uint32_t draw_lit_quad(aglx_context_t *c) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glNormal3f(0.0f, 0.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex3f(-2.0f, -2.0f, -20.0f);
    glVertex3f( 2.0f, -2.0f, -20.0f);
    glVertex3f( 2.0f,  2.0f, -20.0f);
    glVertex3f(-2.0f,  2.0f, -20.0f);
    glEnd();
    return px(c, 16, 16);
}

/* ------------------------------------------------------------- defaults --- */

static int t_lighting_off_by_default(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    int ok = glIsEnabled(GL_LIGHTING) == GL_FALSE;
    aglxDestroyContext(c);
    return ok;
}

/* With lighting off, glColor passes straight through. */
static int t_unlit_uses_vertex_color(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glDisable(GL_LIGHTING);
    glColor3f(1.0f, 0.0f, 0.0f);
    uint32_t p = draw_lit_quad(c);
    int ok = p == 0xFF0000;
    aglxDestroyContext(c);
    return ok;
}

static int t_max_lights_query(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLint n = 0;
    glGetIntegerv(GL_MAX_LIGHTS, &n);
    int ok = n >= 8;
    aglxDestroyContext(c);
    return ok;
}

/* GL_LIGHT0 defaults to white diffuse; the others default to black.  That
 * asymmetry is in the specification and applications rely on it. */
static int t_light0_defaults_white(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    int ok = feq(c->lights[0].diffuse.r, 1.0f, 1e-6f)
          && feq(c->lights[1].diffuse.r, 0.0f, 1e-6f);
    aglxDestroyContext(c);
    return ok;
}

/* Enabling each of the eight lights must work. */
static int t_enable_all_lights(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    int ok = 1;
    for (int i = 0; i < 8; i++) {
        glEnable(GL_LIGHT0 + i);
        if (glIsEnabled(GL_LIGHT0 + i) != GL_TRUE) ok = 0;
    }
    ok = ok && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_light_out_of_range(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLfloat v[4] = { 1, 1, 1, 1 };
    glLightfv(GL_LIGHT0 + 99, GL_DIFFUSE, v);
    int ok = glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------- diffuse term ----- */

/* A directional light along the normal gives the full diffuse term.
 *
 * Expected: emission(0) + scene_ambient(0.2)*mat_ambient(0.2)
 *         + light_ambient(0)*... + N.L(1) * light_diffuse(1) * mat_diffuse
 * With mat_diffuse set to 1.0 that is 0.04 + 1.0, clamped to 1.0 -> 255. */
static int t_diffuse_head_on(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    GLfloat pos[4] = { 0, 0, 1, 0 };        /* directional, towards +z */
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    GLfloat white[4] = { 1, 1, 1, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, white);

    uint32_t p = draw_lit_quad(c);
    int ok = ((p >> 16) & 0xFF) > 250;      /* saturated */
    aglxDestroyContext(c);
    return ok;
}

/* A light at 90 degrees to the normal contributes no diffuse: only the
 * ambient terms remain, so the result must be dark but not black. */
static int t_diffuse_perpendicular(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    GLfloat pos[4] = { 1, 0, 0, 0 };        /* along +x, normal is +z */
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    GLfloat white[4] = { 1, 1, 1, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, white);

    uint32_t p = draw_lit_quad(c);
    int r = (p >> 16) & 0xFF;
    /* scene_ambient 0.2 * mat_ambient 0.2 = 0.04 -> about 10/255. */
    int ok = r > 0 && r < 40;
    aglxDestroyContext(c);
    return ok;
}

/* A light behind the surface contributes nothing beyond ambient. */
static int t_diffuse_behind(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    GLfloat pos[4] = { 0, 0, -1, 0 };       /* shining from behind */
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    GLfloat white[4] = { 1, 1, 1, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, white);

    uint32_t p = draw_lit_quad(c);
    int r = (p >> 16) & 0xFF;
    int ok = r < 40;
    aglxDestroyContext(c);
    return ok;
}

/* Diffuse must scale with cos(angle): a light at 60 degrees gives about half
 * the intensity of one head-on.  This is the property that makes curved
 * surfaces look curved. */
static int t_diffuse_follows_cosine(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    GLfloat white[4] = { 1, 1, 1, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, white);
    /* Half-strength diffuse so the head-on case does not clamp. */
    GLfloat half[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
    glLightfv(GL_LIGHT0, GL_DIFFUSE, half);

    GLfloat straight[4] = { 0, 0, 1, 0 };
    glLightfv(GL_LIGHT0, GL_POSITION, straight);
    int full = (draw_lit_quad(c) >> 16) & 0xFF;

    /* 60 degrees from the normal: cos = 0.5. */
    GLfloat angled[4] = { 0.8660254f, 0.0f, 0.5f, 0.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, angled);
    int half_lit = (draw_lit_quad(c) >> 16) & 0xFF;

    /* Allow slack for the ambient offset and 8-bit quantisation. */
    int ok = full > 100 && half_lit > 40
          && half_lit < full * 0.7f && half_lit > full * 0.3f;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------- specular term ---- */

/* With shininess 0 there is no specular contribution at all. */
static int t_no_specular_without_shininess(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    GLfloat pos[4] = { 0, 0, 1, 0 };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    GLfloat black[4] = { 0, 0, 0, 1 };
    GLfloat white[4] = { 1, 1, 1, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, black);
    glMaterialfv(GL_FRONT, GL_AMBIENT, black);
    glMaterialfv(GL_FRONT, GL_SPECULAR, white);
    glMaterialf(GL_FRONT, GL_SHININESS, 0.0f);

    uint32_t p = draw_lit_quad(c);
    int ok = ((p >> 16) & 0xFF) < 10;
    aglxDestroyContext(c);
    return ok;
}

/* With shininess set, a light aligned with the viewer produces a highlight. */
static int t_specular_highlight(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    GLfloat pos[4] = { 0, 0, 1, 0 };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    GLfloat black[4] = { 0, 0, 0, 1 };
    GLfloat white[4] = { 1, 1, 1, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, black);
    glMaterialfv(GL_FRONT, GL_AMBIENT, black);
    glMaterialfv(GL_FRONT, GL_SPECULAR, white);
    glMaterialf(GL_FRONT, GL_SHININESS, 32.0f);

    uint32_t p = draw_lit_quad(c);
    int ok = ((p >> 16) & 0xFF) > 100;
    aglxDestroyContext(c);
    return ok;
}

/* Shininess out of range is rejected. */
static int t_shininess_range(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glMaterialf(GL_FRONT, GL_SHININESS, 200.0f);
    int ok = glGetError() == GL_INVALID_VALUE;
    glMaterialf(GL_FRONT, GL_SHININESS, -1.0f);
    ok = ok && glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------- ambient/emission - */

/* Emission is added unconditionally, even with no lights at all. */
static int t_emission_without_lights(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    /* No light enabled. */
    GLfloat black[4] = { 0, 0, 0, 1 };
    GLfloat green[4] = { 0, 1, 0, 1 };
    glMaterialfv(GL_FRONT, GL_AMBIENT, black);
    glMaterialfv(GL_FRONT, GL_DIFFUSE, black);
    glMaterialfv(GL_FRONT, GL_EMISSION, green);

    uint32_t p = draw_lit_quad(c);
    int ok = ((p >> 8) & 0xFF) > 250 && ((p >> 16) & 0xFF) < 10;
    aglxDestroyContext(c);
    return ok;
}

/* The scene ambient term applies with no lights enabled. */
static int t_scene_ambient(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    GLfloat red[4] = { 1, 0, 0, 1 };
    glMaterialfv(GL_FRONT, GL_AMBIENT, red);
    GLfloat amb[4] = { 1, 1, 1, 1 };
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);

    uint32_t p = draw_lit_quad(c);
    /* scene_ambient 1.0 * mat_ambient red -> saturated red. */
    int ok = ((p >> 16) & 0xFF) > 250 && ((p >> 8) & 0xFF) < 10;
    aglxDestroyContext(c);
    return ok;
}

/* --------------------------------------------------------- attenuation ---- */

/* A positional light must dim with distance once attenuation is configured. */
static int t_distance_attenuation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    GLfloat white[4] = { 1, 1, 1, 1 };
    GLfloat black[4] = { 0, 0, 0, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, white);
    glMaterialfv(GL_FRONT, GL_AMBIENT, black);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, black);
    glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, 0.5f);

    /* Positional light close to the quad centre (16,16,0). */
    GLfloat near_pos[4] = { 0, 0, -18, 1 };
    glLightfv(GL_LIGHT0, GL_POSITION, near_pos);
    int near_lit = (draw_lit_quad(c) >> 16) & 0xFF;

    GLfloat far_pos[4] = { 0, 0, 60, 1 };
    glLightfv(GL_LIGHT0, GL_POSITION, far_pos);
    int far_lit = (draw_lit_quad(c) >> 16) & 0xFF;

    int ok = near_lit > far_lit && far_lit >= 0;
    aglxDestroyContext(c);
    return ok;
}

/* A directional light must NOT attenuate, however far "away" it nominally is:
 * its position is a direction, not a point. */
static int t_directional_ignores_attenuation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    GLfloat white[4] = { 1, 1, 1, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, white);
    glLightf(GL_LIGHT0, GL_QUADRATIC_ATTENUATION, 10.0f);

    GLfloat dir[4] = { 0, 0, 1000, 0 };     /* w == 0: directional */
    glLightfv(GL_LIGHT0, GL_POSITION, dir);
    uint32_t p = draw_lit_quad(c);
    int ok = ((p >> 16) & 0xFF) > 200;
    aglxDestroyContext(c);
    return ok;
}

static int t_negative_attenuation_rejected(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, -1.0f);
    int ok = glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

/* ---------------------------------------------------------- spotlights ---- */

static int t_spot_cutoff_validation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 45.0f);
    int ok = glGetError() == GL_NO_ERROR;
    glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 180.0f);   /* the "off" value */
    ok = ok && glGetError() == GL_NO_ERROR;
    glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 120.0f);   /* invalid */
    ok = ok && glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

/* Geometry outside the spot cone receives no contribution from that light. */
static int t_spot_cone_excludes_outside(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    GLfloat white[4] = { 1, 1, 1, 1 };
    GLfloat black[4] = { 0, 0, 0, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, white);
    glMaterialfv(GL_FRONT, GL_AMBIENT, black);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, black);

    GLfloat pos[4] = { 0, 0, -10, 1 };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 25.0f);

    /* Aimed at the quad: lit. */
    GLfloat aim_at[4] = { 0, 0, -1, 0 };
    glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, aim_at);
    int lit = (draw_lit_quad(c) >> 16) & 0xFF;

    /* Aimed away: dark. */
    GLfloat aim_away[4] = { 1, 0, 0, 0 };
    glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, aim_away);
    int dark = (draw_lit_quad(c) >> 16) & 0xFF;

    int ok = lit > 100 && dark < 20;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------ colour material --- */

/* With GL_COLOR_MATERIAL on, glColor drives the ambient+diffuse material, so
 * per-vertex colours still work while lighting is enabled. */
static int t_color_material(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);

    GLfloat pos[4] = { 0, 0, 1, 0 };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);

    glColor3f(0.0f, 1.0f, 0.0f);            /* green via glColor */
    uint32_t p = draw_lit_quad(c);
    int ok = ((p >> 8) & 0xFF) > 200 && ((p >> 16) & 0xFF) < 60;
    aglxDestroyContext(c);
    return ok;
}

/* Without GL_COLOR_MATERIAL, glColor is ignored while lighting is on. */
static int t_color_ignored_when_lit(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glDisable(GL_COLOR_MATERIAL);
    GLfloat pos[4] = { 0, 0, 1, 0 };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    GLfloat red[4] = { 1, 0, 0, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, red);

    glColor3f(0.0f, 0.0f, 1.0f);            /* blue: must be ignored */
    uint32_t p = draw_lit_quad(c);
    int ok = ((p >> 16) & 0xFF) > 200 && (p & 0xFF) < 60;
    aglxDestroyContext(c);
    return ok;
}

static int t_color_material_invalid(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glColorMaterial(GL_FRONT, 0x9999);
    int ok = glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------------- normals ---- */

/* The light position must be transformed by the MODELVIEW in force when
 * glLightfv is called — that is what makes "before or after the camera
 * transform" behave differently. */
static int t_light_position_uses_modelview(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glLoadIdentity();
    glTranslatef(5.0f, 0.0f, 0.0f);
    GLfloat pos[4] = { 0, 0, 0, 1 };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glLoadIdentity();
    /* Stored position must be (5,0,0), not (0,0,0). */
    int ok = feq(c->lights[0].position.x, 5.0f, 1e-4f);
    aglxDestroyContext(c);
    return ok;
}

/* GL_NORMALIZE must compensate for a scaled MODELVIEW.  Without it, a scale of
 * 4 makes the normal four times too long and the diffuse term saturates. */
static int t_normalize_compensates_scale(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    GLfloat black[4] = { 0, 0, 0, 1 };
    glMaterialfv(GL_FRONT, GL_AMBIENT, black);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, black);
    GLfloat grey[4] = { 0.25f, 0.25f, 0.25f, 1.0f };
    glLightfv(GL_LIGHT0, GL_DIFFUSE, grey);
    GLfloat white[4] = { 1, 1, 1, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, white);
    GLfloat pos[4] = { 0, 0, 1, 0 };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);

    /* Baseline with no scaling. */
    glDisable(GL_NORMALIZE);
    glLoadIdentity();
    int base = (draw_lit_quad(c) >> 16) & 0xFF;

    /* A uniform scale of 4 on the modelview.  The inverse-transpose scales the
     * normal by 1/4, so without GL_NORMALIZE the result differs from base. */
    glLoadIdentity();
    glScalef(4.0f, 4.0f, 4.0f);
    int scaled_unnormalized = (draw_lit_quad(c) >> 16) & 0xFF;

    glEnable(GL_NORMALIZE);
    int scaled_normalized = (draw_lit_quad(c) >> 16) & 0xFF;

    glLoadIdentity();
    glDisable(GL_NORMALIZE);

    /* With GL_NORMALIZE the scaled case must match the unscaled baseline;
     * without it, it must not. */
    int ok = base > 20
          && scaled_normalized > base - 12 && scaled_normalized < base + 12
          && scaled_unnormalized < base - 12;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------- two-sided -------- */

static int t_two_side_setting(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1);
    int ok = c->light_model_two_side == GL_TRUE;
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 0);
    ok = ok && c->light_model_two_side == GL_FALSE
            && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Back and front materials are independent. */
static int t_front_back_materials_independent(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLfloat red[4]  = { 1, 0, 0, 1 };
    GLfloat blue[4] = { 0, 0, 1, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, red);
    glMaterialfv(GL_BACK,  GL_DIFFUSE, blue);
    int ok = feq(c->material_front.diffuse.r, 1.0f, 1e-6f)
          && feq(c->material_back.diffuse.b, 1.0f, 1e-6f)
          && feq(c->material_front.diffuse.b, 0.0f, 1e-6f);
    aglxDestroyContext(c);
    return ok;
}

/* GL_FRONT_AND_BACK sets both at once. */
static int t_front_and_back(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLfloat green[4] = { 0, 1, 0, 1 };
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, green);
    int ok = feq(c->material_front.diffuse.g, 1.0f, 1e-6f)
          && feq(c->material_back.diffuse.g, 1.0f, 1e-6f);
    aglxDestroyContext(c);
    return ok;
}

/* GL_AMBIENT_AND_DIFFUSE sets both components. */
static int t_ambient_and_diffuse(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLfloat v[4] = { 0.5f, 0.25f, 0.125f, 1.0f };
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, v);
    int ok = feq(c->material_front.ambient.r, 0.5f, 1e-6f)
          && feq(c->material_front.diffuse.r, 0.5f, 1e-6f)
          && feq(c->material_front.diffuse.g, 0.25f, 1e-6f);
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------------ interplay --- */

/* Two lights must sum, not replace one another. */
static int t_two_lights_sum(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    GLfloat black[4] = { 0, 0, 0, 1 };
    glMaterialfv(GL_FRONT, GL_AMBIENT, black);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, black);
    GLfloat white[4] = { 1, 1, 1, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, white);

    GLfloat dim[4] = { 0.3f, 0.3f, 0.3f, 1.0f };
    GLfloat pos[4] = { 0, 0, 1, 0 };
    glLightfv(GL_LIGHT0, GL_DIFFUSE, dim);
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glLightfv(GL_LIGHT1, GL_DIFFUSE, dim);
    glLightfv(GL_LIGHT1, GL_POSITION, pos);

    glEnable(GL_LIGHT0);
    glDisable(GL_LIGHT1);
    int one = (draw_lit_quad(c) >> 16) & 0xFF;

    glEnable(GL_LIGHT1);
    int two = (draw_lit_quad(c) >> 16) & 0xFF;

    int ok = one > 30 && two > one * 1.5f;
    aglxDestroyContext(c);
    return ok;
}

/* Lighting must interpolate across a primitive (Gouraud), not be flat. */
static int t_lighting_interpolates(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glShadeModel(GL_SMOOTH);
    GLfloat white[4] = { 1, 1, 1, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, white);
    GLfloat black[4] = { 0, 0, 0, 1 };
    glMaterialfv(GL_FRONT, GL_AMBIENT, black);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, black);
    GLfloat pos[4] = { 0, 0, 1, 0 };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);

    /* A quad whose two ends have opposing normals: one lit, one not. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_QUADS);
    glNormal3f(0, 0, 1);  glVertex3f(-2.0f, -2.0f, -20.0f);
    glNormal3f(0, 0, 1);  glVertex3f(-2.0f,  2.0f, -20.0f);
    glNormal3f(0, 0, -1); glVertex3f( 2.0f,  2.0f, -20.0f);
    glNormal3f(0, 0, -1); glVertex3f( 2.0f, -2.0f, -20.0f);
    glEnd();

    /* Sample inside the quad's actual footprint.  With glOrtho(-4,4,...) a
     * quad spanning -2..2 covers roughly the middle half of the 32-pixel
     * buffer, so points near the edges fall outside it. */
    int left  = (px(c, 12, 16) >> 16) & 0xFF;
    int right = (px(c, 21, 16) >> 16) & 0xFF;
    int ok = left > right + 40;    /* a gradient, not a flat fill */
    aglxDestroyContext(c);
    return ok;
}

/* Disabling lighting mid-stream restores plain vertex colours. */
static int t_toggle_lighting(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glColor3f(1, 0, 0);
    uint32_t lit = draw_lit_quad(c);

    glDisable(GL_LIGHTING);
    uint32_t unlit = draw_lit_quad(c);

    int ok = unlit == 0xFF0000 && lit != unlit;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------------------------ */

int main(void) {
    printf("=== gllight (G5) unit tests ===\n");

    printf("--- defaults ---\n");
    RUN(t_lighting_off_by_default); RUN(t_unlit_uses_vertex_color);
    RUN(t_max_lights_query); RUN(t_light0_defaults_white);
    RUN(t_enable_all_lights); RUN(t_light_out_of_range);

    printf("--- diffuse ---\n");
    RUN(t_diffuse_head_on); RUN(t_diffuse_perpendicular);
    RUN(t_diffuse_behind); RUN(t_diffuse_follows_cosine);

    printf("--- specular ---\n");
    RUN(t_no_specular_without_shininess); RUN(t_specular_highlight);
    RUN(t_shininess_range);

    printf("--- ambient / emission ---\n");
    RUN(t_emission_without_lights); RUN(t_scene_ambient);

    printf("--- attenuation ---\n");
    RUN(t_distance_attenuation); RUN(t_directional_ignores_attenuation);
    RUN(t_negative_attenuation_rejected);

    printf("--- spotlights ---\n");
    RUN(t_spot_cutoff_validation); RUN(t_spot_cone_excludes_outside);

    printf("--- colour material ---\n");
    RUN(t_color_material); RUN(t_color_ignored_when_lit);
    RUN(t_color_material_invalid);

    printf("--- normals ---\n");
    RUN(t_light_position_uses_modelview); RUN(t_normalize_compensates_scale);

    printf("--- two-sided / materials ---\n");
    RUN(t_two_side_setting); RUN(t_front_back_materials_independent);
    RUN(t_front_and_back); RUN(t_ambient_and_diffuse);

    printf("--- interplay ---\n");
    RUN(t_two_lights_sum); RUN(t_lighting_interpolates);
    RUN(t_toggle_lighting);

    printf("\ntest_gllight: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

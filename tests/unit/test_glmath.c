/*
 * test_glmath.c — host-side unit tests for libgl/src/glmath.c
 *
 * This test links the REAL libgl/src/glmath.c translation unit (see the
 * Makefile rule), not a copy of it.  That is the same technique already used
 * for kernel/lib/bitmap.h and kernel/mm/heap.c: the code under test is the
 * code that ships, so the test cannot drift away from the implementation.
 *
 * Focus areas:
 *   - column-major storage (the classic source of transposed-matrix bugs)
 *   - glFrustum / glOrtho against the matrices printed in the GL 1.1 spec
 *   - rotation behaviour verified by what it does to vectors, not by
 *     hard-coded matrix elements
 *   - inverse / normal matrix round trips
 */

#include <stdio.h>
#include <math.h>
#include "GL/glmath.h"

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

#define EPS 1e-5f

static int feq(float a, float b) {
    float d = a - b;
    if (d < 0) d = -d;
    return d <= EPS;
}

static int veq(glm_vec3 v, float x, float y, float z) {
    return feq(v.x, x) && feq(v.y, y) && feq(v.z, z);
}

/* ---------------------------------------------------------------- vec3 --- */

static int t_vec3_add(void) {
    return veq(glm_vec3_add(glm_vec3_make(1, 2, 3), glm_vec3_make(4, 5, 6)),
               5, 7, 9);
}

static int t_vec3_sub(void) {
    return veq(glm_vec3_sub(glm_vec3_make(4, 5, 6), glm_vec3_make(1, 2, 3)),
               3, 3, 3);
}

static int t_vec3_scale(void) {
    return veq(glm_vec3_scale(glm_vec3_make(1, -2, 3), 2.5f), 2.5f, -5, 7.5f);
}

static int t_vec3_dot(void) {
    return feq(glm_vec3_dot(glm_vec3_make(1, 2, 3), glm_vec3_make(4, -5, 6)),
               1 * 4 + 2 * -5 + 3 * 6);
}

/* x cross y must be +z — verifies right-handedness. */
static int t_vec3_cross_rh(void) {
    return veq(glm_vec3_cross(glm_vec3_make(1, 0, 0), glm_vec3_make(0, 1, 0)),
               0, 0, 1);
}

static int t_vec3_cross_anticommute(void) {
    glm_vec3 a = glm_vec3_make(1, 2, 3), b = glm_vec3_make(4, 5, 6);
    glm_vec3 ab = glm_vec3_cross(a, b), ba = glm_vec3_cross(b, a);
    return veq(ab, -ba.x, -ba.y, -ba.z);
}

static int t_vec3_length(void) {
    return feq(glm_vec3_length(glm_vec3_make(3, 4, 0)), 5.0f);
}

static int t_vec3_normalize(void) {
    glm_vec3 n = glm_vec3_normalize(glm_vec3_make(0, 3, 4));
    return feq(glm_vec3_length(n), 1.0f) && veq(n, 0, 0.6f, 0.8f);
}

/* A zero vector must not produce NaNs. */
static int t_vec3_normalize_zero(void) {
    glm_vec3 n = glm_vec3_normalize(glm_vec3_make(0, 0, 0));
    return veq(n, 0, 0, 0);
}

/* -------------------------------------------------------------- matrix --- */

static int t_identity(void) {
    glm_mat4 i = glm_mat4_identity();
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (!feq(i.m[c * 4 + r], (c == r) ? 1.0f : 0.0f)) return 0;
    return 1;
}

/* Translation MUST land in m[12..14] for column-major storage.  If someone
 * "fixes" the layout to row-major this test fires immediately. */
static int t_translate_layout(void) {
    glm_mat4 t = glm_mat4_translate(7, 8, 9);
    return feq(t.m[12], 7) && feq(t.m[13], 8) && feq(t.m[14], 9)
        && feq(t.m[3], 0) && feq(t.m[7], 0) && feq(t.m[11], 0)
        && feq(t.m[15], 1);
}

static int t_translate_applies(void) {
    glm_mat4 t = glm_mat4_translate(10, 20, 30);
    return veq(glm_mat4_transform_point(t, glm_vec3_make(1, 2, 3)), 11, 22, 33);
}

/* Directions must ignore translation. */
static int t_translate_ignores_dir(void) {
    glm_mat4 t = glm_mat4_translate(10, 20, 30);
    return veq(glm_mat4_transform_dir(t, glm_vec3_make(1, 2, 3)), 1, 2, 3);
}

static int t_scale_applies(void) {
    glm_mat4 s = glm_mat4_scale(2, 3, 4);
    return veq(glm_mat4_transform_point(s, glm_vec3_make(1, 1, 1)), 2, 3, 4);
}

static int t_identity_mul(void) {
    glm_mat4 t = glm_mat4_translate(1, 2, 3);
    glm_mat4 r = glm_mat4_mul(t, glm_mat4_identity());
    for (int i = 0; i < 16; i++) if (!feq(r.m[i], t.m[i])) return 0;
    return 1;
}

/* Rotating (1,0,0) by +90° about Z must give (0,1,0) in a right-handed
 * system.  This pins down the sign convention of the rotation matrices. */
static int t_rot_z_90(void) {
    glm_mat4 r = glm_mat4_rot_z(GLM_DEG2RAD(90.0f));
    return veq(glm_mat4_transform_point(r, glm_vec3_make(1, 0, 0)), 0, 1, 0);
}

static int t_rot_x_90(void) {
    glm_mat4 r = glm_mat4_rot_x(GLM_DEG2RAD(90.0f));
    return veq(glm_mat4_transform_point(r, glm_vec3_make(0, 1, 0)), 0, 0, 1);
}

static int t_rot_y_90(void) {
    glm_mat4 r = glm_mat4_rot_y(GLM_DEG2RAD(90.0f));
    return veq(glm_mat4_transform_point(r, glm_vec3_make(0, 0, 1)), 1, 0, 0);
}

/* rot_axis about Z must agree with the dedicated rot_z. */
static int t_rot_axis_matches_z(void) {
    glm_mat4 a = glm_mat4_rot_axis(GLM_DEG2RAD(37.0f), 0, 0, 1);
    glm_mat4 b = glm_mat4_rot_z(GLM_DEG2RAD(37.0f));
    for (int i = 0; i < 16; i++) if (!feq(a.m[i], b.m[i])) return 0;
    return 1;
}

/* A non-normalised axis must be normalised internally. */
static int t_rot_axis_unnormalised(void) {
    glm_mat4 a = glm_mat4_rot_axis(GLM_DEG2RAD(90.0f), 0, 0, 5);
    return veq(glm_mat4_transform_point(a, glm_vec3_make(1, 0, 0)), 0, 1, 0);
}

static int t_rot_axis_zero(void) {
    glm_mat4 a = glm_mat4_rot_axis(GLM_DEG2RAD(90.0f), 0, 0, 0);
    glm_mat4 i = glm_mat4_identity();
    for (int k = 0; k < 16; k++) if (!feq(a.m[k], i.m[k])) return 0;
    return 1;
}

/* Rotation by 360° is the identity. */
static int t_rot_full_turn(void) {
    glm_mat4 r = glm_mat4_rot_y(GLM_DEG2RAD(360.0f));
    return veq(glm_mat4_transform_point(r, glm_vec3_make(1, 2, 3)), 1, 2, 3);
}

/* Order matters: glm_mat4_mul(a,b) applies b first, then a — the OpenGL
 * convention.  Translate-then-rotate differs from rotate-then-translate. */
static int t_mul_order(void) {
    glm_mat4 t = glm_mat4_translate(1, 0, 0);
    glm_mat4 r = glm_mat4_rot_z(GLM_DEG2RAD(90.0f));

    /* rotate(translate(p)): p=(0,0,0) -> (1,0,0) -> (0,1,0) */
    glm_vec3 p1 = glm_mat4_transform_point(glm_mat4_mul(r, t),
                                           glm_vec3_make(0, 0, 0));
    /* translate(rotate(p)): p=(0,0,0) -> (0,0,0) -> (1,0,0) */
    glm_vec3 p2 = glm_mat4_transform_point(glm_mat4_mul(t, r),
                                           glm_vec3_make(0, 0, 0));
    return veq(p1, 0, 1, 0) && veq(p2, 1, 0, 0);
}

static int t_transpose_involution(void) {
    glm_mat4 a = glm_mat4_mul(glm_mat4_translate(1, 2, 3),
                              glm_mat4_rot_x(0.7f));
    glm_mat4 b = glm_mat4_transpose(glm_mat4_transpose(a));
    for (int i = 0; i < 16; i++) if (!feq(a.m[i], b.m[i])) return 0;
    return 1;
}

static int t_inverse_roundtrip(void) {
    glm_mat4 a = glm_mat4_mul(glm_mat4_translate(3, -2, 5),
                              glm_mat4_rot_y(0.9f));
    a = glm_mat4_mul(a, glm_mat4_scale(2, 2, 2));
    glm_mat4 inv;
    if (!glm_mat4_inverse(a, &inv)) return 0;
    glm_mat4 id = glm_mat4_mul(a, inv);
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (!feq(id.m[c * 4 + r], (c == r) ? 1.0f : 0.0f)) return 0;
    return 1;
}

static int t_inverse_singular(void) {
    /* Scale by zero collapses a dimension — not invertible. */
    glm_mat4 inv;
    return glm_mat4_inverse(glm_mat4_scale(1, 0, 1), &inv) == 0;
}

/* Under non-uniform scaling the normal matrix must keep normals
 * perpendicular to the surface.  Scaling x by 2 turns the surface normal
 * (1,1,0) into (0.5,1,0) normalised — NOT (2,1,0). */
static int t_normal_matrix(void) {
    glm_mat4 mv = glm_mat4_scale(2, 1, 1);
    glm_mat4 nm = glm_mat4_normal(mv);
    glm_vec3 n = glm_vec3_normalize(
        glm_mat4_transform_dir(nm, glm_vec3_make(1, 1, 0)));
    glm_vec3 expect = glm_vec3_normalize(glm_vec3_make(0.5f, 1, 0));
    return veq(n, expect.x, expect.y, expect.z);
}

/* For a rigid transform the normal matrix equals the rotation itself. */
static int t_normal_matrix_rigid(void) {
    glm_mat4 mv = glm_mat4_mul(glm_mat4_translate(5, 6, 7),
                               glm_mat4_rot_z(0.5f));
    glm_mat4 nm = glm_mat4_normal(mv);
    glm_vec3 a = glm_mat4_transform_dir(nm, glm_vec3_make(1, 0, 0));
    glm_vec3 b = glm_mat4_transform_dir(mv, glm_vec3_make(1, 0, 0));
    return veq(a, b.x, b.y, b.z);
}

/* ---------------------------------------------------------- projection --- */

/* glOrtho reference matrix, GL 1.1 §2.10.2. */
static int t_ortho_spec(void) {
    glm_mat4 o = glm_mat4_ortho(-2, 2, -3, 3, 1, 5);
    return feq(o.m[0], 2.0f / 4.0f)      /* 2/(r-l) */
        && feq(o.m[5], 2.0f / 6.0f)      /* 2/(t-b) */
        && feq(o.m[10], -2.0f / 4.0f)    /* -2/(f-n) */
        && feq(o.m[12], 0.0f)            /* -(r+l)/(r-l) = 0 */
        && feq(o.m[13], 0.0f)            /* -(t+b)/(t-b) = 0 */
        && feq(o.m[14], -6.0f / 4.0f)    /* -(f+n)/(f-n) */
        && feq(o.m[15], 1.0f);
}

/* Ortho maps the near-plane centre to the origin of NDC. */
static int t_ortho_maps_centre(void) {
    glm_mat4 o = glm_mat4_ortho(-2, 2, -3, 3, 1, 5);
    return veq(glm_mat4_transform_point(o, glm_vec3_make(0, 0, -1)),
               0, 0, -1);
}

/* glFrustum reference matrix, GL 1.1 §2.10.2. */
static int t_frustum_spec(void) {
    glm_mat4 f = glm_mat4_frustum(-1, 1, -1, 1, 1, 100);
    return feq(f.m[0], 1.0f)                            /* 2n/(r-l) */
        && feq(f.m[5], 1.0f)                            /* 2n/(t-b) */
        && feq(f.m[10], -(100.0f + 1.0f) / 99.0f)       /* -(f+n)/(f-n) */
        && feq(f.m[11], -1.0f)                          /* the -1 in row 3 */
        && feq(f.m[14], -(2.0f * 100.0f * 1.0f) / 99.0f)/* -2fn/(f-n) */
        && feq(f.m[15], 0.0f);
}

/* A point on the near plane must land on the NDC near plane (z = -1). */
static int t_frustum_near_plane(void) {
    glm_mat4 f = glm_mat4_frustum(-1, 1, -1, 1, 1, 100);
    return veq(glm_mat4_transform_point(f, glm_vec3_make(0, 0, -1)), 0, 0, -1);
}

/* ...and a point on the far plane must land on z = +1. */
static int t_frustum_far_plane(void) {
    glm_mat4 f = glm_mat4_frustum(-1, 1, -1, 1, 1, 100);
    return veq(glm_mat4_transform_point(f, glm_vec3_make(0, 0, -100)),
               0, 0, 1);
}

/* Perspective must agree with the equivalent frustum. */
static int t_perspective_matches_frustum(void) {
    float n = 0.5f, fa = 50.0f, aspect = 4.0f / 3.0f;
    float fovy = GLM_DEG2RAD(60.0f);
    float t = n * tanf(fovy * 0.5f);
    float r = t * aspect;
    glm_mat4 a = glm_mat4_perspective(fovy, aspect, n, fa);
    glm_mat4 b = glm_mat4_frustum(-r, r, -t, t, n, fa);
    for (int i = 0; i < 16; i++) if (!feq(a.m[i], b.m[i])) return 0;
    return 1;
}

/* Degenerate parameters must not produce NaNs. */
static int t_frustum_degenerate(void) {
    glm_mat4 f = glm_mat4_frustum(1, 1, -1, 1, 1, 100);  /* l == r */
    glm_mat4 i = glm_mat4_identity();
    for (int k = 0; k < 16; k++) if (!feq(f.m[k], i.m[k])) return 0;
    return 1;
}

/* gluLookAt: an eye on +Z looking at the origin leaves a point at the origin
 * sitting at -distance along the view axis. */
static int t_lookat_basic(void) {
    glm_mat4 v = glm_mat4_look_at(glm_vec3_make(0, 0, 10),
                                  glm_vec3_make(0, 0, 0),
                                  glm_vec3_make(0, 1, 0));
    return veq(glm_mat4_transform_point(v, glm_vec3_make(0, 0, 0)), 0, 0, -10);
}

/* The eye position itself maps to the view-space origin. */
static int t_lookat_eye_at_origin(void) {
    glm_vec3 eye = glm_vec3_make(3, 4, 5);
    glm_mat4 v = glm_mat4_look_at(eye, glm_vec3_make(0, 0, 0),
                                  glm_vec3_make(0, 1, 0));
    return veq(glm_mat4_transform_point(v, eye), 0, 0, 0);
}

/* ------------------------------------------------------------------------ */

int main(void) {
    printf("=== glmath unit tests ===\n");

    printf("--- vec3 ---\n");
    RUN(t_vec3_add); RUN(t_vec3_sub); RUN(t_vec3_scale); RUN(t_vec3_dot);
    RUN(t_vec3_cross_rh); RUN(t_vec3_cross_anticommute);
    RUN(t_vec3_length); RUN(t_vec3_normalize); RUN(t_vec3_normalize_zero);

    printf("--- matrix basics ---\n");
    RUN(t_identity); RUN(t_translate_layout); RUN(t_translate_applies);
    RUN(t_translate_ignores_dir); RUN(t_scale_applies); RUN(t_identity_mul);

    printf("--- rotation ---\n");
    RUN(t_rot_z_90); RUN(t_rot_x_90); RUN(t_rot_y_90);
    RUN(t_rot_axis_matches_z); RUN(t_rot_axis_unnormalised);
    RUN(t_rot_axis_zero); RUN(t_rot_full_turn); RUN(t_mul_order);

    printf("--- inverse / normal matrix ---\n");
    RUN(t_transpose_involution); RUN(t_inverse_roundtrip);
    RUN(t_inverse_singular); RUN(t_normal_matrix); RUN(t_normal_matrix_rigid);

    printf("--- projection ---\n");
    RUN(t_ortho_spec); RUN(t_ortho_maps_centre);
    RUN(t_frustum_spec); RUN(t_frustum_near_plane); RUN(t_frustum_far_plane);
    RUN(t_perspective_matches_frustum); RUN(t_frustum_degenerate);
    RUN(t_lookat_basic); RUN(t_lookat_eye_at_origin);

    printf("\ntest_glmath: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

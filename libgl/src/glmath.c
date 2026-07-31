/* libgl/src/glmath.c — vector/matrix math for the AuraLite GL stack.
 *
 * See GL/glmath.h for the column-major storage convention.
 *
 * This file is deliberately dependency-free apart from <math.h> so that the
 * exact same translation unit is linked into both the GL library and the host
 * unit test (tests/unit/test_glmath.c) — the technique already used by
 * kernel/lib/bitmap.h and kernel/mm/heap.c.
 */

#include "GL/glmath.h"
#include <math.h>

/* Index helper: column-major, element (row, col). */
#define M(mat, row, col) ((mat).m[(col) * 4 + (row)])

/* ============================================================================
 * vec3
 * ==========================================================================*/

glm_vec3 glm_vec3_make(float x, float y, float z) {
    glm_vec3 v; v.x = x; v.y = y; v.z = z; return v;
}

glm_vec3 glm_vec3_add(glm_vec3 a, glm_vec3 b) {
    return glm_vec3_make(a.x + b.x, a.y + b.y, a.z + b.z);
}

glm_vec3 glm_vec3_sub(glm_vec3 a, glm_vec3 b) {
    return glm_vec3_make(a.x - b.x, a.y - b.y, a.z - b.z);
}

glm_vec3 glm_vec3_scale(glm_vec3 a, float s) {
    return glm_vec3_make(a.x * s, a.y * s, a.z * s);
}

float glm_vec3_dot(glm_vec3 a, glm_vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

glm_vec3 glm_vec3_cross(glm_vec3 a, glm_vec3 b) {
    return glm_vec3_make(a.y * b.z - a.z * b.y,
                         a.z * b.x - a.x * b.z,
                         a.x * b.y - a.y * b.x);
}

float glm_vec3_length(glm_vec3 a) {
    return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
}

glm_vec3 glm_vec3_normalize(glm_vec3 a) {
    float len = glm_vec3_length(a);
    /* A zero-length vector has no direction; returning it unchanged is safer
     * than producing NaNs that would then propagate through the pipeline. */
    if (len <= 1e-20f) return a;
    float inv = 1.0f / len;
    return glm_vec3_make(a.x * inv, a.y * inv, a.z * inv);
}

/* ============================================================================
 * vec4
 * ==========================================================================*/

glm_vec4 glm_vec4_make(float x, float y, float z, float w) {
    glm_vec4 v; v.x = x; v.y = y; v.z = z; v.w = w; return v;
}

float glm_vec4_dot(glm_vec4 a, glm_vec4 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

/* ============================================================================
 * Matrix construction
 * ==========================================================================*/

glm_mat4 glm_mat4_zero(void) {
    glm_mat4 r;
    for (int i = 0; i < 16; i++) r.m[i] = 0.0f;
    return r;
}

glm_mat4 glm_mat4_identity(void) {
    glm_mat4 r = glm_mat4_zero();
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

glm_mat4 glm_mat4_translate(float x, float y, float z) {
    glm_mat4 r = glm_mat4_identity();
    /* Column-major: translation occupies the last column, m[12..14]. */
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

glm_mat4 glm_mat4_scale(float x, float y, float z) {
    glm_mat4 r = glm_mat4_zero();
    r.m[0]  = x;
    r.m[5]  = y;
    r.m[10] = z;
    r.m[15] = 1.0f;
    return r;
}

glm_mat4 glm_mat4_rot_x(float rad) {
    glm_mat4 r = glm_mat4_identity();
    float c = cosf(rad), s = sinf(rad);
    M(r, 1, 1) =  c;  M(r, 1, 2) = -s;
    M(r, 2, 1) =  s;  M(r, 2, 2) =  c;
    return r;
}

glm_mat4 glm_mat4_rot_y(float rad) {
    glm_mat4 r = glm_mat4_identity();
    float c = cosf(rad), s = sinf(rad);
    M(r, 0, 0) =  c;  M(r, 0, 2) =  s;
    M(r, 2, 0) = -s;  M(r, 2, 2) =  c;
    return r;
}

glm_mat4 glm_mat4_rot_z(float rad) {
    glm_mat4 r = glm_mat4_identity();
    float c = cosf(rad), s = sinf(rad);
    M(r, 0, 0) =  c;  M(r, 0, 1) = -s;
    M(r, 1, 0) =  s;  M(r, 1, 1) =  c;
    return r;
}

/* Rodrigues' rotation formula, matching the glRotatef() matrix in GL 1.1
 * §2.10.2 exactly. */
glm_mat4 glm_mat4_rot_axis(float rad, float ax, float ay, float az) {
    glm_vec3 axis = glm_vec3_make(ax, ay, az);
    float len = glm_vec3_length(axis);
    if (len <= 1e-20f) return glm_mat4_identity();
    axis = glm_vec3_scale(axis, 1.0f / len);

    float x = axis.x, y = axis.y, z = axis.z;
    float c = cosf(rad), s = sinf(rad);
    float ic = 1.0f - c;

    glm_mat4 r = glm_mat4_identity();
    M(r, 0, 0) = x * x * ic + c;
    M(r, 0, 1) = x * y * ic - z * s;
    M(r, 0, 2) = x * z * ic + y * s;
    M(r, 1, 0) = y * x * ic + z * s;
    M(r, 1, 1) = y * y * ic + c;
    M(r, 1, 2) = y * z * ic - x * s;
    M(r, 2, 0) = z * x * ic - y * s;
    M(r, 2, 1) = z * y * ic + x * s;
    M(r, 2, 2) = z * z * ic + c;
    return r;
}

/* glFrustum, GL 1.1 §2.10.2. */
glm_mat4 glm_mat4_frustum(float l, float r_, float b, float t,
                          float n, float f) {
    glm_mat4 r = glm_mat4_zero();
    if (r_ == l || t == b || f == n) return glm_mat4_identity();
    M(r, 0, 0) = (2.0f * n) / (r_ - l);
    M(r, 0, 2) = (r_ + l) / (r_ - l);
    M(r, 1, 1) = (2.0f * n) / (t - b);
    M(r, 1, 2) = (t + b) / (t - b);
    M(r, 2, 2) = -(f + n) / (f - n);
    M(r, 2, 3) = -(2.0f * f * n) / (f - n);
    M(r, 3, 2) = -1.0f;
    return r;
}

/* glOrtho, GL 1.1 §2.10.2. */
glm_mat4 glm_mat4_ortho(float l, float r_, float b, float t,
                        float n, float f) {
    glm_mat4 r = glm_mat4_identity();
    if (r_ == l || t == b || f == n) return glm_mat4_identity();
    M(r, 0, 0) =  2.0f / (r_ - l);
    M(r, 1, 1) =  2.0f / (t - b);
    M(r, 2, 2) = -2.0f / (f - n);
    M(r, 0, 3) = -(r_ + l) / (r_ - l);
    M(r, 1, 3) = -(t + b) / (t - b);
    M(r, 2, 3) = -(f + n) / (f - n);
    return r;
}

/* gluPerspective expressed as a frustum. */
glm_mat4 glm_mat4_perspective(float fovy_rad, float aspect, float n, float f) {
    if (aspect <= 0.0f || fovy_rad <= 0.0f) return glm_mat4_identity();
    float t = n * tanf(fovy_rad * 0.5f);
    float r = t * aspect;
    return glm_mat4_frustum(-r, r, -t, t, n, f);
}

/* gluLookAt. */
glm_mat4 glm_mat4_look_at(glm_vec3 eye, glm_vec3 center, glm_vec3 up) {
    glm_vec3 f = glm_vec3_normalize(glm_vec3_sub(center, eye));
    glm_vec3 s = glm_vec3_normalize(glm_vec3_cross(f, up));
    glm_vec3 u = glm_vec3_cross(s, f);

    glm_mat4 r = glm_mat4_identity();
    M(r, 0, 0) =  s.x; M(r, 0, 1) =  s.y; M(r, 0, 2) =  s.z;
    M(r, 1, 0) =  u.x; M(r, 1, 1) =  u.y; M(r, 1, 2) =  u.z;
    M(r, 2, 0) = -f.x; M(r, 2, 1) = -f.y; M(r, 2, 2) = -f.z;
    M(r, 0, 3) = -glm_vec3_dot(s, eye);
    M(r, 1, 3) = -glm_vec3_dot(u, eye);
    M(r, 2, 3) =  glm_vec3_dot(f, eye);
    return r;
}

/* ============================================================================
 * Matrix operations
 * ==========================================================================*/

glm_mat4 glm_mat4_mul(glm_mat4 a, glm_mat4 b) {
    glm_mat4 r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += M(a, row, k) * M(b, k, col);
            }
            M(r, row, col) = sum;
        }
    }
    return r;
}

glm_mat4 glm_mat4_transpose(glm_mat4 a) {
    glm_mat4 r;
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++)
            M(r, row, col) = M(a, col, row);
    return r;
}

int glm_mat4_inverse(glm_mat4 a, glm_mat4 *out) {
    if (!out) return 0;

    const float *m = a.m;
    float inv[16];

    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
             + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
             - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
             + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
             - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
             - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
             + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
             - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
             + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
             + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
             - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
             + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
             - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
             - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
             + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
             - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
             + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det > -1e-20f && det < 1e-20f) {
        *out = glm_mat4_identity();
        return 0;
    }

    float idet = 1.0f / det;
    for (int i = 0; i < 16; i++) out->m[i] = inv[i] * idet;
    return 1;
}

glm_mat4 glm_mat4_normal(glm_mat4 modelview) {
    /* Zero the translation: normals are directions, so only the linear part
     * matters.  Then invert-transpose so that non-uniform scaling does not
     * shear the normals away from the surface (GL 1.1 §2.10.3). */
    glm_mat4 linear = modelview;
    linear.m[12] = linear.m[13] = linear.m[14] = 0.0f;
    linear.m[3]  = linear.m[7]  = linear.m[11] = 0.0f;
    linear.m[15] = 1.0f;

    glm_mat4 inv;
    if (!glm_mat4_inverse(linear, &inv)) return glm_mat4_identity();
    return glm_mat4_transpose(inv);
}

/* ============================================================================
 * Transforms
 * ==========================================================================*/

glm_vec4 glm_mat4_transform4(glm_mat4 m, glm_vec4 v) {
    glm_vec4 r;
    r.x = M(m,0,0)*v.x + M(m,0,1)*v.y + M(m,0,2)*v.z + M(m,0,3)*v.w;
    r.y = M(m,1,0)*v.x + M(m,1,1)*v.y + M(m,1,2)*v.z + M(m,1,3)*v.w;
    r.z = M(m,2,0)*v.x + M(m,2,1)*v.y + M(m,2,2)*v.z + M(m,2,3)*v.w;
    r.w = M(m,3,0)*v.x + M(m,3,1)*v.y + M(m,3,2)*v.z + M(m,3,3)*v.w;
    return r;
}

glm_vec3 glm_mat4_transform_point(glm_mat4 m, glm_vec3 v) {
    glm_vec4 r = glm_mat4_transform4(m, glm_vec4_make(v.x, v.y, v.z, 1.0f));
    if (r.w > -1e-20f && r.w < 1e-20f) return glm_vec3_make(r.x, r.y, r.z);
    float inv = 1.0f / r.w;
    return glm_vec3_make(r.x * inv, r.y * inv, r.z * inv);
}

glm_vec3 glm_mat4_transform_dir(glm_mat4 m, glm_vec3 v) {
    glm_vec4 r = glm_mat4_transform4(m, glm_vec4_make(v.x, v.y, v.z, 0.0f));
    return glm_vec3_make(r.x, r.y, r.z);
}

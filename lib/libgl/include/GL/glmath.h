/* libgl/include/GL/glmath.h — vector/matrix math for the AuraLite GL stack.
 *
 * Ported and extended from drivers/framebuffer/render3d.h.  Unlike that
 * kernel-side renderer, this layer is pure user space, has no dependency on
 * the framebuffer, and is deliberately free of global state so the whole file
 * can be compiled and unit-tested on the host (tests/unit/test_glmath.c).
 *
 * MATRIX STORAGE — the single most important convention here.
 *
 * Matrices are COLUMN-MAJOR, exactly as OpenGL specifies (GL 1.1 §2.10.2):
 * element m[i] with i = column*4 + row.  Written out, the array
 *
 *     { m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15 }
 *
 * represents the matrix
 *
 *     | m0  m4  m8  m12 |
 *     | m1  m5  m9  m13 |
 *     | m2  m6  m10 m14 |
 *     | m3  m7  m11 m15 |
 *
 * so the translation components live in m12, m13, m14.  This matches what
 * glLoadMatrixf() expects, which is why applications can hand these arrays
 * straight to GL.  Getting this backwards is the classic source of
 * transposed-transform bugs, hence the explicit note.
 */
#ifndef AURALITE_GLMATH_H
#define AURALITE_GLMATH_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GLM_PI
#define GLM_PI 3.14159265358979323846f
#endif

#define GLM_DEG2RAD(d) ((float)(d) * (GLM_PI / 180.0f))
#define GLM_RAD2DEG(r) ((float)(r) * (180.0f / GLM_PI))

/* ---- Vectors ---- */
typedef struct { float x, y, z; }    glm_vec3;
typedef struct { float x, y, z, w; } glm_vec4;

/* ---- 4x4 matrix, column-major (see note above) ---- */
typedef struct { float m[16]; } glm_mat4;

/* ---- vec3 ---- */
glm_vec3 glm_vec3_make(float x, float y, float z);
glm_vec3 glm_vec3_add(glm_vec3 a, glm_vec3 b);
glm_vec3 glm_vec3_sub(glm_vec3 a, glm_vec3 b);
glm_vec3 glm_vec3_scale(glm_vec3 a, float s);
float    glm_vec3_dot(glm_vec3 a, glm_vec3 b);
glm_vec3 glm_vec3_cross(glm_vec3 a, glm_vec3 b);
float    glm_vec3_length(glm_vec3 a);
glm_vec3 glm_vec3_normalize(glm_vec3 a);

/* ---- vec4 ---- */
glm_vec4 glm_vec4_make(float x, float y, float z, float w);
float    glm_vec4_dot(glm_vec4 a, glm_vec4 b);

/* ---- Matrix construction ---- */
glm_mat4 glm_mat4_identity(void);
glm_mat4 glm_mat4_zero(void);
glm_mat4 glm_mat4_translate(float x, float y, float z);
glm_mat4 glm_mat4_scale(float x, float y, float z);
glm_mat4 glm_mat4_rot_x(float radians);
glm_mat4 glm_mat4_rot_y(float radians);
glm_mat4 glm_mat4_rot_z(float radians);

/* Rotation of `radians` about an arbitrary axis (the axis is normalised
 * internally; a zero-length axis yields the identity).  This is the primitive
 * behind glRotatef(). */
glm_mat4 glm_mat4_rot_axis(float radians, float x, float y, float z);

/* Projections.  glm_mat4_frustum/ortho match the glFrustum/glOrtho matrices
 * from the specification exactly; glm_mat4_perspective is the gluPerspective
 * convenience form (fovy in RADIANS). */
glm_mat4 glm_mat4_frustum(float l, float r, float b, float t, float n, float f);
glm_mat4 glm_mat4_ortho(float l, float r, float b, float t, float n, float f);
glm_mat4 glm_mat4_perspective(float fovy_rad, float aspect, float n, float f);
glm_mat4 glm_mat4_look_at(glm_vec3 eye, glm_vec3 center, glm_vec3 up);

/* ---- Matrix operations ---- */

/* Matrix product in OpenGL order: the result applies `b` first, then `a`.
 * This is what glMultMatrix() does to the current matrix. */
glm_mat4 glm_mat4_mul(glm_mat4 a, glm_mat4 b);

glm_mat4 glm_mat4_transpose(glm_mat4 a);

/* General 4x4 inverse via cofactors.  Returns 1 on success; on a singular
 * matrix returns 0 and leaves *out as the identity. */
int glm_mat4_inverse(glm_mat4 a, glm_mat4 *out);

/* Normal matrix: inverse-transpose of the upper-left 3x3, returned as a mat4
 * with a unit w row/column.  Required to transform normals correctly under
 * non-uniform scaling (GL 1.1 §2.10.3). */
glm_mat4 glm_mat4_normal(glm_mat4 modelview);

/* ---- Transforms ---- */

/* Full 4-component transform (no division performed). */
glm_vec4 glm_mat4_transform4(glm_mat4 m, glm_vec4 v);

/* Transform a point (implicit w = 1) and perform the perspective divide.
 * If the resulting w is ~0 the input is returned unchanged. */
glm_vec3 glm_mat4_transform_point(glm_mat4 m, glm_vec3 v);

/* Transform a direction (implicit w = 0): translation is ignored. */
glm_vec3 glm_mat4_transform_dir(glm_mat4 m, glm_vec3 v);

#ifdef __cplusplus
}
#endif

#endif /* AURALITE_GLMATH_H */

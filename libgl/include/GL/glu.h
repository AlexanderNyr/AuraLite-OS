/* libgl/include/GL/glu.h — GLU utility layer for AuraLite OS.
 *
 * Phase G8 of GL_PLAN.md.
 *
 * GLU is not part of OpenGL proper: it is a companion library of convenience
 * routines built entirely on top of the GL entry points.  Nothing here reaches
 * into libgl's internals, which is exactly how the real GLU works and why it
 * lives in a separate header.
 *
 * The pieces provided are the ones applications actually reach for:
 *   - gluPerspective / gluLookAt / gluOrtho2D, so a demo does not have to
 *     hand-derive a frustum every time
 *   - gluErrorString, so glGetError() can be reported readably
 *   - quadrics (sphere, cylinder, disk), which is how most GL tutorials draw
 *     anything rounder than a cube
 */
#ifndef AURALITE_GLU_H
#define AURALITE_GLU_H

#include "GL/gl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Quadric draw styles (§GLU 6.1) ---- */
#define GLU_FILL        100012
#define GLU_LINE        100011
#define GLU_POINT       100010
#define GLU_SILHOUETTE  100013

/* ---- Quadric normal generation ---- */
#define GLU_NONE        100002
#define GLU_FLAT        100001
#define GLU_SMOOTH      100000

/* ---- Quadric orientation ---- */
#define GLU_OUTSIDE     100020
#define GLU_INSIDE      100021

/* ---- Matrix helpers ----
 *
 * Each multiplies the CURRENT matrix, exactly like glFrustum does, so the
 * usual glMatrixMode/glLoadIdentity preamble still applies.
 */

/* fovy is in DEGREES, matching the real GLU (unlike glm_mat4_perspective,
 * which takes radians).  Mixing the two up is a classic source of a scene
 * that renders but looks wrong, so the units are stated here explicitly. */
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar);

void gluLookAt(GLdouble eyeX, GLdouble eyeY, GLdouble eyeZ,
               GLdouble centerX, GLdouble centerY, GLdouble centerZ,
               GLdouble upX, GLdouble upY, GLdouble upZ);

void gluOrtho2D(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top);

/* ---- Error reporting ---- */

/* Returns a static string; never NULL, even for an unknown code. */
const GLubyte *gluErrorString(GLenum error);

/* ---- Quadrics ----
 *
 * A quadric object holds drawing style, normal generation and orientation.
 * The real GLU makes this opaque and heap-allocated; so does this one, so
 * application code ports unchanged.
 */
typedef struct GLUquadric GLUquadric;

GLUquadric *gluNewQuadric(void);
void gluDeleteQuadric(GLUquadric *q);
void gluQuadricDrawStyle(GLUquadric *q, GLenum drawStyle);
void gluQuadricNormals(GLUquadric *q, GLenum normals);
void gluQuadricOrientation(GLUquadric *q, GLenum orientation);
void gluQuadricTexture(GLUquadric *q, GLboolean textureCoords);

void gluSphere(GLUquadric *q, GLdouble radius, GLint slices, GLint stacks);
void gluCylinder(GLUquadric *q, GLdouble base, GLdouble top, GLdouble height,
                 GLint slices, GLint stacks);
void gluDisk(GLUquadric *q, GLdouble inner, GLdouble outer,
             GLint slices, GLint loops);

/* ---- Mipmap construction (phase G10) ----
 *
 * Uploads level 0 and then every smaller level, box-filtering as it goes.
 * Returns 0 on success and a GLU error code otherwise, matching the real GLU.
 *
 * Unlike the real GLU this does NOT rescale a non-power-of-two image to the
 * next power of two first: the sampler here handles arbitrary sizes, and
 * silently resampling the application's data would be a surprise.  Halving
 * stops at 1 in each dimension independently, so a 12x5 image is a legal
 * chain.
 */
int gluBuild2DMipmaps(GLenum target, GLint internalFormat,
                      GLsizei width, GLsizei height,
                      GLenum format, GLenum type, const void *data);

/* Box-filtered halving of a raw image, exposed because it is useful on its
 * own and because it is what makes gluBuild2DMipmaps testable in isolation. */
int gluScaleImageHalf(GLenum format, GLsizei width, GLsizei height,
                      const unsigned char *src, unsigned char *dst);

#ifdef __cplusplus
}
#endif

#endif /* AURALITE_GLU_H */

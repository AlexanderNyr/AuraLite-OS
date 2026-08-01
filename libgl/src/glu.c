/* libgl/src/glu.c — GLU utility layer.
 *
 * Phase G8 of GL_PLAN.md.
 *
 * Everything here is written against the public GL API only — no glcontext.h,
 * no internal helpers.  That is deliberate: GLU is a convenience library, and
 * keeping it on the public side proves the API is complete enough to build on.
 * If something here needed an internal hook, that would be a sign the GL layer
 * was missing an entry point.
 */

#include <stdlib.h>
#include <math.h>

#include "GL/gl.h"
#include "GL/glu.h"

#ifndef GLU_PI
#define GLU_PI 3.14159265358979323846
#endif

/* ============================================================================
 * Matrix helpers
 * ==========================================================================*/

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear,
                    GLdouble zFar) {
    /* fovy arrives in DEGREES.  The half-angle tangent gives the half-height
     * of the near plane, and the frustum follows from that. */
    if (aspect == 0.0 || zNear == zFar) return;

    GLdouble half = fovy * 0.5 * (GLU_PI / 180.0);
    GLdouble top  = zNear * tan(half);
    GLdouble right = top * aspect;

    glFrustum(-right, right, -top, top, zNear, zFar);
}

void gluLookAt(GLdouble eyeX, GLdouble eyeY, GLdouble eyeZ,
               GLdouble centerX, GLdouble centerY, GLdouble centerZ,
               GLdouble upX, GLdouble upY, GLdouble upZ) {
    /* Forward, side and true-up axes of the camera basis. */
    GLdouble fx = centerX - eyeX, fy = centerY - eyeY, fz = centerZ - eyeZ;
    GLdouble flen = sqrt(fx*fx + fy*fy + fz*fz);
    if (flen < 1e-12) return;                 /* eye == center: no direction */
    fx /= flen; fy /= flen; fz /= flen;

    /* side = forward x up */
    GLdouble sx = fy*upZ - fz*upY;
    GLdouble sy = fz*upX - fx*upZ;
    GLdouble sz = fx*upY - fy*upX;
    GLdouble slen = sqrt(sx*sx + sy*sy + sz*sz);
    if (slen < 1e-12) return;                 /* up parallel to forward */
    sx /= slen; sy /= slen; sz /= slen;

    /* u = side x forward.  Recomputing this rather than using the supplied up
     * vector is what lets callers pass a rough "up" that is not perpendicular
     * to the view direction. */
    GLdouble ux = sy*fz - sz*fy;
    GLdouble uy = sz*fx - sx*fz;
    GLdouble uz = sx*fy - sy*fx;

    /* Column-major, as glMultMatrixf expects. */
    GLfloat m[16];
    m[0] = (GLfloat)sx;  m[4] = (GLfloat)sy;  m[8]  = (GLfloat)sz;  m[12] = 0.0f;
    m[1] = (GLfloat)ux;  m[5] = (GLfloat)uy;  m[9]  = (GLfloat)uz;  m[13] = 0.0f;
    m[2] = (GLfloat)-fx; m[6] = (GLfloat)-fy; m[10] = (GLfloat)-fz; m[14] = 0.0f;
    m[3] = 0.0f;         m[7] = 0.0f;         m[11] = 0.0f;         m[15] = 1.0f;

    glMultMatrixf(m);
    /* The rotation is applied first, then the eye is moved to the origin. */
    glTranslatef((GLfloat)-eyeX, (GLfloat)-eyeY, (GLfloat)-eyeZ);
}

void gluOrtho2D(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top) {
    glOrtho(left, right, bottom, top, -1.0, 1.0);
}

/* ============================================================================
 * Error strings
 * ==========================================================================*/

const GLubyte *gluErrorString(GLenum error) {
    switch (error) {
    case GL_NO_ERROR:          return (const GLubyte *)"no error";
    case GL_INVALID_ENUM:      return (const GLubyte *)"invalid enumerant";
    case GL_INVALID_VALUE:     return (const GLubyte *)"invalid value";
    case GL_INVALID_OPERATION: return (const GLubyte *)"invalid operation";
    case GL_STACK_OVERFLOW:    return (const GLubyte *)"stack overflow";
    case GL_STACK_UNDERFLOW:   return (const GLubyte *)"stack underflow";
    case GL_OUT_OF_MEMORY:     return (const GLubyte *)"out of memory";
    default:                   return (const GLubyte *)"unknown error";
    }
}

/* ============================================================================
 * Quadrics
 * ==========================================================================*/

struct GLUquadric {
    GLenum    draw_style;
    GLenum    normals;
    GLenum    orientation;
    GLboolean texture;
};

GLUquadric *gluNewQuadric(void) {
    GLUquadric *q = (GLUquadric *)malloc(sizeof(GLUquadric));
    if (!q) return (GLUquadric *)0;
    q->draw_style  = GLU_FILL;
    q->normals     = GLU_SMOOTH;
    q->orientation = GLU_OUTSIDE;
    q->texture     = GL_FALSE;
    return q;
}

void gluDeleteQuadric(GLUquadric *q) { free(q); }

void gluQuadricDrawStyle(GLUquadric *q, GLenum drawStyle) {
    if (q) q->draw_style = drawStyle;
}
void gluQuadricNormals(GLUquadric *q, GLenum normals) {
    if (q) q->normals = normals;
}
void gluQuadricOrientation(GLUquadric *q, GLenum orientation) {
    if (q) q->orientation = orientation;
}
void gluQuadricTexture(GLUquadric *q, GLboolean textureCoords) {
    if (q) q->texture = textureCoords;
}

/* Emit one vertex of a quadric surface, with its normal and texture
 * coordinate if the object asks for them.  `flip` reverses the normal for
 * GLU_INSIDE, so the surface lights correctly when viewed from within. */
static void quad_vertex(const GLUquadric *q,
                        GLdouble x, GLdouble y, GLdouble z,
                        GLdouble nx, GLdouble ny, GLdouble nz,
                        GLdouble s, GLdouble t, int flip) {
    if (q->normals != GLU_NONE) {
        if (flip) glNormal3f((GLfloat)-nx, (GLfloat)-ny, (GLfloat)-nz);
        else      glNormal3f((GLfloat)nx,  (GLfloat)ny,  (GLfloat)nz);
    }
    if (q->texture) glTexCoord2f((GLfloat)s, (GLfloat)t);
    glVertex3f((GLfloat)x, (GLfloat)y, (GLfloat)z);
}

/* The primitive a quadric's draw style maps to.  GLU_SILHOUETTE has no
 * meaningful outline for these shapes, so it renders as lines. */
static GLenum quad_prim(const GLUquadric *q, GLenum filled) {
    switch (q->draw_style) {
    case GLU_POINT:      return GL_POINTS;
    case GLU_LINE:
    case GLU_SILHOUETTE: return GL_LINE_STRIP;
    case GLU_FILL:
    default:             return filled;
    }
}

void gluSphere(GLUquadric *q, GLdouble radius, GLint slices, GLint stacks) {
    if (!q || slices < 2 || stacks < 2 || radius <= 0.0) return;

    int flip = (q->orientation == GLU_INSIDE);
    GLenum prim = quad_prim(q, GL_TRIANGLE_STRIP);

    /* Latitude bands from the south pole up.  Each band is a strip, which
     * keeps the vertex count at 2 per column instead of 6. */
    for (GLint st = 0; st < stacks; st++) {
        GLdouble phi0 = GLU_PI * ((GLdouble)st / stacks) - GLU_PI / 2.0;
        GLdouble phi1 = GLU_PI * ((GLdouble)(st + 1) / stacks) - GLU_PI / 2.0;
        GLdouble y0 = sin(phi0), r0 = cos(phi0);
        GLdouble y1 = sin(phi1), r1 = cos(phi1);

        glBegin(prim);
        for (GLint sl = 0; sl <= slices; sl++) {
            GLdouble theta = 2.0 * GLU_PI * ((GLdouble)sl / slices);
            GLdouble ct = cos(theta), stn = sin(theta);
            GLdouble s = (GLdouble)sl / slices;

            quad_vertex(q, radius*r0*ct, radius*y0, radius*r0*stn,
                        r0*ct, y0, r0*stn,
                        s, (GLdouble)st / stacks, flip);
            quad_vertex(q, radius*r1*ct, radius*y1, radius*r1*stn,
                        r1*ct, y1, r1*stn,
                        s, (GLdouble)(st + 1) / stacks, flip);
        }
        glEnd();
    }
}

void gluCylinder(GLUquadric *q, GLdouble base, GLdouble top, GLdouble height,
                 GLint slices, GLint stacks) {
    if (!q || slices < 2 || stacks < 1) return;

    int flip = (q->orientation == GLU_INSIDE);
    GLenum prim = quad_prim(q, GL_TRIANGLE_STRIP);

    /* The side normal tilts with the taper: for a cone it is not horizontal.
     * Deriving it from the slope is what makes a cone light correctly. */
    GLdouble dr = base - top;
    GLdouble nlen = sqrt(dr * dr + height * height);
    GLdouble nr = (nlen > 1e-12) ? (height / nlen) : 1.0;
    GLdouble ny = (nlen > 1e-12) ? (dr / nlen) : 0.0;

    for (GLint k = 0; k < stacks; k++) {
        GLdouble f0 = (GLdouble)k / stacks;
        GLdouble f1 = (GLdouble)(k + 1) / stacks;
        GLdouble r0 = base + (top - base) * f0;
        GLdouble r1 = base + (top - base) * f1;
        GLdouble z0 = height * f0, z1 = height * f1;

        glBegin(prim);
        for (GLint sl = 0; sl <= slices; sl++) {
            GLdouble theta = 2.0 * GLU_PI * ((GLdouble)sl / slices);
            GLdouble ct = cos(theta), stn = sin(theta);
            GLdouble s = (GLdouble)sl / slices;

            quad_vertex(q, r0*ct, r0*stn, z0, nr*ct, nr*stn, ny, s, f0, flip);
            quad_vertex(q, r1*ct, r1*stn, z1, nr*ct, nr*stn, ny, s, f1, flip);
        }
        glEnd();
    }
}

void gluDisk(GLUquadric *q, GLdouble inner, GLdouble outer,
             GLint slices, GLint loops) {
    if (!q || slices < 2 || loops < 1 || outer <= 0.0) return;

    int flip = (q->orientation == GLU_INSIDE);
    GLenum prim = quad_prim(q, GL_TRIANGLE_STRIP);

    for (GLint lp = 0; lp < loops; lp++) {
        GLdouble f0 = (GLdouble)lp / loops;
        GLdouble f1 = (GLdouble)(lp + 1) / loops;
        GLdouble r0 = inner + (outer - inner) * f0;
        GLdouble r1 = inner + (outer - inner) * f1;

        glBegin(prim);
        for (GLint sl = 0; sl <= slices; sl++) {
            GLdouble theta = 2.0 * GLU_PI * ((GLdouble)sl / slices);
            GLdouble ct = cos(theta), stn = sin(theta);

            /* A disk lies in z = 0 with a constant +z normal. */
            quad_vertex(q, r0*ct, r0*stn, 0.0, 0.0, 0.0, 1.0,
                        (r0*ct/outer + 1.0) * 0.5, (r0*stn/outer + 1.0) * 0.5,
                        flip);
            quad_vertex(q, r1*ct, r1*stn, 0.0, 0.0, 0.0, 1.0,
                        (r1*ct/outer + 1.0) * 0.5, (r1*stn/outer + 1.0) * 0.5,
                        flip);
        }
        glEnd();
    }
}

/* libgl/include/GL/gl.h — OpenGL 1.1 API for AuraLite OS.
 *
 * See GL_PLAN.md for the implementation roadmap.  This header declares the
 * types, enumerants and entry points of the OpenGL 1.1 fixed-function
 * pipeline.  Entry points that are not implemented yet record
 * GL_INVALID_OPERATION rather than silently doing nothing, so applications
 * can detect the gap with glGetError().
 *
 * Phase G0 provides the type system, enumerants and the math layer.
 * Phase G1 adds the context, glClear and the error machinery.
 */
#ifndef AURALITE_GL_H
#define AURALITE_GL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Types (OpenGL 1.1 §2.3) ---- */
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef void           GLvoid;
typedef signed char    GLbyte;      /* 1-byte signed    */
typedef short          GLshort;     /* 2-byte signed    */
typedef int            GLint;       /* 4-byte signed    */
typedef unsigned char  GLubyte;     /* 1-byte unsigned  */
typedef unsigned short GLushort;    /* 2-byte unsigned  */
typedef unsigned int   GLuint;      /* 4-byte unsigned  */
typedef int            GLsizei;     /* 4-byte signed    */
typedef float          GLfloat;     /* single precision */
typedef float          GLclampf;    /* single precision, clamped to [0,1] */
typedef double         GLdouble;    /* double precision */
typedef double         GLclampd;    /* double precision, clamped to [0,1] */

/* ---- Boolean values ---- */
#define GL_FALSE                          0
#define GL_TRUE                           1

/* ---- Error codes (§2.5) ---- */
#define GL_NO_ERROR                       0
#define GL_INVALID_ENUM                   0x0500
#define GL_INVALID_VALUE                  0x0501
#define GL_INVALID_OPERATION              0x0502
#define GL_STACK_OVERFLOW                 0x0503
#define GL_STACK_UNDERFLOW                0x0504
#define GL_OUT_OF_MEMORY                  0x0505

/* ---- glGetString names ---- */
#define GL_VENDOR                         0x1F00
#define GL_RENDERER                       0x1F01
#define GL_VERSION                        0x1F02
#define GL_EXTENSIONS                     0x1F03

/* ---- Buffer clear masks (§4.2.3) ---- */
#define GL_DEPTH_BUFFER_BIT               0x00000100
#define GL_STENCIL_BUFFER_BIT             0x00000400
#define GL_COLOR_BUFFER_BIT               0x00004000

/* ---- Primitive types (§2.6.1) ---- */
#define GL_POINTS                         0x0000
#define GL_LINES                          0x0001
#define GL_LINE_LOOP                      0x0002
#define GL_LINE_STRIP                     0x0003
#define GL_TRIANGLES                      0x0004
#define GL_TRIANGLE_STRIP                 0x0005
#define GL_TRIANGLE_FAN                   0x0006
#define GL_QUADS                          0x0007
#define GL_QUAD_STRIP                     0x0008
#define GL_POLYGON                        0x0009

/* ---- Matrix modes (§2.10.2) ---- */
#define GL_MATRIX_MODE                    0x0BA0
#define GL_MODELVIEW                      0x1700
#define GL_PROJECTION                     0x1701
#define GL_TEXTURE                        0x1702

/* ---- Depth buffer (§4.1.5) ---- */
#define GL_NEVER                          0x0200
#define GL_LESS                           0x0201
#define GL_EQUAL                          0x0202
#define GL_LEQUAL                         0x0203
#define GL_GREATER                        0x0204
#define GL_NOTEQUAL                       0x0205
#define GL_GEQUAL                         0x0206
#define GL_ALWAYS                         0x0207
#define GL_DEPTH_TEST                     0x0B71
#define GL_DEPTH_WRITEMASK                0x0B72
#define GL_DEPTH_FUNC                     0x0B74
#define GL_DEPTH_CLEAR_VALUE              0x0B73

/* ---- Face culling (§3.5.1) ---- */
#define GL_FRONT                          0x0404
#define GL_BACK                           0x0405
#define GL_FRONT_AND_BACK                 0x0408
#define GL_CW                             0x0900
#define GL_CCW                            0x0901
#define GL_CULL_FACE                      0x0B44
#define GL_CULL_FACE_MODE                 0x0B45
#define GL_FRONT_FACE                     0x0B46

/* ---- Shading / polygon mode (§2.14.7, §3.5.4) ---- */
#define GL_FLAT                           0x1D00
#define GL_SMOOTH                         0x1D01
#define GL_SHADE_MODEL                    0x0B54
#define GL_POINT                          0x1B00
#define GL_LINE                           0x1B01
#define GL_FILL                           0x1B02

/* ---- Common state queries ---- */
#define GL_VIEWPORT                       0x0BA2
#define GL_COLOR_CLEAR_VALUE              0x0C22
#define GL_SCISSOR_BOX                    0x0C10
#define GL_SCISSOR_TEST                   0x0C11
#define GL_MODELVIEW_MATRIX               0x0BA6
#define GL_PROJECTION_MATRIX              0x0BA7
#define GL_MAX_MODELVIEW_STACK_DEPTH      0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH     0x0D38

/* ---- Lighting (§2.14) ---- */
#define GL_LIGHTING                       0x0B50
#define GL_LIGHT0                         0x4000
#define GL_LIGHT1                         0x4001
#define GL_LIGHT2                         0x4002
#define GL_LIGHT3                         0x4003
#define GL_LIGHT4                         0x4004
#define GL_LIGHT5                         0x4005
#define GL_LIGHT6                         0x4006
#define GL_LIGHT7                         0x4007
#define GL_AMBIENT                        0x1200
#define GL_DIFFUSE                        0x1201
#define GL_SPECULAR                       0x1202
#define GL_POSITION                       0x1203
#define GL_SHININESS                      0x1601
#define GL_EMISSION                       0x1600
#define GL_NORMALIZE                      0x0BA1
#define GL_MAX_LIGHTS                     0x0D31
#define GL_LIGHT_MODEL_AMBIENT            0x0B53
#define GL_LIGHT_MODEL_TWO_SIDE           0x0B52
#define GL_AMBIENT_AND_DIFFUSE            0x1602
#define GL_COLOR_MATERIAL                 0x0B57
#define GL_SPOT_DIRECTION                 0x1204
#define GL_SPOT_EXPONENT                  0x1205
#define GL_SPOT_CUTOFF                    0x1206
#define GL_CONSTANT_ATTENUATION           0x1207
#define GL_LINEAR_ATTENUATION             0x1208
#define GL_QUADRATIC_ATTENUATION          0x1209

/* ---- Texturing (§3.8) ---- */
#define GL_TEXTURE_2D                     0x0DE1
#define GL_TEXTURE_MAG_FILTER             0x2800
#define GL_TEXTURE_MIN_FILTER             0x2801
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803
#define GL_NEAREST                        0x2600
#define GL_LINEAR                         0x2601
#define GL_REPEAT                         0x2901
#define GL_CLAMP                          0x2900
#define GL_RGB                            0x1907
#define GL_RGBA                           0x1908
#define GL_LUMINANCE                      0x1909
#define GL_UNSIGNED_BYTE                  0x1401
#define GL_UNSIGNED_SHORT                 0x1403
#define GL_UNSIGNED_INT                   0x1405
#define GL_FLOAT                          0x1406

/* ---- Blending / fog (§4.1.7) ---- */
#define GL_BLEND                          0x0BE2
#define GL_ZERO                           0
#define GL_ONE                            1
#define GL_SRC_ALPHA                      0x0302
#define GL_ONE_MINUS_SRC_ALPHA            0x0303
#define GL_DST_ALPHA                      0x0304
#define GL_ONE_MINUS_DST_ALPHA            0x0305
#define GL_ALPHA_TEST                     0x0BC0
#define GL_FOG                            0x0B60

/* ---- Vertex arrays (§2.8) ---- */
#define GL_VERTEX_ARRAY                   0x8074
#define GL_NORMAL_ARRAY                   0x8075
#define GL_COLOR_ARRAY                    0x8076
#define GL_TEXTURE_COORD_ARRAY            0x8078

/* ============================================================================
 * Entry points
 *
 * Availability by phase (GL_PLAN.md):
 *   G1: error handling, glClear family, viewport, glGetString
 *   G2: matrix stacks, immediate mode
 *   G3: depth test, culling, shade model
 *   G4: scissor, glGet*
 *   G5: lighting
 *   G6: textures, blending
 *   G7: vertex arrays
 * ==========================================================================*/

/* ---- Errors and strings (G1) ---- */
GLenum       glGetError(void);
const GLubyte *glGetString(GLenum name);

/* ---- Framebuffer clearing (G1) ---- */
void glClear(GLbitfield mask);
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void glClearDepth(GLclampd depth);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void glFlush(void);
void glFinish(void);

/* ---- Matrix stack (G2) ---- */
void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat *m);
void glMultMatrixf(const GLfloat *m);
void glPushMatrix(void);
void glPopMatrix(void);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
               GLdouble n, GLdouble f);
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
             GLdouble n, GLdouble f);

/* ---- Immediate mode (G2) ---- */
void glBegin(GLenum mode);
void glEnd(void);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w);
void glVertex2fv(const GLfloat *v);
void glVertex3fv(const GLfloat *v);
void glVertex4fv(const GLfloat *v);
void glColor3f(GLfloat r, GLfloat g, GLfloat b);
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glColor3ub(GLubyte r, GLubyte g, GLubyte b);
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);
void glColor3fv(const GLfloat *v);
void glColor4fv(const GLfloat *v);
void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz);
void glNormal3fv(const GLfloat *v);
void glTexCoord2f(GLfloat s, GLfloat t);
void glTexCoord2fv(const GLfloat *v);

/* ---- Per-fragment state (G3/G4) ---- */
void glEnable(GLenum cap);
void glDisable(GLenum cap);
GLboolean glIsEnabled(GLenum cap);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glShadeModel(GLenum mode);
void glPolygonMode(GLenum face, GLenum mode);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);

/* ---- Lighting and materials (G5) ---- */
void glLightf(GLenum light, GLenum pname, GLfloat param);
void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void glMaterialf(GLenum face, GLenum pname, GLfloat param);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
void glLightModelfv(GLenum pname, const GLfloat *params);
void glLightModeli(GLenum pname, GLint param);
void glColorMaterial(GLenum face, GLenum mode);

/* ---- Attribute stack (G4) ---- */
#define GL_CURRENT_BIT        0x00000001
#define GL_ENABLE_BIT         0x00002000
#define GL_DEPTH_BUFFER_BIT_A 0x00000100   /* same value as the clear bit */
#define GL_VIEWPORT_BIT       0x00000800
#define GL_SCISSOR_BIT        0x00080000
#define GL_POLYGON_BIT        0x00000008
#define GL_LIGHTING_BIT       0x00000040
#define GL_COLOR_BUFFER_BIT_A 0x00004000
#define GL_ALL_ATTRIB_BITS    0x000FFFFF

void glPushAttrib(GLbitfield mask);
void glPopAttrib(void);

/* ---- State queries (G4) ---- */
void glGetIntegerv(GLenum pname, GLint *params);
void glGetFloatv(GLenum pname, GLfloat *params);
void glGetBooleanv(GLenum pname, GLboolean *params);

#ifdef __cplusplus
}
#endif

#endif /* AURALITE_GL_H */

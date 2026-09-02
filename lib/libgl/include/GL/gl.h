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
typedef char           GLchar;      /* shader source and GLSL names        */
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
#define GL_INVALID_FRAMEBUFFER_OPERATION  0x0506

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

/* ---- Stencil buffer (§4.1.4) ---- */
#define GL_STENCIL_TEST                   0x0B90
#define GL_STENCIL_CLEAR_VALUE            0x0B91
#define GL_STENCIL_FUNC                   0x0B92
#define GL_STENCIL_VALUE_MASK             0x0B93
#define GL_STENCIL_FAIL                   0x0B94
#define GL_STENCIL_PASS_DEPTH_FAIL        0x0B95
#define GL_STENCIL_PASS_DEPTH_PASS        0x0B96
#define GL_STENCIL_REF                    0x0B97
#define GL_STENCIL_WRITEMASK              0x0B98
#define GL_STENCIL_BITS                   0x0D57
#define GL_KEEP                           0x1E00
#define GL_INCR                           0x1E02
#define GL_DECR                           0x1E03
#define GL_INVERT                         0x150A
#define GL_INCR_WRAP                      0x8507
#define GL_DECR_WRAP                      0x8508

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
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_TEXTURE_ENV                    0x2300
#define GL_TEXTURE_ENV_MODE               0x2200
#define GL_TEXTURE_ENV_COLOR              0x2201
#define GL_MODULATE                       0x2100
#define GL_DECAL                          0x2101
#define GL_REPLACE                        0x1E01
#define GL_TEXTURE_BINDING_2D             0x8069
#define GL_LUMINANCE_ALPHA                0x190A
#define GL_ALPHA                          0x1906

/* Blend factors (§4.1.7).  GL_ZERO/GL_ONE are already defined above. */
#define GL_SRC_COLOR                      0x0300
#define GL_ONE_MINUS_SRC_COLOR            0x0301
#define GL_DST_COLOR                      0x0306
#define GL_ONE_MINUS_DST_COLOR            0x0307
#define GL_SRC_ALPHA_SATURATE             0x0308
#define GL_BLEND_SRC                      0x0BE1
#define GL_BLEND_DST                      0x0BE0

/* Alpha test (§4.1.4). */
#define GL_ALPHA_TEST_FUNC                0x0BC1
#define GL_ALPHA_TEST_REF                 0x0BC2

/* Fog (§3.10). */
#define GL_FOG_MODE                       0x0B65
#define GL_FOG_DENSITY                    0x0B62
#define GL_FOG_START                      0x0B63
#define GL_FOG_END                        0x0B64
#define GL_FOG_COLOR                      0x0B66
#define GL_EXP                            0x0800
#define GL_EXP2                           0x0801
/* GL_LINEAR (0x2601) doubles as a fog mode, as in the specification. */

/* ---- GL 1.2 / 1.3 texturing (phase G10) ----
 *
 * Mipmapping, multitexturing, 3D textures and cube maps.  The enum values are
 * the standard ones; an application built against a real GL header must see
 * the same numbers.
 */

/* Mipmap minification filters (§3.8.8). */
#define GL_NEAREST_MIPMAP_NEAREST         0x2700
#define GL_LINEAR_MIPMAP_NEAREST          0x2701
#define GL_NEAREST_MIPMAP_LINEAR          0x2702
#define GL_LINEAR_MIPMAP_LINEAR           0x2703

/* Level-of-detail and level clamping (GL 1.2 §3.8.4). */
#define GL_TEXTURE_BASE_LEVEL             0x813C
#define GL_TEXTURE_MAX_LEVEL              0x813D
#define GL_GENERATE_MIPMAP                0x8191

/* Wrapping (GL 1.2/1.3). */
#define GL_TEXTURE_WRAP_R                 0x8072
#define GL_CLAMP_TO_BORDER                0x812D
#define GL_TEXTURE_BORDER_COLOR           0x1004

/* 3D textures (GL 1.2 §3.8.1). */
#define GL_TEXTURE_3D                     0x806F
#define GL_TEXTURE_BINDING_3D             0x806A
#define GL_PACK_ALIGNMENT                 0x0D05
#define GL_UNPACK_ALIGNMENT               0x0CF5

/* Cube maps (GL 1.3 §3.8.6).  The six face targets are CONSECUTIVE, which is
 * what lets a face index be derived by subtraction. */
#define GL_TEXTURE_CUBE_MAP               0x8513
#define GL_TEXTURE_BINDING_CUBE_MAP       0x8514
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X    0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_X    0x8516
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Y    0x8517
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Y    0x8518
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Z    0x8519
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z    0x851A
#define GL_MAX_CUBE_MAP_TEXTURE_SIZE      0x851C

/* Multitexturing (GL 1.3 §3.8.10).  GL_TEXTUREi are consecutive too. */
#define GL_TEXTURE0                       0x84C0
#define GL_TEXTURE1                       0x84C1
#define GL_TEXTURE2                       0x84C2
#define GL_TEXTURE3                       0x84C3
#define GL_ACTIVE_TEXTURE                 0x84E0
#define GL_CLIENT_ACTIVE_TEXTURE          0x84E1
#define GL_MAX_TEXTURE_UNITS              0x84E2

/* ---- Framebuffer objects (GL 3.0 / EXT_framebuffer_object, phase G12) ----
 *
 * The GL 3.0 core names are used, not the EXT-suffixed ones: an application
 * written against modern GL sees the names it expects, and the values are the
 * same either way.
 */
#define GL_FRAMEBUFFER                    0x8D40
#define GL_RENDERBUFFER                   0x8D41
#define GL_FRAMEBUFFER_BINDING            0x8CA6
#define GL_RENDERBUFFER_BINDING           0x8CA7
#define GL_READ_FRAMEBUFFER               0x8CA8
#define GL_DRAW_FRAMEBUFFER               0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING       0x8CA6  /* alias of FRAMEBUFFER_BINDING */
#define GL_READ_FRAMEBUFFER_BINDING       0x8CAA

/* Attachment points.  GL_COLOR_ATTACHMENTi are consecutive. */
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_DEPTH_ATTACHMENT               0x8D00
#define GL_STENCIL_ATTACHMENT             0x8D20

/* glCheckFramebufferStatus return values (§4.4.4). */
#define GL_FRAMEBUFFER_COMPLETE                      0x8CD5
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT         0x8CD6
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7
#define GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS         0x8CD9
#define GL_FRAMEBUFFER_UNSUPPORTED                   0x8CDD
#define GL_FRAMEBUFFER_UNDEFINED                     0x8219

/* Renderbuffer internal formats this implementation understands. */
#define GL_RGBA8                          0x8058
#define GL_RGB8                           0x8051
#define GL_DEPTH_COMPONENT                0x1902
#define GL_DEPTH_COMPONENT16              0x81A5
#define GL_DEPTH_COMPONENT24              0x81A6
#define GL_DEPTH_COMPONENT32F             0x8CAC
#define GL_STENCIL_INDEX                  0x1901
#define GL_STENCIL_INDEX8                 0x8D48

/* Renderbuffer queries. */
#define GL_RENDERBUFFER_WIDTH             0x8D42
#define GL_RENDERBUFFER_HEIGHT            0x8D43
#define GL_RENDERBUFFER_INTERNAL_FORMAT   0x8D44

/* Implementation limits. */
#define GL_MAX_RENDERBUFFER_SIZE          0x84E8
#define GL_MAX_COLOR_ATTACHMENTS          0x8CDF

/* glReadPixels formats.  GL_RGB/GL_RGBA/GL_ALPHA are already defined above. */
#define GL_BGR                            0x80E0
#define GL_BGRA                           0x80E1

/* ---- Shaders and programs (GL ES 2.0, phase G11c) ---- */
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_VALIDATE_STATUS                0x8B83
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_SHADER_SOURCE_LENGTH           0x8B88
#define GL_SHADER_TYPE                    0x8B4F
#define GL_DELETE_STATUS                  0x8B80
#define GL_ATTACHED_SHADERS               0x8B85
#define GL_ACTIVE_UNIFORMS                0x8B86
#define GL_ACTIVE_ATTRIBUTES              0x8B89
#define GL_CURRENT_PROGRAM                0x8B8D

#define GL_MAX_VERTEX_ATTRIBS             0x8869
#define GL_MAX_VARYING_VECTORS            0x8DFC
#define GL_MAX_VERTEX_UNIFORM_VECTORS     0x8DFB
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS   0x8DFD

/* Uniform and attribute types, as glGetActiveUniform reports them. */
#define GL_FLOAT_VEC2                     0x8B50
#define GL_FLOAT_VEC3                     0x8B51
#define GL_FLOAT_VEC4                     0x8B52
#define GL_INT_VEC2                       0x8B53
#define GL_INT_VEC3                       0x8B54
#define GL_INT_VEC4                       0x8B55
#define GL_BOOL                           0x8B56
#define GL_BOOL_VEC2                      0x8B57
#define GL_BOOL_VEC3                      0x8B58
#define GL_BOOL_VEC4                      0x8B59
#define GL_FLOAT_MAT2                     0x8B5A
#define GL_FLOAT_MAT3                     0x8B5B
#define GL_FLOAT_MAT4                     0x8B5C
#define GL_SAMPLER_2D                     0x8B5E
#define GL_SAMPLER_CUBE                   0x8B60

/* ---- Vertex arrays (§2.8) ---- */
#define GL_VERTEX_ARRAY                   0x8074
#define GL_NORMAL_ARRAY                   0x8075
#define GL_COLOR_ARRAY                    0x8076
#define GL_TEXTURE_COORD_ARRAY            0x8078

/* Buffer objects (GL 1.5 subset). */
#define GL_ARRAY_BUFFER                   0x8892
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#define GL_STREAM_DRAW                    0x88E0
#define GL_STATIC_DRAW                    0x88E4
#define GL_DYNAMIC_DRAW                   0x88E8
#define GL_ARRAY_BUFFER_BINDING           0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING   0x8895

/* Display lists (§5.4). */
#define GL_COMPILE                        0x1300
#define GL_COMPILE_AND_EXECUTE            0x1301
#define GL_LIST_BASE                      0x0B32

/* Index types accepted by glDrawElements. */
#define GL_BYTE                           0x1400
#define GL_SHORT                          0x1402
#define GL_INT                            0x1404
#define GL_DOUBLE                         0x140A

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
void glClearStencil(GLint s);
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
void glStencilFunc(GLenum func, GLint ref, GLuint mask);
void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass);
void glStencilMask(GLuint mask);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glShadeModel(GLenum mode);
void glPolygonMode(GLenum face, GLenum mode);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);

/* ---- Vertex arrays, buffer objects and display lists (G7) ---- */
typedef long GLintptr;      /* large enough for a byte offset on LP64      */
typedef long GLsizeiptr;

void glEnableClientState(GLenum array);
void glDisableClientState(GLenum array);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
void glArrayElement(GLint i);

void glGenBuffers(GLsizei n, GLuint *buffers);
void glDeleteBuffers(GLsizei n, const GLuint *buffers);
void glBindBuffer(GLenum target, GLuint buffer);
GLboolean glIsBuffer(GLuint buffer);
void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data, GLenum usage);
void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                     const GLvoid *data);

GLuint glGenLists(GLsizei range);
void glNewList(GLuint list, GLenum mode);
void glEndList(void);
void glCallList(GLuint list);
void glDeleteLists(GLuint list, GLsizei range);
GLboolean glIsList(GLuint list);

/* ---- Textures, blending and fog (G6) ---- */
void glGenTextures(GLsizei n, GLuint *textures);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glBindTexture(GLenum target, GLuint texture);
GLboolean glIsTexture(GLuint texture);
void glTexImage2D(GLenum target, GLint level, GLint internalFormat,
                  GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format,
                     GLenum type, const GLvoid *pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params);
void glTexEnvi(GLenum target, GLenum pname, GLint param);
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params);

/* ---- GL 1.2 / 1.3 texturing (G10) ---- */
void glTexImage3D(GLenum target, GLint level, GLint internalFormat,
                  GLsizei width, GLsizei height, GLsizei depth, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels);
void glActiveTexture(GLenum texture);
void glClientActiveTexture(GLenum texture);
void glMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t);
void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r);
void glGenerateMipmap(GLenum target);

/* ---- Framebuffer objects and pixel readback (G12) ---- */
void glGenFramebuffers(GLsizei n, GLuint *framebuffers);
void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers);
void glBindFramebuffer(GLenum target, GLuint framebuffer);
GLboolean glIsFramebuffer(GLuint framebuffer);
GLenum glCheckFramebufferStatus(GLenum target);
void glFramebufferTexture2D(GLenum target, GLenum attachment,
                            GLenum textarget, GLuint texture, GLint level);
void glFramebufferRenderbuffer(GLenum target, GLenum attachment,
                               GLenum renderbuffertarget, GLuint renderbuffer);

void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers);
void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers);
void glBindRenderbuffer(GLenum target, GLuint renderbuffer);
GLboolean glIsRenderbuffer(GLuint renderbuffer);
void glRenderbufferStorage(GLenum target, GLenum internalformat,
                           GLsizei width, GLsizei height);
void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params);

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, GLvoid *pixels);

/* Copies (GL2 L2).  Colour only for the TexImage entry points; blit also
 * moves depth (and stencil, when both sides have a plane). */
void glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat,
                      GLint x, GLint y, GLsizei width, GLsizei height,
                      GLint border);
void glCopyTexSubImage2D(GLenum target, GLint level,
                         GLint xoffset, GLint yoffset,
                         GLint x, GLint y, GLsizei width, GLsizei height);
void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                       GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                       GLbitfield mask, GLenum filter);

/* ---- Shaders and programs (G11c) ---- */
GLuint glCreateShader(GLenum type);
void   glDeleteShader(GLuint shader);
GLboolean glIsShader(GLuint shader);
void   glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                      const GLint *length);
void   glCompileShader(GLuint shader);
void   glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
void   glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length,
                          GLchar *infoLog);

GLuint glCreateProgram(void);
void   glDeleteProgram(GLuint program);
GLboolean glIsProgram(GLuint program);
void   glAttachShader(GLuint program, GLuint shader);
void   glDetachShader(GLuint program, GLuint shader);
void   glLinkProgram(GLuint program);
void   glUseProgram(GLuint program);
void   glValidateProgram(GLuint program);
void   glGetProgramiv(GLuint program, GLenum pname, GLint *params);
void   glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length,
                           GLchar *infoLog);

/* ---- Generic vertex attributes ---- */
void   glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                             GLboolean normalized, GLsizei stride,
                             const GLvoid *pointer);
void   glEnableVertexAttribArray(GLuint index);
void   glDisableVertexAttribArray(GLuint index);
void   glBindAttribLocation(GLuint program, GLuint index, const GLchar *name);
GLint  glGetAttribLocation(GLuint program, const GLchar *name);
void   glVertexAttrib1f(GLuint index, GLfloat x);
void   glVertexAttrib2f(GLuint index, GLfloat x, GLfloat y);
void   glVertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z);
void   glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z,
                        GLfloat w);

/* ---- Uniforms ---- */
GLint  glGetUniformLocation(GLuint program, const GLchar *name);
void   glUniform1f(GLint location, GLfloat v0);
void   glUniform2f(GLint location, GLfloat v0, GLfloat v1);
void   glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
void   glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2,
                   GLfloat v3);
void   glUniform1i(GLint location, GLint v0);
void   glUniform1fv(GLint location, GLsizei count, const GLfloat *value);
void   glUniform2fv(GLint location, GLsizei count, const GLfloat *value);
void   glUniform3fv(GLint location, GLsizei count, const GLfloat *value);
void   glUniform4fv(GLint location, GLsizei count, const GLfloat *value);
void   glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *value);
void   glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *value);
void   glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
                          const GLfloat *value);

void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glAlphaFunc(GLenum func, GLclampf ref);

void glFogi(GLenum pname, GLint param);
void glFogf(GLenum pname, GLfloat param);
void glFogfv(GLenum pname, const GLfloat *params);

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

/*
 * test_glfbo.c — host-side unit tests for framebuffer objects, renderbuffers,
 * glReadPixels (phase G12) and the copy/blit entry points (GL2 L2).
 *
 * The central claim of this phase is that rendering into a texture and then
 * sampling that texture gives back what was drawn.  The round-trip test is
 * written to check exactly that, pixel for pixel, rather than checking that
 * the API calls return success — an FBO implementation that quietly rendered
 * into the window would pass the latter.
 *
 * Completeness is tested by constructing each incomplete configuration on
 * purpose and asserting the specific status code, because "returns something
 * that is not COMPLETE" is not useful to an application trying to work out
 * what it got wrong.
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "GL/gl.h"
#include "GL/glu.h"
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

#define W 64
#define H 64

/* Window-buffer pixel, for checks that the window was or was not touched. */
static uint32_t wpx(aglx_context_t *c, int x, int y) {
    const uint32_t *b = aglxGetColorBuffer(c);
    return b[(size_t)(H - 1 - y) * W + x];
}

static int near_u8(int a, int b, int tol) {
    int d = a - b; if (d < 0) d = -d; return d <= tol;
}

static aglx_context_t *setup(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    if (!c) return NULL;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, 0, H, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glColor3f(1, 1, 1);
    return c;
}

/* Set up an orthographic projection matching an off-screen target of size n. */
static void ortho_for(int n) {
    glViewport(0, 0, n, n);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, n, 0, n, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
}

static void solid_quad(float x0, float y0, float x1, float y1) {
    glBegin(GL_QUADS);
    glVertex3f(x0, y0, 0); glVertex3f(x1, y0, 0);
    glVertex3f(x1, y1, 0); glVertex3f(x0, y1, 0);
    glEnd();
}

/* Create an empty RGBA texture of size n, suitable as a colour attachment. */
static GLuint make_target_texture(int n) {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, n, n, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return id;
}

/* ============================================================================
 * Object management
 * ==========================================================================*/

static int t_gen_and_delete(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb[3] = { 0, 0, 0 };
    glGenFramebuffers(3, fb);
    int ok = fb[0] && fb[1] && fb[2] &&
             fb[0] != fb[1] && fb[1] != fb[2];
    ok = ok && glIsFramebuffer(fb[0]) == GL_TRUE;
    ok = ok && glIsFramebuffer(9999)  == GL_FALSE;

    glDeleteFramebuffers(3, fb);
    ok = ok && glIsFramebuffer(fb[0]) == GL_FALSE;
    ok = ok && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_bind_reports_binding(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb = 0;
    glGenFramebuffers(1, &fb);

    GLint v = -1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &v);
    int ok = v == 0;

    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &v);
    ok = ok && v == (GLint)fb;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &v);
    ok = ok && v == 0;
    aglxDestroyContext(c);
    return ok;
}

/* Deleting the bound framebuffer must revert to the window, not leave the
 * rasterizer pointing at a freed object. */
static int t_delete_bound_reverts(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb = 0, tex = make_target_texture(16);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    glDeleteFramebuffers(1, &fb);

    GLint v = -1;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &v);
    int ok = v == 0;

    /* And the window must still be drawable. */
    ortho_for(W);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ok = ok && wpx(c, 32, 32) == 0x0000FF00u && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_bind_unused_name_creates(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    /* GL creates the object on first bind of an unused name. */
    glBindFramebuffer(GL_FRAMEBUFFER, 42);
    int ok = glIsFramebuffer(42) == GL_TRUE && glGetError() == GL_NO_ERROR;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

static int t_bad_target_rejected(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    while (glGetError() != GL_NO_ERROR) { }
    glBindFramebuffer(GL_TEXTURE_2D, 0);
    int ok = glGetError() == GL_INVALID_ENUM;
    glBindRenderbuffer(GL_FRAMEBUFFER, 0);
    ok = ok && glGetError() == GL_INVALID_ENUM;
    glCheckFramebufferStatus(GL_RENDERBUFFER);
    ok = ok && glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

/* ============================================================================
 * Completeness
 * ==========================================================================*/

/* An FBO with nothing attached is incomplete — this is the mistake that would
 * otherwise silently render into the window. */
static int t_status_missing_attachment(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb = 0;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER)
             == GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

static int t_status_complete(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb = 0, tex = make_target_texture(32);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE
             && glGetError() == GL_NO_ERROR;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

/* Attaching a texture level that was never uploaded is an unusable
 * attachment, not a missing one. */
static int t_status_unallocated_level(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb = 0, tex = make_target_texture(32);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 4);   /* no mipmaps built */
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER)
             == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

/* Colour and depth attachments of different sizes must be diagnosed, not
 * rendered into with one of the two sizes silently winning. */
static int t_status_dimension_mismatch(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb = 0, tex = make_target_texture(32), rb = 0;
    glGenFramebuffers(1, &fb);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 16, 16);

    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, rb);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER)
             == GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS;

    /* Fixing the size must make it complete. */
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 32, 32);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER)
               == GL_FRAMEBUFFER_COMPLETE;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

/* A colour renderbuffer attached as depth is a type error. */
static int t_status_wrong_attachment_type(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb = 0, tex = make_target_texture(32), rb = 0;
    glGenFramebuffers(1, &fb);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 32, 32);

    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, rb);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER)
             == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

/* Deleting an attached texture must make the FBO incomplete rather than leave
 * a dangling reference — this is the use-after-free guard. */
static int t_deleted_texture_incompletes(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb = 0, tex = make_target_texture(32);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glDeleteTextures(1, &tex);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER)
               == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

/* Drawing into an incomplete framebuffer must be refused, not crash. */
static int t_incomplete_refuses_draw(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb = 0;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    while (glGetError() != GL_NO_ERROR) { }

    glClear(GL_COLOR_BUFFER_BIT);
    int ok = glGetError() == GL_INVALID_FRAMEBUFFER_OPERATION;

    glBegin(GL_TRIANGLES);
    ok = ok && glGetError() == GL_INVALID_FRAMEBUFFER_OPERATION;
    /* glBegin was refused, so glVertex must report that it is outside a
     * begin/end pair rather than writing anywhere. */
    glVertex3f(0, 0, 0);
    ok = ok && glGetError() == GL_INVALID_OPERATION;
    glEnd();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

/* GL2 L1: GL_STENCIL_ATTACHMENT is a real slot.  A colour renderbuffer
 * attached as stencil is accepted, then diagnosed as incomplete (wrong
 * type) rather than INVALID_OPERATION. */
static int t_stencil_attachment_accepted(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb = 0, rb = 0;
    glGenFramebuffers(1, &fb);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 8, 8);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    while (glGetError() != GL_NO_ERROR) { }

    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, rb);
    int ok = glGetError() == GL_NO_ERROR
          && glCheckFramebufferStatus(GL_FRAMEBUFFER)
             == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

/* Attaching to framebuffer 0 is meaningless and must be refused. */
static int t_attach_to_default_refused(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint tex = make_target_texture(8);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    while (glGetError() != GL_NO_ERROR) { }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    int ok = glGetError() == GL_INVALID_OPERATION;
    aglxDestroyContext(c);
    return ok;
}

/* ============================================================================
 * Renderbuffers
 * ==========================================================================*/

static int t_renderbuffer_storage_and_query(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint rb = 0;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 40, 24);

    GLint v = 0;
    int ok = glGetError() == GL_NO_ERROR;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &v);
    ok = ok && v == 40;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &v);
    ok = ok && v == 24;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER,
                                 GL_RENDERBUFFER_INTERNAL_FORMAT, &v);
    ok = ok && v == GL_DEPTH_COMPONENT24;

    glGetIntegerv(GL_RENDERBUFFER_BINDING, &v);
    ok = ok && v == (GLint)rb;
    aglxDestroyContext(c);
    return ok;
}

static int t_renderbuffer_bad_format(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint rb = 0;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    while (glGetError() != GL_NO_ERROR) { }
    glRenderbufferStorage(GL_RENDERBUFFER, GL_TRIANGLES, 8, 8);
    int ok = glGetError() == GL_INVALID_ENUM;

    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, -1, 8);
    ok = ok && glGetError() == GL_INVALID_VALUE;

    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, AGLX_MAX_DIM + 1, 8);
    ok = ok && glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

/* Deleting a renderbuffer must detach it everywhere. */
static int t_delete_renderbuffer_detaches(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb = 0, tex = make_target_texture(16), rb = 0;
    glGenFramebuffers(1, &fb);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 16, 16);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, rb);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glDeleteRenderbuffers(1, &rb);
    /* The depth attachment is gone, but the colour one remains, so the FBO is
     * still complete — a colour-only framebuffer is legal. */
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    ok = ok && c->framebuffers[0].depth.kind == GL_ATTACH_NONE;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

/* ============================================================================
 * Rendering into an FBO
 * ==========================================================================*/

/* The headline test: render into a texture, bind it, draw it to the window,
 * and check the sampled colours are what was rendered. */
static int t_render_to_texture_roundtrip(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 16;
    GLuint fb = 0, tex = make_target_texture(N);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        aglxDestroyContext(c);
        return 0;
    }

    /* Paint the off-screen target: red background, blue in the bottom-left
     * quadrant, so both the clear and the draw path are exercised. */
    ortho_for(N);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(0, 0, 1);
    solid_quad(0, 0, N / 2, N / 2);

    /* Back to the window; sample the texture over the whole surface. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ortho_for(W);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(1, 1, 1);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(0, 0, 0);
    glTexCoord2f(1, 0); glVertex3f(W, 0, 0);
    glTexCoord2f(1, 1); glVertex3f(W, H, 0);
    glTexCoord2f(0, 1); glVertex3f(0, H, 0);
    glEnd();

    /* Bottom-left quarter of the window shows the blue quadrant; everywhere
     * else shows the red clear. */
    int ok = wpx(c, 8,  8)  == 0x000000FFu &&
             wpx(c, 48, 8)  == 0x00FF0000u &&
             wpx(c, 8,  48) == 0x00FF0000u &&
             wpx(c, 48, 48) == 0x00FF0000u &&
             glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Rendering into an FBO must leave the window untouched. */
static int t_fbo_does_not_touch_window(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 16;

    /* Paint the window green first. */
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint fb = 0, tex = make_target_texture(N);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    ortho_for(N);
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    solid_quad(0, 0, N, N);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* The window buffer must still be entirely green. */
    int ok = 1;
    for (int y = 0; ok && y < H; y += 7) {
        for (int x = 0; x < W; x += 7) {
            if (wpx(c, x, y) != 0x0000FF00u) { ok = 0; break; }
        }
    }
    aglxDestroyContext(c);
    return ok;
}

/* Depth testing must work inside an FBO with a depth renderbuffer, and the
 * window's own depth buffer must not be disturbed. */
static int t_fbo_depth_renderbuffer(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 16;
    GLuint fb = 0, tex = make_target_texture(N), rb = 0;
    glGenFramebuffers(1, &fb);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, N, N);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        aglxDestroyContext(c);
        return 0;
    }

    ortho_for(N);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    /* Near red quad, then a far blue one over it: the blue must be rejected.
     * glOrtho negates z, so +z is nearer. */
    glColor3f(1, 0, 0);
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 5); glVertex3f(N, 0, 5);
    glVertex3f(N, N, 5); glVertex3f(0, N, 5);
    glEnd();
    glColor3f(0, 0, 1);
    glBegin(GL_QUADS);
    glVertex3f(0, 0, -5); glVertex3f(N, 0, -5);
    glVertex3f(N, N, -5); glVertex3f(0, N, -5);
    glEnd();

    /* Read the FBO's colour back directly. */
    unsigned char rgb[3];
    glReadPixels(N / 2, N / 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    int ok = rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 0;

    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

/* An FBO with a colour attachment but no depth attachment has no depth
 * buffer: the depth test must silently do nothing rather than write through
 * the window's depth buffer. */
static int t_fbo_without_depth_has_none(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 16;
    GLuint fb = 0, tex = make_target_texture(N);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);

    int ok = c->depth == NULL;

    ortho_for(N);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    /* With no depth buffer both quads draw, so the later one wins. */
    glColor3f(1, 0, 0);
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 5); glVertex3f(N, 0, 5);
    glVertex3f(N, N, 5); glVertex3f(0, N, 5);
    glEnd();
    glColor3f(0, 0, 1);
    glBegin(GL_QUADS);
    glVertex3f(0, 0, -5); glVertex3f(N, 0, -5);
    glVertex3f(N, N, -5); glVertex3f(0, N, -5);
    glEnd();

    unsigned char rgb[3];
    glReadPixels(N / 2, N / 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    ok = ok && rgb[2] == 255 && rgb[0] == 0;

    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* The window's depth buffer must still be present and untouched. */
    ok = ok && c->depth != NULL && c->depth == c->win_depth;
    aglxDestroyContext(c);
    return ok;
}

/* Binding an FBO changes the effective size, which the rasterizer's clipping
 * must follow — a 16x16 target must not accept writes at x=40. */
static int t_fbo_size_is_attachment_size(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 16;
    GLuint fb = 0, tex = make_target_texture(N);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);

    int ok = c->width == N && c->height == N;

    /* Draw far outside the attachment; nothing must be written and nothing
     * must be corrupted. */
    ortho_for(W);              /* deliberately the WINDOW's projection */
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1, 1, 1);
    solid_quad(40, 40, 60, 60);

    unsigned char rgb[3];
    glReadPixels(8, 8, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    ok = ok && rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0;
    ok = ok && glGetError() == GL_NO_ERROR;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ok = ok && c->width == W && c->height == H;
    aglxDestroyContext(c);
    return ok;
}

/* Rendering into one mipmap level of a texture must leave the others alone. */
static int t_render_to_mipmap_level(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glGenerateMipmap(GL_TEXTURE_2D);

    GLuint fb = 0;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 2);   /* the 4x4 level */
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    ok = ok && c->width == 4 && c->height == 4;

    ortho_for(4);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Level 2 is green; level 0 is still the black it was uploaded as. */
    gl_texture_t *t = NULL;
    for (int i = 0; i < GL_MAX_TEXTURES_IMPL; i++) {
        if (c->textures[i].used && c->textures[i].name == tex) {
            t = &c->textures[i];
        }
    }
    ok = ok && t && (t->img[0][2].texels[0] & 0x00FFFFFFu) == 0x0000FF00u;
    ok = ok && (t->img[0][0].texels[0] & 0x00FFFFFFu) == 0x00000000u;
    aglxDestroyContext(c);
    return ok;
}

/* A texture rendered into must sample as opaque, not as alpha zero — the
 * rasterizer writes 0x00RRGGBB and the sampler reads 0xAARRGGBB. */
static int t_rendered_texture_is_opaque(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 8;
    GLuint fb = 0, tex = make_target_texture(N);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    ortho_for(N);
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Sample it with GL_MODULATE, which multiplies by texture alpha.  If the
     * alpha bytes were left at zero the result would be black. */
    ortho_for(W);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor4f(1, 0, 0, 1);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(0, 0, 0);
    glTexCoord2f(1, 0); glVertex3f(W, 0, 0);
    glTexCoord2f(1, 1); glVertex3f(W, H, 0);
    glTexCoord2f(0, 1); glVertex3f(0, H, 0);
    glEnd();
    glDisable(GL_BLEND);

    int ok = wpx(c, 32, 32) == 0x00FF0000u;
    aglxDestroyContext(c);
    return ok;
}

/* ============================================================================
 * glReadPixels
 * ==========================================================================*/

static int t_readpixels_rgb(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearColor(0.2f, 0.4f, 0.6f, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    unsigned char buf[4 * 3];
    glReadPixels(10, 10, 2, 2, GL_RGB, GL_UNSIGNED_BYTE, buf);
    int ok = glGetError() == GL_NO_ERROR;
    for (int i = 0; ok && i < 4; i++) {
        ok = near_u8(buf[i * 3 + 0], 51,  2) &&
             near_u8(buf[i * 3 + 1], 102, 2) &&
             near_u8(buf[i * 3 + 2], 153, 2);
    }
    aglxDestroyContext(c);
    return ok;
}

/* Rows come back bottom-first, matching glTexImage2D's expectation, so a
 * read-then-upload round trip needs no flip. */
static int t_readpixels_row_order(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    /* A red band along the BOTTOM of the window. */
    glColor3f(1, 0, 0);
    solid_quad(0, 0, W, 8);

    unsigned char buf[2 * 3];
    /* Read a 1x2 column spanning y=4 (inside the band) and y=20 (outside). */
    glReadPixels(32, 4, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, buf);
    glReadPixels(32, 20, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, buf + 3);
    int ok = buf[0] == 255 && buf[3] == 0;

    /* And a two-row read starting at the boundary must have the lower row
     * first. */
    unsigned char two[2 * 3];
    glReadPixels(32, 7, 1, 2, GL_RGB, GL_UNSIGNED_BYTE, two);
    ok = ok && two[0] == 255 && two[3] == 0;
    aglxDestroyContext(c);
    return ok;
}

static int t_readpixels_formats(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearColor(1, 0, 0, 1);          /* pure red */
    glClear(GL_COLOR_BUFFER_BIT);

    unsigned char rgba[4], bgr[3], alpha[1];
    glReadPixels(5, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    int ok = rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255;

    glReadPixels(5, 5, 1, 1, GL_BGR, GL_UNSIGNED_BYTE, bgr);
    ok = ok && bgr[0] == 0 && bgr[1] == 0 && bgr[2] == 255;

    glReadPixels(5, 5, 1, 1, GL_ALPHA, GL_UNSIGNED_BYTE, alpha);
    ok = ok && alpha[0] == 255;
    ok = ok && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Depth readback, which is how a test can verify a depth attachment. */
static int t_readpixels_depth(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    unsigned char d = 0;
    glReadPixels(5, 5, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, &d);
    int ok = d == 255;                 /* cleared to the far plane */

    glEnable(GL_DEPTH_TEST);
    glColor3f(1, 1, 1);
    solid_quad(0, 0, W, H);            /* at z=0, which is mid-depth */
    glReadPixels(5, 5, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, &d);
    ok = ok && near_u8(d, 128, 4);
    glDisable(GL_DEPTH_TEST);
    ok = ok && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Reads outside the framebuffer return zero rather than reading out of
 * bounds — the values are undefined in GL, and defined-as-zero is safe. */
static int t_readpixels_out_of_bounds(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    unsigned char buf[4 * 3];
    memset(buf, 0xAA, sizeof buf);
    /* A 2x2 read straddling the right edge: two pixels in, two out. */
    glReadPixels(W - 1, 10, 2, 2, GL_RGB, GL_UNSIGNED_BYTE, buf);
    int ok = buf[0] == 255 && buf[3] == 0;   /* in-bounds, then out */
    ok = ok && glGetError() == GL_NO_ERROR;

    /* Wholly outside is all zero and still not a crash. */
    glReadPixels(1000, 1000, 2, 2, GL_RGB, GL_UNSIGNED_BYTE, buf);
    ok = ok && buf[0] == 0 && buf[11] == 0;
    aglxDestroyContext(c);
    return ok;
}

static int t_readpixels_validation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    unsigned char buf[16];
    while (glGetError() != GL_NO_ERROR) { }

    glReadPixels(0, 0, -1, 1, GL_RGB, GL_UNSIGNED_BYTE, buf);
    int ok = glGetError() == GL_INVALID_VALUE;

    glReadPixels(0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    ok = ok && glGetError() == GL_INVALID_VALUE;

    glReadPixels(0, 0, 1, 1, GL_RGB, GL_FLOAT, buf);
    ok = ok && glGetError() == GL_INVALID_ENUM;

    glReadPixels(0, 0, 1, 1, GL_TRIANGLES, GL_UNSIGNED_BYTE, buf);
    ok = ok && glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

/* glReadPixels from an FBO reads the FBO, not the window — this is why the
 * two features belong in one phase. */
static int t_readpixels_reads_fbo(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 8;
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);      /* window is green */

    GLuint fb = 0, tex = make_target_texture(N);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    ortho_for(N);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);      /* FBO is blue */

    unsigned char rgb[3];
    glReadPixels(2, 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    int ok = rgb[2] == 255 && rgb[1] == 0;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(2, 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    ok = ok && rgb[1] == 255 && rgb[2] == 0;
    aglxDestroyContext(c);
    return ok;
}

/* ============================================================================
 * Interplay and regressions
 * ==========================================================================*/

/* aglxSwapBuffers must present the WINDOW even while an FBO is bound. */
static int t_swap_presents_window(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint fb = 0, tex = make_target_texture(8);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    ortho_for(8);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    ag_stub_reset();
    int ok = aglxSwapBuffers(c) == 0;
    /* The stub records the blit; it must have carried the window's size and
     * the window's green pixels. */
    ok = ok && ag_stub.calls == 1;
    ok = ok && ag_stub.last_w == (uint32_t)W;
    ok = ok && ag_stub.last_h == (uint32_t)H;
    /* And the pixels handed over were the window's green, not the FBO's red. */
    ok = ok && ag_stub.last_first_pixel == 0x0000FF00u;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ok = ok && wpx(c, 32, 32) == 0x0000FF00u;
    aglxDestroyContext(c);
    return ok;
}

/* Resizing the window while an FBO is bound must not move the render target. */
static int t_resize_while_fbo_bound(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 16;
    GLuint fb = 0, tex = make_target_texture(N);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);

    int ok = aglxResize(c, 32, 32) == 0;
    /* Still rendering into the 16x16 texture, not the new 32x32 window. */
    ok = ok && c->width == N && c->height == N;
    ok = ok && c->win_width == 32 && c->win_height == 32;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ok = ok && c->width == 32 && c->height == 32;
    ok = ok && c->color == c->win_color;
    aglxDestroyContext(c);
    return ok;
}

/* Destroying a context with FBOs and renderbuffers alive must free them.
 * Under a leak checker this catches a missed free in the new storage. */
static int t_destroy_frees_renderbuffers(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    for (int i = 0; i < 4; i++) {
        GLuint rb = 0;
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 32, 32);
        GLuint rbd = 0;
        glGenRenderbuffers(1, &rbd);
        glBindRenderbuffer(GL_RENDERBUFFER, rbd);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 32, 32);
    }
    aglxDestroyContext(c);
    return aglxGetCurrentContext() == NULL;
}

/* Nothing in phase G12 may change how plain window rendering behaves.  This
 * is the regression gate for the target-redirection change. */
static int t_window_rendering_unchanged(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    glColor3f(1, 0, 0);
    glBegin(GL_QUADS);
    glVertex3f(8, 8, 5); glVertex3f(56, 8, 5);
    glVertex3f(56, 56, 5); glVertex3f(8, 56, 5);
    glEnd();
    glColor3f(0, 0, 1);
    glBegin(GL_QUADS);
    glVertex3f(8, 8, -5); glVertex3f(56, 8, -5);
    glVertex3f(56, 56, -5); glVertex3f(8, 56, -5);
    glEnd();

    int ok = wpx(c, 32, 32) == 0x00FF0000u &&   /* near red wins */
             wpx(c, 2, 2)   == 0x00000000u &&   /* outside stays clear */
             glGetError() == GL_NO_ERROR;
    glDisable(GL_DEPTH_TEST);
    aglxDestroyContext(c);
    return ok;
}

static int t_query_limits(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLint v = 0;
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &v);
    int ok = v == GL_MAX_COLOR_ATTACHMENTS_IMPL && v >= 1;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &v);
    ok = ok && v == AGLX_MAX_DIM;
    ok = ok && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* ============================================================================
 * Copies (GL2 L2)
 *
 * The definition of done is that a glReadPixels of a blitted (or copied)
 * region matches a glReadPixels of the source.  Sampling after CopyTex is
 * the Y-flip check: window storage is top-down, textures are bottom-up, and
 * a missed flip puts the blue quadrant on the wrong side of the window.
 * ==========================================================================*/

static GLuint make_color_fbo(GLuint *tex_out, int n) {
    GLuint fb = 0, tex = make_target_texture(n);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    if (tex_out) *tex_out = tex;
    return fb;
}

/* Paint the current draw target: red background, blue bottom-left quadrant. */
static void paint_red_blue(int n) {
    ortho_for(n);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(0, 0, 1);
    solid_quad(0, 0, n / 2, n / 2);
}

static int rgb_eq(const unsigned char *p, int r, int g, int b) {
    return p[0] == r && p[1] == g && p[2] == b;
}

/* Window → texture via CopyTexImage2D, then sample.  Also byte-compares
 * glReadPixels of the window against glReadPixels of the resulting texture. */
static int t_copytex_window_roundtrip(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    paint_red_blue(W);

    unsigned char src[4 * 3];
    glReadPixels(W / 4, H / 4, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, src);
    glReadPixels(W * 3 / 4, H * 3 / 4, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, src + 3);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, W, H, 0);
    int ok = glGetError() == GL_NO_ERROR;

    /* Read the new texture back through an FBO: must match the window read. */
    GLuint fb = 0;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    unsigned char dst[4 * 3];
    glReadPixels(W / 4, H / 4, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, dst);
    glReadPixels(W * 3 / 4, H * 3 / 4, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, dst + 3);
    ok = ok && memcmp(src, dst, 6) == 0;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ortho_for(W);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(0, 0, 0);
    glTexCoord2f(1, 0); glVertex3f(W, 0, 0);
    glTexCoord2f(1, 1); glVertex3f(W, H, 0);
    glTexCoord2f(0, 1); glVertex3f(0, H, 0);
    glEnd();
    glDisable(GL_TEXTURE_2D);

    /* Blue stays at the BOTTOM-left.  An inverted copy would put it at the top. */
    ok = ok && wpx(c, 8,  8)  == 0x000000FFu &&
               wpx(c, 48, 8)  == 0x00FF0000u &&
               wpx(c, 8,  48) == 0x00FF0000u &&
               wpx(c, 48, 48) == 0x00FF0000u;
    aglxDestroyContext(c);
    return ok;
}

/* CopyTexSubImage2D writes a rectangle into an existing image. */
static int t_copytex_subimage(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 16;
    unsigned char green[16 * 16 * 3];
    for (int i = 0; i < N * N; i++) {
        green[i * 3 + 0] = 0; green[i * 3 + 1] = 255; green[i * 3 + 2] = 0;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, N, N, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, green);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 4, 4, 0, 0, 8, 8);
    int ok = glGetError() == GL_NO_ERROR;

    GLuint fb = 0;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    unsigned char p[3];
    glReadPixels(0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, p);   /* corner: green */
    ok = ok && rgb_eq(p, 0, 255, 0);
    glReadPixels(8, 8, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, p);   /* centre: red */
    ok = ok && rgb_eq(p, 255, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

/* CopyTexImage2D from an FBO, not the window. */
static int t_copytex_from_fbo(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 16;
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);           /* window green, must stay green */

    GLuint src_tex = 0, fb = make_color_fbo(&src_tex, N);
    paint_red_blue(N);

    GLuint dst = 0;
    glGenTextures(1, &dst);
    glBindTexture(GL_TEXTURE_2D, dst);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, N, N, 0);
    int ok = glGetError() == GL_NO_ERROR;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ok = ok && wpx(c, 32, 32) == 0x0000FF00u;   /* window untouched */

    GLuint fb2 = 0;
    glGenFramebuffers(1, &fb2);
    glBindFramebuffer(GL_FRAMEBUFFER, fb2);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, dst, 0);
    unsigned char p[3];
    glReadPixels(N / 4, N / 4, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, p);
    ok = ok && rgb_eq(p, 0, 0, 255);
    glReadPixels(N * 3 / 4, N * 3 / 4, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, p);
    ok = ok && rgb_eq(p, 255, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    (void)fb;
    aglxDestroyContext(c);
    return ok;
}

static int t_copytex_validation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    while (glGetError() != GL_NO_ERROR) { }

    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 0, 0, 8, 8, 0);
    int ok = glGetError() == GL_INVALID_ENUM;

    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, -1, 8, 0);
    ok = ok && glGetError() == GL_INVALID_VALUE;

    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 8, 8, 1);
    ok = ok && glGetError() == GL_INVALID_VALUE;

    /* SubImage of a texture that has no storage yet. */
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, 4, 4);
    ok = ok && glGetError() == GL_INVALID_OPERATION;
    aglxDestroyContext(c);
    return ok;
}

/* FBO → FBO blit: ReadPixels of the dest matches ReadPixels of the source. */
static int t_blit_fbo_to_fbo(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 16;
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint src_tex = 0, src = make_color_fbo(&src_tex, N);
    paint_red_blue(N);
    unsigned char want[6];
    glReadPixels(N / 4, N / 4, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, want);
    glReadPixels(N * 3 / 4, N * 3 / 4, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, want + 3);

    GLuint dst_tex = 0, dst = make_color_fbo(&dst_tex, N);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
    glBlitFramebuffer(0, 0, N, N, 0, 0, N, N, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    int ok = glGetError() == GL_NO_ERROR;

    /* ReadPixels reads the READ framebuffer, so rebind dest as read. */
    glBindFramebuffer(GL_FRAMEBUFFER, dst);
    unsigned char got[6];
    glReadPixels(N / 4, N / 4, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, got);
    glReadPixels(N * 3 / 4, N * 3 / 4, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, got + 3);
    ok = ok && memcmp(want, got, 6) == 0;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ok = ok && wpx(c, 32, 32) == 0x0000FF00u;
    aglxDestroyContext(c);
    return ok;
}

/* Window → FBO blit preserves window-coordinate orientation (Y-flip). */
static int t_blit_window_to_fbo(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    paint_red_blue(W);

    GLuint tex = 0, fb = make_color_fbo(&tex, W);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb);
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    int ok = glGetError() == GL_NO_ERROR;

    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    unsigned char p[3];
    glReadPixels(W / 4, H / 4, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, p);
    ok = ok && rgb_eq(p, 0, 0, 255);
    glReadPixels(W / 4, H * 3 / 4, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, p);
    ok = ok && rgb_eq(p, 255, 0, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

static int t_blit_overlap_error(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 16;
    GLuint tex = 0, fb = make_color_fbo(&tex, N);
    paint_red_blue(N);
    while (glGetError() != GL_NO_ERROR) { }

    glBlitFramebuffer(0, 0, 8, 8, 4, 4, 12, 12,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    int ok = glGetError() == GL_INVALID_OPERATION;

    /* Non-overlapping boxes on the same FBO are legal. */
    glBlitFramebuffer(0, 0, 4, 4, 8, 8, 12, 12,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    ok = ok && glGetError() == GL_NO_ERROR;
    (void)fb;
    aglxDestroyContext(c);
    return ok;
}

static int t_blit_linear_error(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    while (glGetError() != GL_NO_ERROR) { }
    glBlitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    int ok = glGetError() == GL_INVALID_OPERATION;
    glBlitFramebuffer(0, 0, 8, 8, 0, 0, 8, 8,
                      GL_COLOR_BUFFER_BIT, GL_TEXTURE_2D);
    ok = ok && glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

/* NEAREST scale: a 2×2 source blown up 2×, each texel covering a 2×2 block. */
static int t_blit_scale_nearest(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint src_tex = 0, src = make_color_fbo(&src_tex, 2);
    ortho_for(2);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(0, 0, 1);
    solid_quad(0, 0, 1, 1);             /* bottom-left blue */

    GLuint dst_tex = 0, dst = make_color_fbo(&dst_tex, 4);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
    glBlitFramebuffer(0, 0, 2, 2, 0, 0, 4, 4, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    int ok = glGetError() == GL_NO_ERROR;

    glBindFramebuffer(GL_FRAMEBUFFER, dst);
    unsigned char p[3];
    glReadPixels(0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, p);
    ok = ok && rgb_eq(p, 0, 0, 255);
    glReadPixels(1, 1, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, p);
    ok = ok && rgb_eq(p, 0, 0, 255);
    glReadPixels(3, 3, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, p);
    ok = ok && rgb_eq(p, 255, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

/* Depth blit: dest depth test then sees the copied values. */
static int t_blit_depth(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 8;
    GLuint src_tex = 0, src = make_color_fbo(&src_tex, N);
    GLuint srb = 0;
    glGenRenderbuffers(1, &srb);
    glBindRenderbuffer(GL_RENDERBUFFER, srb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, N, N);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, srb);

    GLuint dst_tex = 0, dst = make_color_fbo(&dst_tex, N);
    GLuint drb = 0;
    glGenRenderbuffers(1, &drb);
    glBindRenderbuffer(GL_RENDERBUFFER, drb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, N, N);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, drb);

    glBindFramebuffer(GL_FRAMEBUFFER, src);
    ortho_for(N);
    glClearColor(1, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glColor3f(1, 0, 0);
    /* z=+5 is near under glOrtho. */
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 5); glVertex3f(N, 0, 5);
    glVertex3f(N, N, 5); glVertex3f(0, N, 5);
    glEnd();

    unsigned char dsrc = 0;
    glReadPixels(N / 2, N / 2, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, &dsrc);

    glBindFramebuffer(GL_FRAMEBUFFER, dst);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
    glBlitFramebuffer(0, 0, N, N, 0, 0, N, N, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    int ok = glGetError() == GL_NO_ERROR;

    glBindFramebuffer(GL_FRAMEBUFFER, dst);
    unsigned char ddst = 0;
    glReadPixels(N / 2, N / 2, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, &ddst);
    ok = ok && ddst == dsrc && dsrc < 80;     /* near, not far-plane 255 */

    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

/* GL_FRAMEBUFFER binds both; DRAW/READ can be split.  ReadPixels follows
 * READ, drawing follows DRAW. */
static int t_blit_read_draw_split(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 8;
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);           /* window green */

    GLuint tex = 0, fb = make_color_fbo(&tex, N);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);           /* FBO blue */
    /* make_color_fbo left GL_FRAMEBUFFER = fb, so both bindings are fb. */

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    GLint rd = -1, dr = -1;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rd);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &dr);
    int ok = rd == 0 && dr == (GLint)fb;

    unsigned char p[3];
    glReadPixels(2, 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, p);
    ok = ok && rgb_eq(p, 0, 255, 0);        /* window, not FBO */

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ok = ok && wpx(c, 32, 32) == 0x0000FF00u;
    aglxDestroyContext(c);
    return ok;
}

/* A DEPTH bit against a colour-only FBO is ignored, matching glClear. */
static int t_blit_missing_buffer_ignored(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const int N = 8;
    GLuint a_tex = 0, a = make_color_fbo(&a_tex, N);
    GLuint b_tex = 0, b = make_color_fbo(&b_tex, N);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, a);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b);
    while (glGetError() != GL_NO_ERROR) { }
    glBlitFramebuffer(0, 0, N, N, 0, 0, N, N,
                      GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    int ok = glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* ============================================================================
 * Driver
 * ==========================================================================*/

int main(void) {
    printf("=== test_glfbo: framebuffer objects, glReadPixels, copies (G12/L2) ===\n");

    printf("--- object management ---\n");
    RUN(t_gen_and_delete); RUN(t_bind_reports_binding);
    RUN(t_delete_bound_reverts); RUN(t_bind_unused_name_creates);
    RUN(t_bad_target_rejected);

    printf("--- completeness ---\n");
    RUN(t_status_missing_attachment); RUN(t_status_complete);
    RUN(t_status_unallocated_level); RUN(t_status_dimension_mismatch);
    RUN(t_status_wrong_attachment_type); RUN(t_deleted_texture_incompletes);
    RUN(t_incomplete_refuses_draw); RUN(t_stencil_attachment_accepted);
    RUN(t_attach_to_default_refused);

    printf("--- renderbuffers ---\n");
    RUN(t_renderbuffer_storage_and_query); RUN(t_renderbuffer_bad_format);
    RUN(t_delete_renderbuffer_detaches);

    printf("--- rendering into an FBO ---\n");
    RUN(t_render_to_texture_roundtrip); RUN(t_fbo_does_not_touch_window);
    RUN(t_fbo_depth_renderbuffer); RUN(t_fbo_without_depth_has_none);
    RUN(t_fbo_size_is_attachment_size); RUN(t_render_to_mipmap_level);
    RUN(t_rendered_texture_is_opaque);

    printf("--- glReadPixels ---\n");
    RUN(t_readpixels_rgb); RUN(t_readpixels_row_order);
    RUN(t_readpixels_formats); RUN(t_readpixels_depth);
    RUN(t_readpixels_out_of_bounds); RUN(t_readpixels_validation);
    RUN(t_readpixels_reads_fbo);

    printf("--- copies (GL2 L2) ---\n");
    RUN(t_copytex_window_roundtrip); RUN(t_copytex_subimage);
    RUN(t_copytex_from_fbo); RUN(t_copytex_validation);
    RUN(t_blit_fbo_to_fbo); RUN(t_blit_window_to_fbo);
    RUN(t_blit_overlap_error); RUN(t_blit_linear_error);
    RUN(t_blit_scale_nearest); RUN(t_blit_depth);
    RUN(t_blit_read_draw_split); RUN(t_blit_missing_buffer_ignored);

    printf("--- interplay and regressions ---\n");
    RUN(t_swap_presents_window); RUN(t_resize_while_fbo_bound);
    RUN(t_destroy_frees_renderbuffers); RUN(t_window_rendering_unchanged);
    RUN(t_query_limits);

    printf("\ntest_glfbo: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

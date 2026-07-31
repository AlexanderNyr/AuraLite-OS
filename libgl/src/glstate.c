/* libgl/src/glstate.c — GL state machine: errors, clearing, viewport.
 *
 * Phase G1 of GL_PLAN.md.  Implements the parts of the state machine needed
 * to produce a first frame; matrix state arrives in G2 and the per-fragment
 * tests in G3.
 */

#include <string.h>

#include "GL/gl.h"
#include "GL/auraglx.h"
#include "glcontext.h"

/* ============================================================================
 * Errors (§2.5)
 * ==========================================================================*/

GLenum glGetError(void) {
    struct aglx_context *ctx = gl_current_ctx;

    /* Calling glGetError() without a context is itself a misuse, but there is
     * no context to record it in.  Reporting GL_INVALID_OPERATION is the most
     * informative answer available. */
    if (!ctx) return GL_INVALID_OPERATION;

    GLenum e = ctx->error;
    ctx->error = GL_NO_ERROR;   /* reading clears (§2.5) */
    return e;
}

/* ============================================================================
 * Strings (§6.1.11)
 * ==========================================================================*/

const GLubyte *glGetString(GLenum name) {
    switch (name) {
    case GL_VENDOR:
        return (const GLubyte *)"AuraLite OS";
    case GL_RENDERER:
        /* Updated in phase G9 once the backend boundary can report which
         * backend is actually active. */
        return (const GLubyte *)"AuraLite Software Rasterizer";
    case GL_VERSION:
        return (const GLubyte *)"1.1 AuraLite";
    case GL_EXTENSIONS:
        /* No extensions yet.  The empty string is the correct answer here,
         * not NULL: applications tokenise this and NULL would crash them. */
        return (const GLubyte *)"";
    default:
        gl_set_error(GL_INVALID_ENUM);
        return (const GLubyte *)0;
    }
}

/* ============================================================================
 * Clear state (§4.2.3)
 * ==========================================================================*/

void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    /* GLclampf arguments are clamped when specified, not when used. */
    ctx->clear_color.r = gl_clampf(r);
    ctx->clear_color.g = gl_clampf(g);
    ctx->clear_color.b = gl_clampf(b);
    ctx->clear_color.a = gl_clampf(a);
}

void glClearDepth(GLclampd depth) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    ctx->clear_depth = gl_clampf((GLfloat)depth);
}

void glClear(GLbitfield mask) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    /* Any bit outside the defined set is GL_INVALID_VALUE, and when that
     * happens nothing is cleared at all (§4.2.3). */
    GLbitfield valid = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                       GL_STENCIL_BUFFER_BIT;
    if (mask & ~valid) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }

    size_t pixels = (size_t)ctx->width * (size_t)ctx->height;

    if (mask & GL_COLOR_BUFFER_BIT) {
        gl_pixel_t c = gl_pack_color(ctx->clear_color);
        gl_pixel_t *p = ctx->color;

        if (c == 0) {
            /* Clearing to black is by far the most common case and memset is
             * substantially faster than a manual loop. */
            memset(p, 0, pixels * sizeof(gl_pixel_t));
        } else {
            /* Unrolled by 8: the clear touches every pixel every frame, so it
             * is one of the few places where this is worth the extra code. */
            size_t i = 0;
            size_t n8 = pixels & ~(size_t)7;
            for (; i < n8; i += 8) {
                p[i+0] = c; p[i+1] = c; p[i+2] = c; p[i+3] = c;
                p[i+4] = c; p[i+5] = c; p[i+6] = c; p[i+7] = c;
            }
            for (; i < pixels; i++) p[i] = c;
        }
    }

    if (mask & GL_DEPTH_BUFFER_BIT) {
        /* Silently ignored when the context has no depth buffer, exactly as a
         * GL implementation with a 0-bit depth buffer behaves. */
        if (ctx->depth) {
            float d = ctx->clear_depth;
            float *p = ctx->depth;
            size_t i = 0;
            size_t n8 = pixels & ~(size_t)7;
            for (; i < n8; i += 8) {
                p[i+0] = d; p[i+1] = d; p[i+2] = d; p[i+3] = d;
                p[i+4] = d; p[i+5] = d; p[i+6] = d; p[i+7] = d;
            }
            for (; i < pixels; i++) p[i] = d;
        }
    }

    /* GL_STENCIL_BUFFER_BIT is accepted but has no effect: AuraLite has no
     * stencil buffer yet.  Per the specification, clearing a buffer that does
     * not exist is a no-op rather than an error. */
}

/* ============================================================================
 * Viewport (§2.11)
 * ==========================================================================*/

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (width < 0 || height < 0) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }

    ctx->viewport_x = x;
    ctx->viewport_y = y;
    ctx->viewport_w = width;
    ctx->viewport_h = height;
}

/* ============================================================================
 * Shading model (§2.14.7)
 * ==========================================================================*/

void glShadeModel(GLenum mode) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (mode != GL_FLAT && mode != GL_SMOOTH) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    ctx->shade_model = mode;
}

/* ============================================================================
 * Synchronisation (§5.4)
 * ==========================================================================*/

void glFlush(void) {
    /* A software rasterizer has no command queue: drawing has already
     * happened by the time the entry point returns.  Validating the context
     * still gives applications the expected GL_INVALID_OPERATION when they
     * call GL with nothing current. */
    (void)gl_ctx_or_error();
}

void glFinish(void) {
    /* Same reasoning as glFlush(): nothing is ever outstanding. */
    (void)gl_ctx_or_error();
}

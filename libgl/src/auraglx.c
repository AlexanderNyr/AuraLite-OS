/* libgl/src/auraglx.c — GL context lifecycle and window presentation.
 *
 * See GL/auraglx.h for the public contract and GL_PLAN.md phase G1.
 *
 * Design note: rendering never touches the window.  Everything lands in the
 * context's own colour buffer, and aglxSwapBuffers() is the single point that
 * crosses into the kernel (one ag_blit per frame).  That keeps syscalls out of
 * the rasterizer's inner loops and means the compositor only ever observes
 * complete frames, so there is no tearing.
 */

#include <stdlib.h>
#include <string.h>

#include "GL/auraglx.h"
#include "GL/gl.h"
#include "glcontext.h"
#include "GL/glbackend.h"
#include "glvertex.h"
#include "auragui.h"

/* The current context.  Single-threaded for now; when libgl gains thread
 * support this becomes thread-local, which is exactly why every entry point
 * goes through gl_ctx_or_error() instead of touching this directly. */
struct aglx_context *gl_current_ctx = NULL;

/* ============================================================================
 * Error handling (§2.5)
 * ==========================================================================*/

void gl_set_error(GLenum error) {
    /* GL keeps the FIRST error until glGetError() reads it, so an early
     * failure is never masked by later noise. */
    if (gl_current_ctx && gl_current_ctx->error == GL_NO_ERROR) {
        gl_current_ctx->error = error;
    }
}

struct aglx_context *gl_ctx_or_error(void) {
    if (!gl_current_ctx) {
        /* No context: nowhere to record the error, but returning NULL makes
         * every caller bail out safely. */
        return NULL;
    }
    return gl_current_ctx;
}

/* ============================================================================
 * Context lifecycle
 * ==========================================================================*/

/* Reset all GL state to the defaults mandated by the specification.  Called on
 * creation and on resize so a context is never observed half-initialised. */
static void ctx_set_defaults(struct aglx_context *ctx) {
    ctx->clear_color.r = 0.0f;
    ctx->clear_color.g = 0.0f;
    ctx->clear_color.b = 0.0f;
    ctx->clear_color.a = 0.0f;
    ctx->clear_depth   = 1.0f;      /* far plane (§4.2.3) */

    ctx->viewport_x = 0;
    ctx->viewport_y = 0;
    ctx->viewport_w = ctx->width;
    ctx->viewport_h = ctx->height;

    ctx->error = GL_NO_ERROR;

    /* Matrix state (exercised from G2 onwards). */
    ctx->matrix_mode    = GL_MODELVIEW;
    ctx->modelview_top  = 0;
    ctx->projection_top = 0;
    ctx->modelview[0]   = glm_mat4_identity();
    ctx->projection[0]  = glm_mat4_identity();

    /* Per-fragment state (exercised from G3 onwards).  Defaults per §6.2. */
    ctx->depth_test  = GL_FALSE;
    ctx->depth_mask  = GL_TRUE;
    ctx->depth_func  = GL_LESS;
    ctx->cull_face   = GL_FALSE;
    ctx->cull_mode   = GL_BACK;
    ctx->front_face  = GL_CCW;
    ctx->shade_model = GL_SMOOTH;
    ctx->polygon_mode = GL_FILL;

    ctx->scissor_test = GL_FALSE;
    ctx->scissor_x = 0;
    ctx->scissor_y = 0;
    ctx->scissor_w = ctx->width;
    ctx->scissor_h = ctx->height;

    ctx->attrib_top = 0;

    gl_lighting_set_defaults(ctx);
    gl_texture_set_defaults(ctx);
    gl_frag_set_defaults(ctx);
    gl_array_set_defaults(ctx);
}

/* Allocate colour (and optionally depth) buffers for w*h.
 * Returns 0 on success; on failure nothing is allocated. */
static int ctx_alloc_buffers(struct aglx_context *ctx, int w, int h,
                             uint32_t flags) {
    /* Dimensions are validated by the caller, so this cannot overflow:
     * 4096*4096*4 = 64 MiB fits comfortably in size_t. */
    size_t pixels = (size_t)w * (size_t)h;

    gl_pixel_t *color = (gl_pixel_t *)malloc(pixels * sizeof(gl_pixel_t));
    if (!color) return -1;

    float *depth = NULL;
    if (flags & AGLX_DEPTH) {
        depth = (float *)malloc(pixels * sizeof(float));
        if (!depth) {
            free(color);
            return -1;
        }
    }

    ctx->color  = color;
    ctx->depth  = depth;
    ctx->width  = w;
    ctx->height = h;
    return 0;
}

static void ctx_free_buffers(struct aglx_context *ctx) {
    free(ctx->color);
    free(ctx->depth);
    ctx->color = NULL;
    ctx->depth = NULL;
}

aglx_context_t *aglxCreateContext(int wid, int width, int height,
                                  uint32_t flags) {
    if (wid < 0) return NULL;
    if (width <= 0 || height <= 0) return NULL;
    if (width > AGLX_MAX_DIM || height > AGLX_MAX_DIM) return NULL;

    /* Register the backends before the first context exists.  The hardware
     * candidate goes first so that, once it works, it takes precedence
     * automatically; today it declines and the registry falls through to
     * software (see glvirgl.c). */
    gl_virgl_register();
    gl_backend_init_defaults();

    struct aglx_context *ctx =
        (struct aglx_context *)malloc(sizeof(struct aglx_context));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->wid   = wid;
    ctx->flags = flags;

    if (ctx_alloc_buffers(ctx, width, height, flags) != 0) {
        free(ctx);
        return NULL;
    }

    ctx_set_defaults(ctx);

    /* Start from a defined state: an application that swaps before drawing
     * must not see uninitialised heap memory. */
    memset(ctx->color, 0, (size_t)width * (size_t)height * sizeof(gl_pixel_t));
    if (ctx->depth) {
        for (size_t i = 0; i < (size_t)width * (size_t)height; i++) {
            ctx->depth[i] = 1.0f;
        }
    }

    return ctx;
}

int aglxMakeCurrent(aglx_context_t *ctx) {
    /* NULL is legal and means "unbind", matching glXMakeCurrent(NULL). */
    gl_current_ctx = ctx;
    return 0;
}

aglx_context_t *aglxGetCurrentContext(void) {
    return gl_current_ctx;
}

void aglxDestroyContext(aglx_context_t *ctx) {
    if (!ctx) return;
    if (gl_current_ctx == ctx) gl_current_ctx = NULL;
    gl_backend_notify_destroy(ctx);
    /* Texture images are heap allocations owned by the context. */
    gl_texture_free_all(ctx);
    gl_array_free_all(ctx);
    ctx_free_buffers(ctx);
    free(ctx);
}

int aglxResize(aglx_context_t *ctx, int width, int height) {
    if (!ctx) return -1;
    if (width <= 0 || height <= 0) return -1;
    if (width > AGLX_MAX_DIM || height > AGLX_MAX_DIM) return -1;

    if (width == ctx->width && height == ctx->height) return 0;

    /* Allocate the new buffers BEFORE releasing the old ones, so a failed
     * resize leaves the existing ones intact and the application can keep
     * rendering.
     *
     * This used to allocate into a scratch `struct aglx_context` on the
     * stack.  That was fine while the context was small, and became a
     * user-mode page fault the moment phase G10 grew gl_texture_t with its
     * per-face mipmap chains: sizeof(struct aglx_context) went past 130 KB,
     * comfortably more stack than a user process here has.  Two pointers do
     * the same job and cannot outgrow the stack. */
    size_t pixels = (size_t)width * (size_t)height;

    gl_pixel_t *new_color = (gl_pixel_t *)malloc(pixels * sizeof(gl_pixel_t));
    if (!new_color) return -1;

    float *new_depth = NULL;
    if (ctx->flags & AGLX_DEPTH) {
        new_depth = (float *)malloc(pixels * sizeof(float));
        if (!new_depth) {
            free(new_color);
            return -1;
        }
    }

    ctx_free_buffers(ctx);
    ctx->color  = new_color;
    ctx->depth  = new_depth;
    ctx->width  = width;
    ctx->height = height;

    /* A resize resets the viewport and scissor box to the new full size,
     * which is what a window-system resize does on other platforms. */
    ctx->viewport_x = 0;
    ctx->viewport_y = 0;
    ctx->viewport_w = width;
    ctx->viewport_h = height;
    ctx->scissor_x  = 0;
    ctx->scissor_y  = 0;
    ctx->scissor_w  = width;
    ctx->scissor_h  = height;

    memset(ctx->color, 0, (size_t)width * (size_t)height * sizeof(gl_pixel_t));
    if (ctx->depth) {
        for (size_t i = 0; i < (size_t)width * (size_t)height; i++) {
            ctx->depth[i] = 1.0f;
        }
    }
    return 0;
}

/* ============================================================================
 * Presentation
 * ==========================================================================*/

int aglxSwapBuffers(aglx_context_t *ctx) {
    if (!ctx || !ctx->color) return -1;

    /* Give the backend first refusal: a GPU path would scan its own render
     * target out instead of blitting a CPU buffer through the compositor. */
    if (gl_backend_try_present(ctx) == 0) return 0;

    /* One syscall per frame: hand the whole colour buffer to the window.
     * ag_blit() clips against the window's back buffer, so a context larger
     * than its window is harmless. */
    int rc = ag_blit(ctx->wid, 0, 0,
                     (uint32_t)ctx->width, (uint32_t)ctx->height,
                     ctx->color, (uint32_t)ctx->width);
    if (rc != 0) return -1;

    /* Mark the window dirty so the compositor picks the new content up on its
     * next tick.
     *
     * Deliberately NOT ag_render_now(): that op (GUI_OP_RENDER) is restricted
     * to PID <= 2 in the kernel, so an ordinary application calling it just
     * gets -1 back.  gui_blit() already sets content_dirty, and this makes the
     * intent explicit and keeps the frame path working for unprivileged
     * processes.  The compositor runs at 100 FPS, so the added latency is at
     * most one tick. */
    ag_window_invalidate(ctx->wid);
    return 0;
}

/* ============================================================================
 * Introspection
 * ==========================================================================*/

int aglxGetWidth(const aglx_context_t *ctx) {
    return ctx ? ctx->width : 0;
}

int aglxGetHeight(const aglx_context_t *ctx) {
    return ctx ? ctx->height : 0;
}

const uint32_t *aglxGetColorBuffer(const aglx_context_t *ctx) {
    return ctx ? ctx->color : NULL;
}

const float *aglxGetDepthBuffer(const aglx_context_t *ctx) {
    return ctx ? ctx->depth : NULL;
}

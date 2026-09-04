/* libgl/include/GL/glbackend.h — rendering backend selection.
 *
 * Phase G9 of GL_PLAN.md.
 *
 * WHY THIS EXISTS
 *
 * Everything up to G8 rasterises on the CPU.  That is the right default: it
 * works in QEMU with no flags, in VirtualBox and VMware, and on real hardware.
 * But AuraLite already carries a VirGL/virtio-gpu command transport
 * (drivers/gpu/), and a hardware path should be able to slot in without
 * touching a single line of application code.
 *
 * This header is that seam.  It is modelled directly on kernel/net/netdev.h,
 * which lets the IP stack run over e1000 or virtio-net chosen at boot: a
 * backend fills in a table of function pointers and registers itself, and the
 * first one that registers becomes active.
 *
 * WHAT A BACKEND IS RESPONSIBLE FOR
 *
 * Deliberately very little.  A backend supplies the operations where hardware
 * acceleration would actually pay: clearing buffers, drawing a batch of
 * triangles, and presenting a finished frame.  Everything else — the state
 * machine, matrix stacks, clipping, lighting, display lists — stays in the
 * shared code above the seam, because duplicating it per backend is exactly
 * how two rendering paths start disagreeing with each other.
 *
 * A backend may leave any operation NULL, in which case libgl falls back to
 * the software implementation for that operation alone.  This matters: a
 * partial hardware backend is useful long before it is complete, and it can be
 * brought up one entry point at a time with the software path covering the
 * rest.
 */
#ifndef AURALITE_GL_BACKEND_H
#define AURALITE_GL_BACKEND_H

#include <stdint.h>

#include "GL/gl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Backend capability flags, reported through gl_backend_info(). */
#define GL_BACKEND_SOFTWARE   0x0001u  /* CPU rasterisation                  */
#define GL_BACKEND_HARDWARE   0x0002u  /* real GPU command submission        */
#define GL_BACKEND_DEPTH      0x0004u  /* provides its own depth buffer      */
#define GL_BACKEND_TEXTURE    0x0008u  /* accelerates texture sampling       */

/* Opaque to backends; defined in libgl/src/glcontext.h. */
struct aglx_context;

/* One draw batch handed to a backend's `draw` hook.
 *
 * `data` holds `count` vertices of eight floats each: the CLIP-space position
 * (x, y, z, w — the same values the fixed-function transform stage computes,
 * so the GPU divides and clips exactly what the CPU rasterizer would have)
 * followed by the vertex colour (r, g, b, a).  Only whole GL_TRIANGLES
 * batches are eligible, so `count` is always a multiple of three.
 */
typedef struct gl_draw_batch {
    const GLfloat *data;
    GLsizei        count;
} gl_draw_batch_t;

/* One rendering backend.
 *
 * Any member except `name` may be NULL: libgl then uses its software path for
 * that operation.  A backend that implements nothing but `present` is legal
 * and still useful — it would hand finished frames to a GPU scanout while
 * everything else stays on the CPU.
 */
typedef struct gl_backend {
    const char *name;
    unsigned    flags;

    /* Called once when the backend becomes active.  Return 0 on success; a
     * non-zero return means the backend declines and libgl keeps looking. */
    int (*init)(void);

    /* Clear the colour and/or depth buffers.  `mask` uses the same bits as
     * glClear.  Return 0 if handled, non-zero to fall back to software. */
    int (*clear)(struct aglx_context *ctx, GLbitfield mask);

    /* Present the finished colour buffer to the window.  Return 0 if handled,
     * non-zero to fall back. */
    int (*present)(struct aglx_context *ctx);

    /* Draw a batch of triangles on the hardware (GL2 L6).  Return 0 if the
     * batch was submitted — the caller then skips its software rasterizer for
     * it — non-zero and the WHOLE draw falls back: a frame half CPU-drawn and
     * half GPU-drawn is a tearing bug, so the fallback unit is the entire
     * draw call, never one triangle inside it. */
    int (*draw)(struct aglx_context *ctx, const gl_draw_batch_t *batch);

    /* Release backend resources for a context being destroyed. */
    void (*destroy)(struct aglx_context *ctx);
} gl_backend_t;

/* What the active backend is and what it can do. */
typedef struct {
    const char *name;
    unsigned    flags;
    int         hardware;        /* non-zero when a GPU path is active */
} gl_backend_info_t;

/* Register a backend.  The first one to register and whose init() succeeds
 * becomes active; later registrations are remembered but do not displace it.
 * The software backend registers itself lazily, so a hardware backend that
 * registers before the first context is created wins. */
void gl_backend_register(const gl_backend_t *backend);

/* The active backend, never NULL once a context exists (the software backend
 * is the guaranteed fallback). */
const gl_backend_t *gl_backend_active(void);

/* Describe the active backend.  Also drives glGetString(GL_RENDERER), so an
 * application can report which path it is on. */
const gl_backend_info_t *gl_backend_info(void);

/* Force a specific backend by name, or NULL to accept the default.  Intended
 * for tests and for a debug switch in demos; returns 0 if the named backend is
 * registered, non-zero otherwise. */
int gl_backend_force(const char *name);

/* Try the active backend's `draw` hook on one assembled batch (GL2 L6).
 * Returns 0 only if the backend took the batch; the caller then skips the
 * software rasterizer for it.  A non-zero return means the whole draw falls
 * back. */
int gl_backend_try_draw(struct aglx_context *ctx,
                        const gl_draw_batch_t *batch);

/* Whether the current GL state would let a fixed-function glDrawArrays batch
 * take the canned hardware path at all: GL_TRIANGLES, whole triangles, no
 * bound program, and none of the state the canned pipeline cannot reproduce
 * (texturing, blending, tests, scissor, culling, non-fill polygon mode, fog).
 * The canned vertex shader does no transforms of its own — the batch carries
 * clip coordinates the fixed-function matrices already produced — so no
 * matrix restriction is needed.  Checked BEFORE any batch data is built, so
 * declining costs one read of the state vector. */
int gl_backend_draw_eligible(struct aglx_context *ctx, GLenum mode,
                             GLsizei count);

/* Register the VirGL hardware backend candidate.  Called automatically at
 * context creation; exposed so tests can register it explicitly.  It declines
 * at init() until the kernel exposes a user-space 3D submission path — see
 * libgl/src/glvirgl.c for what completing it involves. */
void gl_virgl_register(void);

/* GL2 L6 test seam: the canned pipeline's encoding, exposed so host tests can
 * walk it against the kernel validator with no device in the room.  The setup
 * stream is the one-time pipeline submission (objects, shaders, binds,
 * framebuffer, viewport, vertex-buffer bind); it writes at most `cap` dwords
 * and returns its length, or -1 when `cap` is too small. */
const uint32_t *gl_virgl_canned_vs_tokens(int *n);
const uint32_t *gl_virgl_canned_fs_tokens(int *n);
int  gl_virgl_canned_setup(uint32_t w, uint32_t h, uint32_t *d, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AURALITE_GL_BACKEND_H */

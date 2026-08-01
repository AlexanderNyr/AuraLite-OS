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

/* Register the VirGL hardware backend candidate.  Called automatically at
 * context creation; exposed so tests can register it explicitly.  It declines
 * at init() until the kernel exposes a user-space 3D submission path — see
 * libgl/src/glvirgl.c for what completing it involves. */
void gl_virgl_register(void);

#ifdef __cplusplus
}
#endif

#endif /* AURALITE_GL_BACKEND_H */

/* libgl/include/GL/auraglx.h — window binding for the AuraLite GL stack.
 *
 * AuraGLX is to AuraLite what GLX is to X11 and EGL is to Wayland: the glue
 * that ties a GL rendering context to a window-system drawable.  Core GL has
 * no concept of a window, so every platform needs a layer like this.
 *
 * Usage:
 *
 *     int wid = ag_window_create(x, y, w, h, "demo", AG_WIN_DEFAULT);
 *     aglx_context_t *ctx = aglxCreateContext(wid, 640, 480, 0);
 *     aglxMakeCurrent(ctx);
 *
 *     glClearColor(0.2f, 0.3f, 0.4f, 1.0f);
 *     glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
 *     ... drawing ...
 *     aglxSwapBuffers(ctx);          // presents through ag_blit()
 *
 *     aglxDestroyContext(ctx);
 *
 * Rendering always targets an off-screen colour buffer owned by the context;
 * aglxSwapBuffers() is the only function that touches the window.  That keeps
 * the GL pipeline free of syscalls in its inner loops and gives tear-free
 * output for free, since the compositor only ever sees complete frames.
 */
#ifndef AURALITE_AURAGLX_H
#define AURALITE_AURAGLX_H

#include <stdint.h>
#include "GL/gl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Context creation flags ---- */

/* Allocate a depth buffer.  Without this GL_DEPTH_TEST cannot be enabled and
 * glClear(GL_DEPTH_BUFFER_BIT) is a no-op.  Costs width*height*4 bytes. */
#define AGLX_DEPTH      0x0001

/* Allocate an 8-bit stencil plane.  Without this GL_STENCIL_TEST is a no-op
 * and glClear(GL_STENCIL_BUFFER_BIT) does nothing.  Costs width*height bytes,
 * on the heap with the other buffers — never the C stack. */
#define AGLX_STENCIL    0x0002

/* Default: colour + depth + stencil, which is what almost every 3D
 * application wants and what lets /glcube pick stencil up without opting in. */
#define AGLX_DEFAULT    (AGLX_DEPTH | AGLX_STENCIL)

/* Software rasterisation at high resolutions is slow and memory hungry
 * (colour + depth = width*height*8 bytes), so contexts are capped.  4096 is
 * far above anything usable in practice but keeps the size arithmetic
 * provably overflow-free. */
#define AGLX_MAX_DIM    4096

/* Opaque to applications; the definition lives in libgl/src/glcontext.h. */
typedef struct aglx_context aglx_context_t;

/* Create a rendering context targeting window `wid`.
 *
 * width/height are the resolution of the render buffers, which need not match
 * the window size: aglxSwapBuffers() blits at 1:1 into the window's top-left
 * corner, so a smaller context simply covers part of the window.  This is the
 * cheap way to keep a software rasterizer responsive.
 *
 * Returns NULL if the parameters are invalid or memory is exhausted. */
aglx_context_t *aglxCreateContext(int wid, int width, int height,
                                  uint32_t flags);

/* Make `ctx` the context that GL entry points operate on.  Passing NULL
 * unbinds the current context, after which GL calls are no-ops that record
 * GL_INVALID_OPERATION.  Returns 0 on success. */
int aglxMakeCurrent(aglx_context_t *ctx);

/* The context currently bound, or NULL. */
aglx_context_t *aglxGetCurrentContext(void);

/* Present the colour buffer to the window.  Returns 0 on success. */
int aglxSwapBuffers(aglx_context_t *ctx);

/* Resize the render buffers.  Existing contents are discarded; the viewport is
 * reset to the full new size, matching what a window-system resize does
 * elsewhere.  Returns 0 on success; on failure the old buffers are kept. */
int aglxResize(aglx_context_t *ctx, int width, int height);

/* Release every resource owned by the context.  Safe to call with NULL.  If
 * the context is current it is unbound first. */
void aglxDestroyContext(aglx_context_t *ctx);

/* ---- Introspection (useful for demos, tests and the FPS counter) ---- */
int aglxGetWidth(const aglx_context_t *ctx);
int aglxGetHeight(const aglx_context_t *ctx);

/* Direct access to the colour buffer, for tests and for applications that
 * want to read back pixels without a full glReadPixels implementation.
 * Returns NULL if ctx is NULL.  The buffer is width*height packed XRGB8888
 * with no padding between rows. */
const uint32_t *aglxGetColorBuffer(const aglx_context_t *ctx);

/* Direct access to the depth buffer, or NULL if the context has none.
 * Values are in window-space depth [0,1], where 1.0 is the far plane. */
const float *aglxGetDepthBuffer(const aglx_context_t *ctx);

/* Direct access to the window stencil plane, or NULL if the context has none.
 * One byte per pixel, same row order as the colour buffer. */
const uint8_t *aglxGetStencilBuffer(const aglx_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* AURALITE_AURAGLX_H */

/* libgl/src/glvirgl.c — VirGL hardware backend.
 *
 * Phase G13 of GL_PLAN.md, on the seam phase G9 built and the syscall phase
 * K1 exposed.
 *
 * WHAT THIS DOES, AND WHAT IT DELIBERATELY DOES NOT
 *
 * It implements steps 1-3 of the plan's scope: probe, clear and present.  The
 * frame is still rasterised by the CPU; what moves to the GPU is the PRESENT,
 * which stops the finished pixels travelling through the compositor's blit
 * path and lands them on the scanout directly.
 *
 * It does NOT implement DRAW_VBO.  That is step 5, it needs shaders expressed
 * as TGSI, and the plan says so: "a real triangle path depends on G11's
 * compiler being retargetable to TGSI".  G11 produces an AST and interprets
 * it; retargeting that to TGSI is a compiler back end, which is a phase in its
 * own right and is not this one.  Claiming otherwise by shipping a
 * half-working draw path would be worse than not shipping it -- see the note
 * on honesty below.
 *
 * SO WHAT IS IT WORTH?
 *
 * Measure before believing.  On this implementation the software rasterizer
 * costs milliseconds per frame and ag_blit() costs tens of microseconds, so
 * moving the present to the GPU is a small fraction of a frame.  The value is
 * not speed today: it is that the seam is proved end to end -- a real
 * virtio-gpu context, a real backed resource, a real command stream accepted
 * by a real device -- so the remaining work is bounded and known.
 *
 * HONESTY ABOUT DECLINING
 *
 * probe() returns failure unless a virtio-gpu with VirGL is actually present
 * AND every setup step succeeded.  A backend that registered and then failed
 * per-frame would report "hardware" in GL_RENDERER and produce silence or
 * corruption; declining leaves the software path in place and the renderer
 * string truthful.  That was the G9 reasoning and it has not changed.
 */

#include <string.h>
#include <unistd.h>

#include "GL/gl.h"
#include "GL/glbackend.h"
#include "glcontext.h"

/* The kernel's GPU ABI.  libgl is user space, so this header is shared with
 * the kernel rather than duplicated -- the alternative is two definitions of
 * a binary layout drifting apart. */
#include "kernel/gpu/gpu_syscalls.h"

/* ============================================================================
 * Syscall plumbing
 * ==========================================================================*/

static int64_t gpu_call(uint64_t op, uint64_t a2) {
    return syscall(SYS_GPU_CALL, op, a2, 0, 0, 0, 0);
}

/* ---- VirGL command-stream encoding ----
 *
 * A VirGL command is a header dword followed by its payload:
 *
 *   bits 24..31  command id
 *   bits 16..23  object type (unused by the commands here)
 *   bits  0..15  payload length in dwords, NOT counting the header
 *
 * The layout is taken from drivers/gpu/virgl.h rather than restated, because
 * the first draft of this file guessed it the other way round -- id low,
 * length high -- and produced headers the kernel validator rejected.  It
 * failed safely, but only because the validator exists; a stream that had
 * slipped through would have been a length field the host GPU trusted.
 *
 * Sharing the driver's macro means there is one definition of the wire format
 * in the tree. */
#include "drivers/gpu/virgl.h"

/* Everything else -- the CLEAR opcode, the buffer bitmask, the pixel format
 * and the bind flags -- comes from drivers/gpu/virgl.h too.  The first draft
 * of this file guessed all four and got all four wrong: CLEAR was 3 rather
 * than 4, the colour bit was 0x4 rather than 0x1, depth was 0x1 rather than
 * 0x2, and RENDER_TARGET was 0x2 rather than 0x10.  Only the redefinition
 * warning caught them, because none of it executes without a GPU.
 *
 * The lesson is narrow and worth stating: protocol constants are not worth
 * retyping.  A wrong one produces a stream the device accepts and
 * misinterprets, which is far harder to diagnose than a stream it rejects.
 *
 * VIRGL_PIPE_TEXTURE_2D is the one value the driver header does not define,
 * since nothing in the kernel creates a 2D texture resource; PIPE_TEXTURE_2D
 * is 2 in Gallium's enum, which is what virglrenderer implements. */
#define VIRGL_PIPE_TEXTURE_2D 2u

/* ============================================================================
 * Backend state
 *
 * One context and one render target for the whole library.  A second GL
 * context would need its own, but nothing creates one today and a table would
 * be speculative structure -- the single slot is a deliberate limit, checked
 * in aglx_bind() rather than assumed.
 * ==========================================================================*/

static struct {
    int      up;                 /* probe() succeeded                       */
    uint32_t ctx;                /* kernel context handle                   */
    uint32_t res;                /* render-target resource handle           */
    int      res_w, res_h;       /* its size, so a resize can be detected   */
    struct aglx_context *owner;  /* the GL context using it, or NULL        */
    gpu_info_t info;
} vg;

/* Tear the resource down; the context survives so a resize can rebuild. */
static void release_target(void) {
    if (vg.res) {
        gpu_call(GPU_OP_RES_DESTROY, vg.res);
        vg.res = 0;
        vg.res_w = vg.res_h = 0;
    }
}

/* Create a render target of the given size, replacing any existing one. */
static int make_target(int w, int h) {
    release_target();

    gpu_res_create_t rc;
    memset(&rc, 0, sizeof rc);
    rc.ctx        = vg.ctx;
    rc.target     = VIRGL_PIPE_TEXTURE_2D;
    rc.format     = VIRGL_PIPE_FORMAT_B8G8R8X8_UNORM;
    rc.bind       = VIRGL_BIND_RENDER_TARGET;
    rc.width      = (uint32_t)w;
    rc.height     = (uint32_t)h;
    rc.depth      = 1;
    rc.array_size = 1;

    int64_t r = gpu_call(GPU_OP_RES_CREATE, (uint64_t)(uintptr_t)&rc);
    if (r <= 0) return -1;

    vg.res   = (uint32_t)r;
    vg.res_w = w;
    vg.res_h = h;
    return 0;
}

/* ============================================================================
 * Backend entry points
 * ==========================================================================*/

static int glvirgl_init(void) {
    memset(&vg, 0, sizeof vg);

    /* Ask the kernel what is there.  A kernel without the syscall returns a
     * negative errno, which is the same "no" as no device at all. */
    if (gpu_call(GPU_OP_INFO, (uint64_t)(uintptr_t)&vg.info) != 0) return -1;
    if (!vg.info.present || !vg.info.virgl) return -1;

    int64_t c = gpu_call(GPU_OP_CTX_CREATE, 0);
    if (c <= 0) return -1;
    vg.ctx = (uint32_t)c;

    vg.up = 1;
    return 0;
}

/* Bind the backend to a GL context, creating a matching render target.
 * Returns 0 when the hardware path can serve this context. */
static int aglx_bind(struct aglx_context *ctx) {
    if (!vg.up || !ctx) return -1;

    /* One GL context at a time.  A second one falls back to software rather
     * than silently sharing a render target with the first. */
    if (vg.owner && vg.owner != ctx) return -1;

    if (!vg.res || vg.res_w != ctx->win_width || vg.res_h != ctx->win_height) {
        if (make_target(ctx->win_width, ctx->win_height) != 0) return -1;
    }
    vg.owner = ctx;
    return 0;
}

static int glvirgl_clear(struct aglx_context *ctx, GLbitfield mask) {
    if (aglx_bind(ctx) != 0) return -1;

    /* VirGL's CLEAR takes the buffer mask, an RGBA colour as four floats, a
     * double depth and a stencil value: 8 dwords of payload. */
    uint32_t buffers = 0;
    if (mask & GL_COLOR_BUFFER_BIT) buffers |= VIRGL_PIPE_CLEAR_COLOR0;
    if (mask & GL_DEPTH_BUFFER_BIT) buffers |= VIRGL_PIPE_CLEAR_DEPTH;
    if (buffers == 0) return -1;              /* nothing we handle */

    union { float f; uint32_t u; } cv[4];
    cv[0].f = ctx->clear_color.r;
    cv[1].f = ctx->clear_color.g;
    cv[2].f = ctx->clear_color.b;
    cv[3].f = ctx->clear_color.a;

    union { double d; uint32_t u[2]; } dv;
    dv.d = (double)ctx->clear_depth;

    uint32_t cmd[9];
    cmd[0] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
    cmd[1] = buffers;
    cmd[2] = cv[0].u; cmd[3] = cv[1].u; cmd[4] = cv[2].u; cmd[5] = cv[3].u;
    cmd[6] = dv.u[0]; cmd[7] = dv.u[1];
    cmd[8] = 0;                                /* stencil */

    gpu_submit_t sb;
    memset(&sb, 0, sizeof sb);
    sb.ctx  = vg.ctx;
    sb.size = (uint32_t)sizeof cmd;
    sb.cmd  = (uint64_t)(uintptr_t)cmd;

    if (gpu_call(GPU_OP_SUBMIT, (uint64_t)(uintptr_t)&sb) != 0) return -1;

    /* The GPU cleared its own render target, but the CPU rasterizer draws
     * into ctx->color and knows nothing about that.  Until the draw path
     * moves to the GPU too, the software buffer has to be cleared as well or
     * the frame presented below would carry the previous frame's pixels.
     *
     * So this returns -1: "not handled, do it in software".  Emitting the
     * hardware clear anyway keeps the GPU's target consistent for the day the
     * draw path arrives, and costs one command per frame.  Returning 0 here
     * would be the bug -- it would skip the software clear and present stale
     * content. */
    return -1;
}

static int glvirgl_present(struct aglx_context *ctx) {
    if (aglx_bind(ctx) != 0) return -1;
    if (!ctx->win_color) return -1;

    size_t bytes = (size_t)ctx->win_width * (size_t)ctx->win_height * 4u;
    if (bytes == 0 || bytes > GPU_MAX_XFER_BYTES) return -1;

    /* Upload the finished frame into the render target. */
    gpu_transfer_t tr;
    memset(&tr, 0, sizeof tr);
    tr.ctx    = vg.ctx;
    tr.res    = vg.res;
    tr.w      = (uint32_t)ctx->win_width;
    tr.h      = (uint32_t)ctx->win_height;
    tr.d      = 1;
    tr.stride = (uint32_t)ctx->win_width * 4u;
    tr.size   = (uint32_t)bytes;
    tr.data   = (uint64_t)(uintptr_t)ctx->win_color;

    if (gpu_call(GPU_OP_TRANSFER, (uint64_t)(uintptr_t)&tr) != 0) return -1;

    /* Scan it out and flush.  SET_SCANOUT points the display at the resource;
     * RESOURCE_FLUSH tells the host the contents changed. */
    gpu_scanout_t so;
    memset(&so, 0, sizeof so);
    so.scanout = 0;
    so.res     = vg.res;
    so.w       = (uint32_t)ctx->win_width;
    so.h       = (uint32_t)ctx->win_height;
    if (gpu_call(GPU_OP_SET_SCANOUT, (uint64_t)(uintptr_t)&so) != 0) return -1;

    gpu_flush_t fl;
    memset(&fl, 0, sizeof fl);
    fl.res = vg.res;
    fl.w   = (uint32_t)ctx->win_width;
    fl.h   = (uint32_t)ctx->win_height;
    if (gpu_call(GPU_OP_FLUSH, (uint64_t)(uintptr_t)&fl) != 0) return -1;

    return 0;                                  /* handled: skip ag_blit */
}

static void glvirgl_destroy(struct aglx_context *ctx) {
    if (vg.owner == ctx) {
        release_target();
        vg.owner = NULL;
    }
}

static const gl_backend_t glvirgl_backend = {
    "AuraLite VirGL (virtio-gpu)",
    GL_BACKEND_HARDWARE,
    glvirgl_init,
    glvirgl_clear,
    glvirgl_present,
    glvirgl_destroy,
};

/* Called from context creation, before the software backend is registered, so
 * a working hardware path takes precedence automatically. */
void gl_virgl_register(void) {
    gl_backend_register(&glvirgl_backend);
}

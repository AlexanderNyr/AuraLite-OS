/* libgl/src/glvirgl.c — VirGL hardware backend (groundwork).
 *
 * Phase G9 of GL_PLAN.md.
 *
 * STATUS: this backend deliberately DECLINES at init() time.  It exists to
 * prove the backend seam is real and to give the hardware path a place to grow
 * into, not to accelerate anything today.
 *
 * WHY IT DECLINES RATHER THAN HALF-WORKING
 *
 * AuraLite has a VirGL command transport in drivers/gpu/virgl.c, but it lives
 * in the KERNEL: contexts, resources, fenced SUBMIT_3D and scanout present are
 * all kernel-side operations with no syscall exposed to user space.  libgl runs
 * in user space by design (see GL_PLAN.md decision D3), so wiring this up needs
 * a new syscall surface for 3D submission — a piece of kernel work in its own
 * right, with its own validation and security review.
 *
 * Until that exists, the honest behaviour is to decline.  A backend that
 * registered and then failed every draw call would be far worse: applications
 * would see a "hardware" renderer string and silent corruption instead of a
 * clean software fallback.
 *
 * WHAT COMPLETING IT LOOKS LIKE
 *
 *   1. Kernel: expose virtio_gpu_ctx_create / resource_create_3d /
 *      submit_3d_fenced / set_scanout_resource through a syscall, with the
 *      same user-pointer validation the GUI blit path uses.
 *   2. probe(): call it, and return 0 only when a virtio-gpu with VirGL is
 *      actually present.
 *   3. clear(): emit VIRGL_CCMD_CLEAR instead of touching ctx->color.
 *   4. present(): TRANSFER_TO_HOST_3D + SET_SCANOUT + RESOURCE_FLUSH, which
 *      drivers/gpu/virgl.c already implements as
 *      virgl_present_render_target().
 *   5. A triangle path needs a TGSI shader compiler, which is phase G11
 *      territory — but clear+present alone would already move the per-frame
 *      blit off the CPU.
 *
 * Registering this backend is harmless precisely because it declines: the
 * registry then falls through to software, which is what the test asserts.
 */

#include "GL/gl.h"
#include "GL/glbackend.h"
#include "glcontext.h"

/* Is a VirGL-capable GPU reachable from user space?
 *
 * Always false today: there is no syscall to ask.  Written as a function
 * rather than a constant so the one place that needs to change when the
 * kernel gains that syscall is obvious. */
static int virgl_available_to_userspace(void) {
    return 0;
}

static int virgl_init(void) {
    if (!virgl_available_to_userspace()) {
        return -1;               /* decline; the registry moves on */
    }
    return 0;
}

static int virgl_clear(struct aglx_context *ctx, GLbitfield mask) {
    (void)ctx; (void)mask;
    return -1;                   /* not handled: software does it */
}

static int virgl_present(struct aglx_context *ctx) {
    (void)ctx;
    return -1;
}

static void virgl_destroy(struct aglx_context *ctx) {
    (void)ctx;
}

static const gl_backend_t virgl_backend = {
    "AuraLite VirGL (virtio-gpu)",
    GL_BACKEND_HARDWARE,
    virgl_init,
    virgl_clear,
    virgl_present,
    virgl_destroy,
};

/* Called from context creation, before the software backend is registered, so
 * that a working hardware path would take precedence automatically. */
void gl_virgl_register(void) {
    gl_backend_register(&virgl_backend);
}

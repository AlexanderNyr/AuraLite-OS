/* tests/unit/glstub/auragui_stub.c — recording stub for libauragui.
 *
 * Lets the host unit tests observe exactly what aglxSwapBuffers() presents,
 * and lets them simulate a failing blit, without a kernel or a window server.
 */

#include <stdint.h>
#include "auragui.h"

ag_stub_state_t ag_stub;

void ag_stub_reset(void) {
    ag_stub.calls            = 0;
    ag_stub.renders          = 0;
    ag_stub.last_wid         = -1;
    ag_stub.last_x           = 0;
    ag_stub.last_y           = 0;
    ag_stub.last_w           = 0;
    ag_stub.last_h           = 0;
    ag_stub.last_stride      = 0;
    ag_stub.last_first_pixel = 0;
    ag_stub.fail_next        = 0;
}

int ag_blit(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h,
            const uint32_t *src, uint32_t src_stride) {
    ag_stub.calls++;
    ag_stub.last_wid    = wid;
    ag_stub.last_x      = x;
    ag_stub.last_y      = y;
    ag_stub.last_w      = w;
    ag_stub.last_h      = h;
    ag_stub.last_stride = src_stride;
    ag_stub.last_first_pixel = (src && w && h) ? src[0] : 0;

    if (ag_stub.fail_next) {
        ag_stub.fail_next = 0;
        return -1;
    }
    return 0;
}

int ag_blit_alpha(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h,
                  const uint32_t *src, uint32_t src_stride) {
    return ag_blit(wid, x, y, w, h, src, src_stride);
}

void ag_render_now(void) {
    ag_stub.renders++;
}

int ag_window_invalidate(int wid) {
    (void)wid;
    ag_stub.renders++;
    return 0;
}

/* ---- Syscall stand-in, for the VirGL backend (phase G13) ----
 *
 * glvirgl.c asks the kernel whether a virtio-gpu with VirGL exists.  On the
 * host there is no kernel, so this reports failure for every call, and the
 * backend declines exactly as it does on hardware without a GPU.
 *
 * That is the behaviour the backend tests assert: the hardware candidate is
 * registered, it declines, and the registry falls through to software with a
 * truthful GL_RENDERER string.
 */
int64_t syscall(int64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)num; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return -1;
}

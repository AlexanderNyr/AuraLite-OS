/* tests/unit/glstub/auragui_stub.c — recording stub for libauragui.
 *
 * Lets the host unit tests observe exactly what aglxSwapBuffers() presents,
 * and lets them simulate a failing blit, without a kernel or a window server.
 */

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

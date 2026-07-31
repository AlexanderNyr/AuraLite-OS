/* tests/unit/glstub/auragui.h — host-side stand-in for libauragui.
 *
 * libgl/src/auraglx.c includes "auragui.h" to reach ag_blit()/ag_render_now().
 * The real header pulls in AuraLite's freestanding libc, which cannot be
 * compiled against the host toolchain, so the unit tests put this directory
 * first on the include path instead.
 *
 * This is deliberately NOT a re-implementation of libauragui: it declares
 * exactly the two functions auraglx.c uses, and the accompanying
 * auragui_stub.c records the calls so tests can assert on what was presented.
 * The code under test is still the real libgl/src/auraglx.c.
 */
#ifndef AURALITE_TEST_AURAGUI_STUB_H
#define AURALITE_TEST_AURAGUI_STUB_H

#include <stdint.h>

int  ag_blit(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h,
             const uint32_t *src, uint32_t src_stride);
int  ag_blit_alpha(int wid, int32_t x, int32_t y, uint32_t w, uint32_t h,
                   const uint32_t *src, uint32_t src_stride);
void ag_render_now(void);

/* ---- Test observation hooks (not part of the real libauragui API) ---- */

typedef struct {
    int      calls;         /* how many times ag_blit() was called */
    int      renders;       /* how many times ag_render_now() was called */
    int      last_wid;
    int32_t  last_x, last_y;
    uint32_t last_w, last_h;
    uint32_t last_stride;
    uint32_t last_first_pixel;   /* src[0] at the time of the call */
    int      fail_next;     /* when set, the next ag_blit() returns -1 */
} ag_stub_state_t;

extern ag_stub_state_t ag_stub;

void ag_stub_reset(void);

#endif /* AURALITE_TEST_AURAGUI_STUB_H */

/* sdf_blend_stub.c — the two extern "C" hooks sdf.rs declares.
 *
 * tests/unit/test_sdf_simd.rs links the REAL kernel/gui/sdf/sdf.rs, which
 * calls gfx_blend_pixel/gfx_blend_pixels4 from its draw entry points.
 * Those entry points are never reached by the interpolation test, but the
 * symbols must resolve or the link fails.  This stub is deliberately
 * inert: a test that accidentally drew something would count nothing.
 */
#include <stdint.h>

void gfx_blend_pixel(int x, int y, uint32_t color, uint8_t alpha) {
    (void)x; (void)y; (void)color; (void)alpha;
}

void gfx_blend_pixels4(int x, int y, uint32_t color, const uint8_t *alpha) {
    (void)x; (void)y; (void)color; (void)alpha;
}

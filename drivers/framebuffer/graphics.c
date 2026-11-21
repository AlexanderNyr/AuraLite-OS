/* graphics.c — 2D graphics library with double-buffering.
 *
 * Renders to an off-screen back buffer allocated via kmalloc, then copies
 * (flips) to the visible Limine framebuffer. This avoids tearing/flicker
 * during multi-element redraws. Pixel plotting, filled rectangles, lines
 * (Bresenham), and bitmap-font text are provided.
 *
 * Extended with:
 *   - MSDF / anti-aliased blending primitives (gfx_blend_*).
 *   - Translucent rect fill, per-pixel-alpha blit, box blur.
 *   - Clip query, AA rounded rect, four-corner bilinear gradient.
 */

#include <stdint.h>
#include <stddef.h>
#include "drivers/framebuffer/graphics.h"
#include "drivers/framebuffer/psf.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/string.h"
#include "kernel/boot_info.h"
#include "drivers/gpu/virtio_gpu.h"

#if defined(__i386__)
/* RES-11: declared here for the same reason fb.c declares it -- including
 * kernel/arch/i386/fbmap32.h from a portable file would grow the arch-include
 * count the width sweep measures, and arch.h has no framebuffer entry yet. */
void *fb32_map_fb(uint32_t phys, uint32_t len);
#endif

#if defined(__x86_64__) && !defined(__TINYC__)
#include <emmintrin.h>
#define GFX_HAVE_SIMD 1
#else
#define GFX_HAVE_SIMD 0
#endif

static uint32_t *front_fb = NULL;
static uint32_t *back_fb  = NULL;
static volatile int bsod_seized;
static uint32_t fb_width  = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch  = 0;
static uint8_t r_shift, g_shift, b_shift;

/* O4: compositor clip rectangle and pixel accumulator. */
static int32_t clip_x0, clip_y0;
static int32_t clip_x1, clip_y1;
static uint64_t px_written;

void gfx_clip_set(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    int64_t x0 = x;
    int64_t y0 = y;
    int64_t x1 = x0 + (int64_t)w;
    int64_t y1 = y0 + (int64_t)h;
    int64_t sw = (int64_t)fb_width;
    int64_t sh = (int64_t)fb_height;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 > sw) x0 = sw;
    if (y0 > sh) y0 = sh;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;

    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;

    clip_x0 = (int32_t)x0;
    clip_y0 = (int32_t)y0;
    clip_x1 = (int32_t)x1;
    clip_y1 = (int32_t)y1;
}

void gfx_clip_clear(void) {
    clip_x0 = 0;
    clip_y0 = 0;
    clip_x1 = (int32_t)fb_width;
    clip_y1 = (int32_t)fb_height;
}

/*
 * Intersect a signed-origin rectangle with the active clip rectangle and
 * framebuffer bounds. The returned source offsets preserve the original
 * source coordinates for blit and gradient operations.
 */
static int gfx_clip_rect_i64(
    int64_t x,
    int64_t y,
    uint32_t w,
    uint32_t h,
    int32_t *out_x0,
    int32_t *out_y0,
    int32_t *out_x1,
    int32_t *out_y1,
    uint32_t *src_x_off,
    uint32_t *src_y_off
) {
    if (w == 0 || h == 0 ||
        !out_x0 || !out_y0 || !out_x1 || !out_y1) {
        return 0;
    }

    int64_t cx0 = clip_x0;
    int64_t cy0 = clip_y0;
    int64_t cx1 = clip_x1;
    int64_t cy1 = clip_y1;
    int64_t sw = fb_width;
    int64_t sh = fb_height;

    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 < 0) cx1 = 0;
    if (cy1 < 0) cy1 = 0;
    if (cx0 > sw) cx0 = sw;
    if (cy0 > sh) cy0 = sh;
    if (cx1 > sw) cx1 = sw;
    if (cy1 > sh) cy1 = sh;

    if (cx0 >= cx1 || cy0 >= cy1) {
        return 0;
    }

    /* All callers pass int32/uint32 coordinates, so these sums fit int64. */
    int64_t rx1 = x + (int64_t)w;
    int64_t ry1 = y + (int64_t)h;

    int64_t x0 = (x > cx0) ? x : cx0;
    int64_t y0 = (y > cy0) ? y : cy0;
    int64_t x1 = (rx1 < cx1) ? rx1 : cx1;
    int64_t y1 = (ry1 < cy1) ? ry1 : cy1;

    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }

    *out_x0 = (int32_t)x0;
    *out_y0 = (int32_t)y0;
    *out_x1 = (int32_t)x1;
    *out_y1 = (int32_t)y1;

    if (src_x_off) *src_x_off = (uint32_t)(x0 - x);
    if (src_y_off) *src_y_off = (uint32_t)(y0 - y);

    return 1;
}

uint64_t gfx_take_px_counter(void) {
    uint64_t v = px_written;
    px_written = 0;
    return v;
}

/* Return the current clip rectangle. Useful for Rust-side culling. */
void gfx_get_clip(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) {
    if (x) *x = clip_x0;
    if (y) *y = clip_y0;
    if (w) *w = (uint32_t)(clip_x1 > clip_x0 ? clip_x1 - clip_x0 : 0);
    if (h) *h = (uint32_t)(clip_y1 > clip_y0 ? clip_y1 - clip_y0 : 0);
}

static color_t make_color(uint32_t rgb) {
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;

    return ((uint32_t)r << r_shift) |
           ((uint32_t)g << g_shift) |
           ((uint32_t)b << b_shift);
}

void gfx_init(void) {
    /* There is no teardown API; avoid leaking a previously initialized buffer. */
    if (front_fb || back_fb) {
        return;
    }

    boot_fb_t *fb = boot_get_framebuffer();

    if (!fb || fb->bpp != 32) {
        return;
    }

    uint64_t width = (uint64_t)fb->width;
    uint64_t height = (uint64_t)fb->height;
    uint64_t pitch = (uint64_t)fb->pitch;

    if (width == 0 || height == 0 ||
        width > INT32_MAX || height > INT32_MAX) {
        return;
    }

    if (pitch > UINT32_MAX ||
        pitch < width * sizeof(uint32_t) ||
        (pitch & (sizeof(uint32_t) - 1u)) != 0) {
        return;
    }

    if (fb->red_shift > 24 || fb->green_shift > 24 ||
        fb->blue_shift > 24 ||
        (fb->red_shift & 7u) != 0 ||
        (fb->green_shift & 7u) != 0 ||
        (fb->blue_shift & 7u) != 0 ||
        (uint32_t)(fb->red_shift > fb->green_shift
            ? fb->red_shift - fb->green_shift
            : fb->green_shift - fb->red_shift) < 8u ||
        (uint32_t)(fb->red_shift > fb->blue_shift
            ? fb->red_shift - fb->blue_shift
            : fb->blue_shift - fb->red_shift) < 8u ||
        (uint32_t)(fb->green_shift > fb->blue_shift
            ? fb->green_shift - fb->blue_shift
            : fb->blue_shift - fb->green_shift) < 8u) {
        return;
    }

    if (pitch > UINT64_MAX / height) {
        return;
    }

    uint64_t buf_bytes = pitch * height;
    if (buf_bytes == 0 || buf_bytes > (uint64_t)SIZE_MAX) {
        return;
    }

#if defined(__i386__)
    uint64_t phys = (uint64_t)fb->phys_base;
    if (buf_bytes > UINT32_MAX || phys > UINT32_MAX ||
        buf_bytes - 1u > UINT32_MAX - phys) {
        return;
    }

    front_fb = (uint32_t *)fb32_map_fb((uint32_t)phys, (uint32_t)buf_bytes);
    if (!front_fb) {
        return;
    }
#else
    uintptr_t hhdm = (uintptr_t)boot_get_hhdm_offset();
    uint64_t phys = (uint64_t)fb->phys_base;

    if (phys > (uint64_t)(UINTPTR_MAX - hhdm)) {
        return;
    }

    front_fb = (uint32_t *)(uintptr_t)(hhdm + (uintptr_t)phys);
#endif

    fb_width = (uint32_t)width;
    fb_height = (uint32_t)height;
    fb_pitch = (uint32_t)pitch;

    r_shift = fb->red_shift;
    g_shift = fb->green_shift;
    b_shift = fb->blue_shift;

    gfx_clip_clear();
    px_written = 0;

    back_fb = (uint32_t *)kmalloc((size_t)buf_bytes);
    if (!back_fb) {
        front_fb = NULL;
        fb_width = 0;
        fb_height = 0;
        fb_pitch = 0;
        gfx_clip_clear();
        return;
    }

    memset(back_fb, 0, (size_t)buf_bytes);
}

void gfx_putpixel(uint32_t x, uint32_t y, color_t color) {
    if (!back_fb || x >= fb_width || y >= fb_height) {
        return;
    }

    if ((int32_t)x < clip_x0 || (int32_t)x >= clip_x1 ||
        (int32_t)y < clip_y0 || (int32_t)y >= clip_y1) {
        return;
    }

    color_t packed = make_color(color);
    size_t pitch32 = fb_pitch / sizeof(uint32_t);

    back_fb[(size_t)y * pitch32 + x] = packed;
    px_written++;
}

/* Internal signed-coordinate pixel helper used by geometry primitives. */
static void gfx_putpixel_i64(int64_t x, int64_t y, color_t color) {
    if (x < 0 || y < 0 || x >= fb_width || y >= fb_height ||
        x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) {
        return;
    }

    gfx_putpixel((uint32_t)x, (uint32_t)y, color);
}

/*
 * SSE2 implementation of a 32-bit fill.
 *
 * This function is compiled separately with SSE2 enabled. The surrounding
 * graphics code remains compiled with the kernel's -mno-sse/-mno-sse2 flags.
 */
#if GFX_HAVE_SIMD
__attribute__((target("sse2")))
static void gfx_memset32_sse2(uint32_t *dst, uint32_t v, uint32_t n) {
    __m128i vv = _mm_set1_epi32((int)v);
    uint32_t i = 0;

    for (; i <= n && n - i >= 4; i += 4) {
        _mm_storeu_si128((__m128i *)(dst + i), vv);
    }

    for (; i < n; i++) {
        dst[i] = v;
    }
}
#endif

static void gfx_memset32(uint32_t *dst, uint32_t v, uint32_t n) {
#if GFX_HAVE_SIMD
    if (n >= 4) {
        gfx_memset32_sse2(dst, v, n);
        return;
    }
#endif

    for (uint32_t i = 0; i < n; i++) {
        dst[i] = v;
    }
}

static void gfx_fill_rect_i64(int64_t x, int64_t y,
                              uint32_t w, uint32_t h, color_t color) {
    if (!back_fb || w == 0 || h == 0) {
        return;
    }

    int32_t x0, y0, x1, y1;
    if (!gfx_clip_rect_i64(x, y, w, h,
                           &x0, &y0, &x1, &y1, NULL, NULL)) {
        return;
    }

    color_t packed = make_color(color);
    size_t pitch32 = fb_pitch / sizeof(uint32_t);
    uint32_t rw = (uint32_t)(x1 - x0);
    uint32_t rh = (uint32_t)(y1 - y0);

    for (int32_t py = y0; py < y1; py++) {
        gfx_memset32(
            back_fb + (size_t)py * pitch32 + (uint32_t)x0,
            packed,
            rw
        );
    }

    px_written += (uint64_t)rw * rh;
}

void gfx_fill_rect(uint32_t x, uint32_t y,
                   uint32_t w, uint32_t h,
                   color_t color) {
    gfx_fill_rect_i64((int64_t)x, (int64_t)y, w, h, color);
}

/*
 * SSE2 implementation of the RGB888 blit operation.
 *
 * The function is isolated behind a target attribute because the kernel
 * itself is compiled without SSE/SSE2 enabled.
 *
 * Mask convention:
 *   bit 1 = skip destination pixel
 *   bit 0 = copy source pixel
 *
 * Bits are MSB-first within each mask byte.
 */
#if GFX_HAVE_SIMD
__attribute__((target("sse2")))
static uint64_t gfx_blit_rgb888_sse2(
    int32_t dx0,
    int32_t dy0,
    uint32_t vis_w,
    uint32_t vis_h,
    uint32_t sx_off,
    uint32_t sy_off,
    const uint32_t *src,
    uint32_t src_stride,
    const uint8_t *skip_mask,
    uint32_t mask_stride,
    uint32_t pitch32
) {
    uint64_t stored = 0;

    for (uint32_t row = 0; row < vis_h; row++) {
        const uint32_t *s =
            src + ((size_t)sy_off + row) * src_stride + sx_off;

        uint32_t *d =
            back_fb + (size_t)(dy0 + (int32_t)row) * pitch32 + (uint32_t)dx0;

        const uint8_t *m = skip_mask
            ? skip_mask + ((size_t)sy_off + row) * mask_stride
            : NULL;

        uint32_t col = 0;

        if (m) {
            while (col < vis_w && ((sx_off + col) & 7u) != 0u) {
                uint32_t mask_bit = sx_off + col;
                uint32_t b = m[mask_bit >> 3];

                if (!((b >> (7u - (mask_bit & 7u))) & 1u)) {
                    d[col] = s[col];
                    stored++;
                }

                col++;
            }

            for (; col + 8 <= vis_w; col += 8) {
                uint32_t mask_byte_index = (sx_off + col) >> 3;
                uint8_t b = m[mask_byte_index];

                __m128i keep0 = _mm_setr_epi32(
                    ((b >> 7) & 1) ? -1 : 0,
                    ((b >> 6) & 1) ? -1 : 0,
                    ((b >> 5) & 1) ? -1 : 0,
                    ((b >> 4) & 1) ? -1 : 0
                );

                __m128i keep1 = _mm_setr_epi32(
                    ((b >> 3) & 1) ? -1 : 0,
                    ((b >> 2) & 1) ? -1 : 0,
                    ((b >> 1) & 1) ? -1 : 0,
                    (b & 1) ? -1 : 0
                );

                __m128i v0 =
                    _mm_loadu_si128((const __m128i *)(s + col));
                __m128i v1 =
                    _mm_loadu_si128((const __m128i *)(s + col + 4));
                __m128i d0 =
                    _mm_loadu_si128((const __m128i *)(d + col));
                __m128i d1 =
                    _mm_loadu_si128((const __m128i *)(d + col + 4));

                _mm_storeu_si128(
                    (__m128i *)(d + col),
                    _mm_or_si128(
                        _mm_and_si128(d0, keep0),
                        _mm_andnot_si128(keep0, v0)
                    )
                );

                _mm_storeu_si128(
                    (__m128i *)(d + col + 4),
                    _mm_or_si128(
                        _mm_and_si128(d1, keep1),
                        _mm_andnot_si128(keep1, v1)
                    )
                );

                stored += 8u -
                    (uint32_t)__builtin_popcount((unsigned)b);
            }

            for (; col < vis_w; col++) {
                uint32_t mask_bit = sx_off + col;
                uint32_t b = m[mask_bit >> 3];

                if (!((b >> (7u - (mask_bit & 7u))) & 1u)) {
                    d[col] = s[col];
                    stored++;
                }
            }
        } else {
            for (; col + 4 <= vis_w; col += 4) {
                _mm_storeu_si128(
                    (__m128i *)(d + col),
                    _mm_loadu_si128((const __m128i *)(s + col))
                );
            }

            for (; col < vis_w; col++) {
                d[col] = s[col];
            }

            stored += vis_w;
        }
    }

    return stored;
}
#endif

uint64_t gfx_blit_rgb888(
    int32_t x,
    int32_t y,
    uint32_t w,
    uint32_t h,
    const uint32_t *src,
    uint32_t src_stride,
    const uint8_t *skip_mask,
    uint32_t mask_stride
) {
    if (!back_fb || !src || w == 0 || h == 0 || src_stride < w) {
        return 0;
    }

    uint64_t source_last = (uint64_t)(h - 1u) * src_stride + (w - 1u);
    if (source_last > (uint64_t)(SIZE_MAX / sizeof(uint32_t))) {
        return 0;
    }

    if (skip_mask) {
        uint64_t mask_row_bytes = ((uint64_t)w + 7u) / 8u;
        uint64_t mask_last = (uint64_t)(h - 1u) * mask_stride + mask_row_bytes;
        if (mask_stride < mask_row_bytes || mask_last > SIZE_MAX) {
            return 0;
        }
    }

    int32_t dx0, dy0, dx1, dy1;
    uint32_t sx_off, sy_off;

    if (!gfx_clip_rect_i64(x, y, w, h,
                           &dx0, &dy0, &dx1, &dy1,
                           &sx_off, &sy_off)) {
        return 0;
    }

    uint32_t pitch32 = fb_pitch / sizeof(uint32_t);
    uint32_t vis_w = (uint32_t)(dx1 - dx0);
    uint32_t vis_h = (uint32_t)(dy1 - dy0);
    uint64_t stored = 0;

#if GFX_HAVE_SIMD
    stored = gfx_blit_rgb888_sse2(
        dx0, dy0, vis_w, vis_h,
        sx_off, sy_off,
        src, src_stride, skip_mask, mask_stride, pitch32
    );
    px_written += stored;
    return stored;
#else
    for (uint32_t row = 0; row < vis_h; row++) {
        const uint32_t *s =
            src + ((size_t)sy_off + row) * src_stride + sx_off;
        uint32_t *d =
            back_fb + (size_t)(dy0 + (int32_t)row) * pitch32 + (uint32_t)dx0;
        const uint8_t *m = skip_mask
            ? skip_mask + ((size_t)sy_off + row) * mask_stride
            : NULL;

        for (uint32_t col = 0; col < vis_w; col++) {
            if (m) {
                uint32_t mask_bit = sx_off + col;
                uint32_t b = m[mask_bit >> 3];
                if ((b >> (7u - (mask_bit & 7u))) & 1u) {
                    continue;
                }
            }

            d[col] = s[col];
            stored++;
        }
    }

    px_written += stored;
    return stored;
#endif
}

void gfx_draw_rect(uint32_t x, uint32_t y,
                   uint32_t w, uint32_t h,
                   color_t color) {
    if (w == 0 || h == 0) {
        return;
    }

    int64_t x0 = x;
    int64_t y0 = y;
    int64_t x1 = x0 + (int64_t)w - 1;
    int64_t y1 = y0 + (int64_t)h - 1;

    for (uint32_t i = 0; i < w; i++) {
        gfx_putpixel_i64(x0 + i, y0, color);
        gfx_putpixel_i64(x0 + i, y1, color);
    }

    for (uint32_t i = 0; i < h; i++) {
        gfx_putpixel_i64(x0, y0 + i, color);
        gfx_putpixel_i64(x1, y0 + i, color);
    }
}

/* Cohen-Sutherland outcodes. */
#define GFX_OC_LEFT   1
#define GFX_OC_RIGHT  2
#define GFX_OC_BOTTOM 4
#define GFX_OC_TOP    8

static int gfx_line_outcode(
    int64_t x,
    int64_t y,
    int64_t xmax,
    int64_t ymax
) {
    int code = 0;

    if (x < 0) {
        code |= GFX_OC_LEFT;
    } else if (x > xmax) {
        code |= GFX_OC_RIGHT;
    }

    if (y < 0) {
        code |= GFX_OC_BOTTOM;
    } else if (y > ymax) {
        code |= GFX_OC_TOP;
    }

    return code;
}

/*
 * Exact signed (a * b) / c for the bounded uint32-coordinate values used
 * by line clipping. The unsigned product fits in uint64_t.
 */
static int64_t gfx_muldiv_i64(int64_t a, int64_t b, int64_t c) {
    if (c == 0) {
        return 0;
    }

    uint64_t ua = (uint64_t)(a < 0 ? -a : a);
    uint64_t ub = (uint64_t)(b < 0 ? -b : b);
    uint64_t uc = (uint64_t)(c < 0 ? -c : c);
    uint64_t quotient = (ua * ub) / uc;
    int negative = ((a < 0) != (b < 0)) != (c < 0);

    return negative ? -(int64_t)quotient : (int64_t)quotient;
}

void gfx_draw_line(
    uint32_t x0u,
    uint32_t y0u,
    uint32_t x1u,
    uint32_t y1u,
    color_t color
) {
    if (!back_fb || fb_width == 0 || fb_height == 0) {
        return;
    }

    int64_t x0 = x0u;
    int64_t y0 = y0u;
    int64_t x1 = x1u;
    int64_t y1 = y1u;
    const int64_t xmax = (int64_t)fb_width - 1;
    const int64_t ymax = (int64_t)fb_height - 1;

    for (;;) {
        int c0 = gfx_line_outcode(x0, y0, xmax, ymax);
        int c1 = gfx_line_outcode(x1, y1, xmax, ymax);

        if (!(c0 | c1)) {
            break;
        }

        if (c0 & c1) {
            return;
        }

        int c = c0 ? c0 : c1;
        int64_t nx;
        int64_t ny;
        int64_t dx = x1 - x0;
        int64_t dy = y1 - y0;

        if (c & GFX_OC_LEFT) {
            if (dx == 0) return;
            nx = 0;
            ny = y0 + gfx_muldiv_i64(dy, -x0, dx);
        } else if (c & GFX_OC_RIGHT) {
            if (dx == 0) return;
            nx = xmax;
            ny = y0 + gfx_muldiv_i64(dy, xmax - x0, dx);
        } else if (c & GFX_OC_BOTTOM) {
            if (dy == 0) return;
            ny = 0;
            nx = x0 + gfx_muldiv_i64(dx, -y0, dy);
        } else {
            if (dy == 0) return;
            ny = ymax;
            nx = x0 + gfx_muldiv_i64(dx, ymax - y0, dy);
        }

        if (c == c0) {
            x0 = nx;
            y0 = ny;
        } else {
            x1 = nx;
            y1 = ny;
        }
    }

    int64_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int64_t dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int64_t sx = (x0 < x1) ? 1 : -1;
    int64_t sy = (y0 < y1) ? 1 : -1;
    int64_t err = dx - dy;

    for (;;) {
        gfx_putpixel_i64(x0, y0, color);

        if (x0 == x1 && y0 == y1) {
            break;
        }

        int64_t e2 = 2 * err;

        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }

        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_draw_string(
    uint32_t x,
    uint32_t y,
    const char *s,
    color_t color
) {
    if (!s) {
        return;
    }

    const struct psf_font *f = psf_get_font();
    if (!f || !f->data) {
        return;
    }

    int64_t cx = x;
    int64_t cy = y;
    const int64_t origin_x = x;

    for (; *s; s++) {
        if (*s == '\n') {
            cx = origin_x;
            cy += f->height;
            continue;
        }

        uint32_t idx = (unsigned char)*s;
        if (idx >= f->num_glyphs) {
            idx = 0;
        }

        const uint8_t *glyph =
            f->data + (uint64_t)idx * f->bytes_per_glyph;

        for (uint32_t gy = 0; gy < f->height; gy++) {
            const uint8_t *row_bytes = glyph + (size_t)gy * f->bytes_per_row;

            for (uint32_t gx = 0; gx < f->width; gx++) {
                uint8_t byte = row_bytes[gx / 8];
                uint8_t bit_pos = (uint8_t)(7u - (gx % 8u));

                if ((byte >> bit_pos) & 1u) {
                    gfx_putpixel_i64(cx + gx, cy + gy, color);
                }
            }
        }

        cx += f->width;
    }
}

void gfx_draw_text(
    uint32_t x,
    uint32_t y,
    const char *s,
    color_t color
) {
    gfx_draw_string(x, y, s, color);
}

uint32_t gfx_text_width(const char *s) {
    const struct psf_font *f = psf_get_font();
    uint32_t glyph_w = f ? f->width : 8;
    uint64_t width = 0;

    while (s && *s) {
        if (*s == '\n') {
            break;
        }

        width += glyph_w;
        if (width > UINT32_MAX) {
            return UINT32_MAX;
        }
        s++;
    }

    return (uint32_t)width;
}

void gfx_draw_text_centered(
    uint32_t y,
    const char *s,
    color_t color
) {
    uint32_t tw = gfx_text_width(s);
    uint32_t x = (tw < fb_width) ? ((fb_width - tw) / 2) : 0;
    gfx_draw_string(x, y, s, color);
}

color_t gfx_blend(color_t a, color_t b, uint8_t t) {
    uint32_t ar = (a >> 16) & 0xFF;
    uint32_t ag = (a >> 8) & 0xFF;
    uint32_t ab = a & 0xFF;

    uint32_t br = (b >> 16) & 0xFF;
    uint32_t bg = (b >> 8) & 0xFF;
    uint32_t bb = b & 0xFF;

    uint32_t r = (ar * (255u - t) + br * t) / 255u;
    uint32_t g = (ag * (255u - t) + bg * t) / 255u;
    uint32_t bl = (ab * (255u - t) + bb * t) / 255u;

    return (r << 16) | (g << 8) | bl;
}

/* -------------------------------------------------------------------------
 * MSDF / anti-aliasing blending primitives.
 *
 * The whole pipeline works with native 32-bit pixels (already shifted
 * per the framebuffer layout). Blending is done per byte. The source alpha
 * byte is zero and the destination alpha byte is not otherwise maintained.
 * ------------------------------------------------------------------------- */

/* x / 255 for x in [0, 65025]; exact with the +1 + (x>>8) trick. */
static inline uint32_t gfx_div255(uint32_t x) {
    return (x + 1u + (x >> 8)) >> 8;
}

/* Blend one native pixel in place: dst = dst*(1-a) + src*a. */
static inline uint32_t gfx_blend_native_u32(uint32_t d, uint32_t s, uint32_t a) {
    uint32_t inv = 255u - a;

    uint32_t b0 = gfx_div255((d & 0xFFu) * inv + (s & 0xFFu) * a);
    uint32_t b1 = gfx_div255(((d >> 8) & 0xFFu) * inv + ((s >> 8) & 0xFFu) * a);
    uint32_t b2 = gfx_div255(((d >> 16) & 0xFFu) * inv + ((s >> 16) & 0xFFu) * a);
    uint32_t b3 = gfx_div255(((d >> 24) & 0xFFu) * inv + ((s >> 24) & 0xFFu) * a);

    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

#if GFX_HAVE_SIMD
/* Blend 4 pixels with four distinct alphas. */
__attribute__((target("sse2")))
static void gfx_blend4_sse2_native(uint32_t *dst, uint32_t src_native,
                                   uint32_t a0, uint32_t a1,
                                   uint32_t a2, uint32_t a3) {
    __m128i zero = _mm_setzero_si128();
    __m128i v_src = _mm_set1_epi32((int)src_native);
    __m128i a_lo = _mm_setr_epi16(
        (int16_t)a0, (int16_t)a0, (int16_t)a0, (int16_t)a0,
        (int16_t)a1, (int16_t)a1, (int16_t)a1, (int16_t)a1);
    __m128i a_hi = _mm_setr_epi16(
        (int16_t)a2, (int16_t)a2, (int16_t)a2, (int16_t)a2,
        (int16_t)a3, (int16_t)a3, (int16_t)a3, (int16_t)a3);
    __m128i inv_lo = _mm_sub_epi16(_mm_set1_epi16(255), a_lo);
    __m128i inv_hi = _mm_sub_epi16(_mm_set1_epi16(255), a_hi);
    __m128i one = _mm_set1_epi16(1);

    __m128i d = _mm_loadu_si128((__m128i *)dst);
    __m128i d_lo = _mm_unpacklo_epi8(d, zero);
    __m128i d_hi = _mm_unpackhi_epi8(d, zero);
    __m128i s_lo = _mm_unpacklo_epi8(v_src, zero);
    __m128i s_hi = _mm_unpackhi_epi8(v_src, zero);

    __m128i p_lo = _mm_add_epi16(_mm_mullo_epi16(d_lo, inv_lo),
                                 _mm_mullo_epi16(s_lo, a_lo));
    __m128i p_hi = _mm_add_epi16(_mm_mullo_epi16(d_hi, inv_hi),
                                 _mm_mullo_epi16(s_hi, a_hi));
    p_lo = _mm_add_epi16(_mm_add_epi16(p_lo, one), _mm_srli_epi16(p_lo, 8));
    p_hi = _mm_add_epi16(_mm_add_epi16(p_hi, one), _mm_srli_epi16(p_hi, 8));
    p_lo = _mm_srli_epi16(p_lo, 8);
    p_hi = _mm_srli_epi16(p_hi, 8);

    _mm_storeu_si128((__m128i *)dst, _mm_packus_epi16(p_lo, p_hi));
}

/* Blend 4 pixels with the same alpha. */
__attribute__((target("sse2")))
static void gfx_fill4_sse2_native(uint32_t *dst, uint32_t src_native,
                                  uint32_t alpha) {
    __m128i zero = _mm_setzero_si128();
    __m128i v_src = _mm_set1_epi32((int)src_native);
    __m128i a = _mm_set1_epi16((int16_t)alpha);
    __m128i inv = _mm_sub_epi16(_mm_set1_epi16(255), a);
    __m128i one = _mm_set1_epi16(1);

    __m128i d = _mm_loadu_si128((__m128i *)dst);
    __m128i d_lo = _mm_unpacklo_epi8(d, zero);
    __m128i d_hi = _mm_unpackhi_epi8(d, zero);
    __m128i s_lo = _mm_unpacklo_epi8(v_src, zero);
    __m128i s_hi = _mm_unpackhi_epi8(v_src, zero);

    __m128i p_lo = _mm_add_epi16(_mm_mullo_epi16(d_lo, inv),
                                 _mm_mullo_epi16(s_lo, a));
    __m128i p_hi = _mm_add_epi16(_mm_mullo_epi16(d_hi, inv),
                                 _mm_mullo_epi16(s_hi, a));
    p_lo = _mm_add_epi16(_mm_add_epi16(p_lo, one), _mm_srli_epi16(p_lo, 8));
    p_hi = _mm_add_epi16(_mm_add_epi16(p_hi, one), _mm_srli_epi16(p_hi, 8));
    p_lo = _mm_srli_epi16(p_lo, 8);
    p_hi = _mm_srli_epi16(p_hi, 8);

    _mm_storeu_si128((__m128i *)dst, _mm_packus_epi16(p_lo, p_hi));
}
#endif

#if !GFX_HAVE_SIMD
static void gfx_blend4_scalar_native(uint32_t *dst, uint32_t src_native,
                                     uint32_t a0, uint32_t a1,
                                     uint32_t a2, uint32_t a3) {
    dst[0] = gfx_blend_native_u32(dst[0], src_native, a0);
    dst[1] = gfx_blend_native_u32(dst[1], src_native, a1);
    dst[2] = gfx_blend_native_u32(dst[2], src_native, a2);
    dst[3] = gfx_blend_native_u32(dst[3], src_native, a3);
}

static void gfx_fill4_scalar_native(uint32_t *dst, uint32_t src_native,
                                    uint32_t alpha) {
    dst[0] = gfx_blend_native_u32(dst[0], src_native, alpha);
    dst[1] = gfx_blend_native_u32(dst[1], src_native, alpha);
    dst[2] = gfx_blend_native_u32(dst[2], src_native, alpha);
    dst[3] = gfx_blend_native_u32(dst[3], src_native, alpha);
}
#endif

static inline void gfx_blend4_native(uint32_t *dst, uint32_t src_native,
                                     uint32_t a0, uint32_t a1,
                                     uint32_t a2, uint32_t a3) {
#if GFX_HAVE_SIMD
    gfx_blend4_sse2_native(dst, src_native, a0, a1, a2, a3);
#else
    gfx_blend4_scalar_native(dst, src_native, a0, a1, a2, a3);
#endif
}

static inline void gfx_fill4_native(uint32_t *dst, uint32_t src_native,
                                    uint32_t alpha) {
#if GFX_HAVE_SIMD
    gfx_fill4_sse2_native(dst, src_native, alpha);
#else
    gfx_fill4_scalar_native(dst, src_native, alpha);
#endif
}

/* Blend one pixel using a scalar reference implementation. */
void gfx_blend_pixel(int32_t x, int32_t y, uint32_t color, uint8_t alpha) {
    if (!back_fb || alpha == 0) return;
    if (x < 0 || y < 0 || x >= (int32_t)fb_width || y >= (int32_t)fb_height) return;
    if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;

    size_t pitch32 = fb_pitch / sizeof(uint32_t);
    uint32_t *p = &back_fb[(size_t)(uint32_t)y * pitch32 + (uint32_t)x];

    if (alpha == 255) {
        *p = make_color(color);
    } else {
        *p = gfx_blend_native_u32(*p, make_color(color), alpha);
    }
    px_written++;
}

/*
 * MSDF glyph runs are horizontal, so blend 4 adjacent pixels with 4
 * per-pixel alphas in one SSE2 pass. Partially visible runs use scalar blending.
 */
void gfx_blend_pixels4(int32_t x, int32_t y, uint32_t color, const uint8_t alpha[4]) {
    if (!back_fb || !alpha) return;
    if (y < 0 || y >= (int32_t)fb_height || y < clip_y0 || y >= clip_y1) return;

    int64_t run_x0 = x;
    int64_t run_x1 = run_x0 + 4;
    int64_t left = run_x0;
    int64_t right = run_x1;

    if (left < clip_x0) left = clip_x0;
    if (left < 0) left = 0;
    if (right > clip_x1) right = clip_x1;
    if (right > fb_width) right = fb_width;
    if (left >= right) return;

    uint32_t start = (uint32_t)(left - run_x0);
    uint32_t end = (uint32_t)(right - run_x0);
    size_t pitch32 = fb_pitch / sizeof(uint32_t);
    uint32_t src_native = make_color(color);

    if (start == 0 && end == 4) {
        uint32_t *dst = &back_fb[(size_t)(uint32_t)y * pitch32 + (uint32_t)x];
        gfx_blend4_native(dst, src_native, alpha[0], alpha[1], alpha[2], alpha[3]);
        px_written += 4;
        return;
    }

    for (uint32_t i = start; i < end; i++) {
        if (alpha[i] == 0) continue;
        uint32_t *p = &back_fb[(size_t)(uint32_t)y * pitch32 + (size_t)left + (i - start)];
        if (alpha[i] == 255) *p = src_native;
        else *p = gfx_blend_native_u32(*p, src_native, alpha[i]);
    }

    px_written += (uint64_t)(end - start);
}

/*
 * Blend a whole horizontal run of pixels in a single call. Removes
 * per-pixel FFI overhead when the Rust glyph renderer feeds a full glyph row.
 */
void gfx_blend_span(int32_t x, int32_t y, uint32_t count,
                    uint32_t color, const uint8_t *alphas) {
    if (!back_fb || count == 0 || !alphas) return;
    if (y < 0 || y >= (int32_t)fb_height || y < clip_y0 || y >= clip_y1) return;

    int64_t run_x0 = x;
    int64_t run_x1 = run_x0 + (int64_t)count;
    int64_t left = run_x0;
    int64_t right = run_x1;

    if (left < clip_x0) left = clip_x0;
    if (left < 0) left = 0;
    if (right > clip_x1) right = clip_x1;
    if (right > fb_width) right = fb_width;
    if (left >= right) return;

    uint32_t start = (uint32_t)(left - run_x0);
    uint32_t visible = (uint32_t)(right - left);
    size_t pitch32 = fb_pitch / sizeof(uint32_t);
    uint32_t src_native = make_color(color);
    uint32_t *dst = &back_fb[(size_t)(uint32_t)y * pitch32 + (size_t)left];

    uint32_t i = 0;
    for (; i + 4 <= visible; i += 4) {
        gfx_blend4_native(dst + i, src_native,
                          alphas[start + i], alphas[start + i + 1],
                          alphas[start + i + 2], alphas[start + i + 3]);
    }

    for (; i < visible; i++) {
        uint8_t a = alphas[start + i];
        if (a == 0) continue;
        uint32_t *p = &dst[i];
        if (a == 255) *p = src_native;
        else *p = gfx_blend_native_u32(*p, src_native, a);
    }

    px_written += visible;
}

/* Translucent rectangle fill with scalar and SSE2 paths. */
void gfx_fill_rect_alpha(int32_t x, int32_t y, uint32_t w, uint32_t h,
                         uint32_t color, uint8_t alpha) {
    if (!back_fb || w == 0 || h == 0 || alpha == 0) return;

    int32_t x0, y0, x1, y1;
    if (!gfx_clip_rect_i64(x, y, w, h,
                           &x0, &y0, &x1, &y1, NULL, NULL)) {
        return;
    }

    if (alpha == 255) {
        gfx_fill_rect_i64(x0, y0,
                          (uint32_t)(x1 - x0),
                          (uint32_t)(y1 - y0), color);
        return;
    }

    size_t pitch32 = fb_pitch / sizeof(uint32_t);
    uint32_t src_native = make_color(color);
    uint32_t rw = (uint32_t)(x1 - x0);
    uint32_t rh = (uint32_t)(y1 - y0);

    for (int32_t py = y0; py < y1; py++) {
        uint32_t *row = &back_fb[(size_t)py * pitch32 + (uint32_t)x0];
        uint32_t i = 0;

        for (; i + 4 <= rw; i += 4) {
            gfx_fill4_native(row + i, src_native, alpha);
        }
        for (; i < rw; i++) {
            row[i] = gfx_blend_native_u32(row[i], src_native, alpha);
        }
    }

    px_written += (uint64_t)rw * rh;
}

/* Per-pixel alpha blit from a 0xAARRGGBB source. */
void gfx_blit_alpha(int32_t dst_x, int32_t dst_y, uint32_t w, uint32_t h,
                    const uint32_t *src, uint32_t stride) {
    if (!back_fb || !src || w == 0 || h == 0 || stride < w) return;

    uint64_t source_last = (uint64_t)(h - 1u) * stride + (w - 1u);
    if (source_last > (uint64_t)(SIZE_MAX / sizeof(uint32_t))) return;

    int32_t dx0, dy0, dx1, dy1;
    uint32_t sx_off, sy_off;
    if (!gfx_clip_rect_i64(dst_x, dst_y, w, h,
                           &dx0, &dy0, &dx1, &dy1,
                           &sx_off, &sy_off)) {
        return;
    }

    size_t pitch32 = fb_pitch / sizeof(uint32_t);
    uint64_t written = 0;

    for (int32_t row = 0; row < dy1 - dy0; row++) {
        const uint32_t *s =
            src + ((size_t)sy_off + (uint32_t)row) * stride + sx_off;
        uint32_t *d =
            &back_fb[(size_t)(dy0 + row) * pitch32 + (uint32_t)dx0];

        for (int32_t col = 0; col < dx1 - dx0; col++) {
            uint32_t sp = s[col];
            uint32_t sa = (sp >> 24) & 0xFFu;
            if (sa == 0) continue;

            uint32_t native = make_color(sp & 0x00FFFFFFu);
            if (sa == 255) d[col] = native;
            else d[col] = gfx_blend_native_u32(d[col], native, sa);
            written++;
        }
    }

    px_written += written;
}

/*
 * Two-pass box blur over the clipped back-buffer region. Both passes use
 * sliding-window sums per channel. Allocation failure leaves the buffer
 * unchanged.
 */
void gfx_box_blur_rect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                       uint32_t radius) {
    if (!back_fb || w < 2 || h < 2 || radius == 0) return;

    int32_t x0, y0, x1, y1;
    if (!gfx_clip_rect_i64(x, y, w, h,
                           &x0, &y0, &x1, &y1, NULL, NULL)) {
        return;
    }

    uint32_t bw = (uint32_t)(x1 - x0);
    uint32_t bh = (uint32_t)(y1 - y0);
    if (bw < 2 || bh < 2) return;

    if (radius > bw / 2) radius = bw / 2;
    if (radius > bh / 2) radius = bh / 2;
    if (radius == 0) return;

    uint64_t pixels = (uint64_t)bw * bh;
    if (pixels > (uint64_t)(SIZE_MAX / sizeof(uint32_t))) return;
    size_t tmp_bytes = (size_t)pixels * sizeof(uint32_t);

    uint32_t *tmp = (uint32_t *)kmalloc(tmp_bytes);
    if (!tmp) return;

    size_t pitch32 = fb_pitch / sizeof(uint32_t);
    uint32_t win = radius * 2u + 1u;

    /* Horizontal pass: back buffer region -> temporary buffer. */
    for (uint32_t row = 0; row < bh; row++) {
        const uint32_t *src =
            &back_fb[(size_t)(y0 + (int32_t)row) * pitch32 + (uint32_t)x0];
        uint32_t *dst = tmp + (size_t)row * bw;
        uint64_t sr = 0, sg = 0, sb = 0;

        for (uint32_t i = 0; i <= radius; i++) {
            uint32_t idx = (i < bw) ? i : bw - 1;
            uint32_t p = src[idx];
            sr += (p >> r_shift) & 0xFFu;
            sg += (p >> g_shift) & 0xFFu;
            sb += (p >> b_shift) & 0xFFu;
        }

        uint32_t p0 = src[0];
        sr += (uint64_t)((p0 >> r_shift) & 0xFFu) * radius;
        sg += (uint64_t)((p0 >> g_shift) & 0xFFu) * radius;
        sb += (uint64_t)((p0 >> b_shift) & 0xFFu) * radius;

        for (uint32_t px = 0; px < bw; px++) {
            uint32_t r = (uint32_t)(sr / win);
            uint32_t g = (uint32_t)(sg / win);
            uint32_t b = (uint32_t)(sb / win);
            dst[px] = (r << r_shift) | (g << g_shift) | (b << b_shift);

            uint32_t rem = (px > radius) ? px - radius : 0;
            uint64_t add64 = (uint64_t)px + radius + 1u;
            uint32_t add = (add64 < bw) ? (uint32_t)add64 : bw - 1;
            uint32_t pr = src[rem];
            uint32_t pa = src[add];

            sr -= (pr >> r_shift) & 0xFFu;
            sg -= (pr >> g_shift) & 0xFFu;
            sb -= (pr >> b_shift) & 0xFFu;
            sr += (pa >> r_shift) & 0xFFu;
            sg += (pa >> g_shift) & 0xFFu;
            sb += (pa >> b_shift) & 0xFFu;
        }
    }

    /* Vertical pass: temporary buffer -> back buffer region. */
    for (uint32_t col = 0; col < bw; col++) {
        uint64_t sr = 0, sg = 0, sb = 0;

        for (uint32_t i = 0; i <= radius; i++) {
            uint32_t idx = (i < bh) ? i : bh - 1;
            uint32_t p = tmp[(size_t)idx * bw + col];
            sr += (p >> r_shift) & 0xFFu;
            sg += (p >> g_shift) & 0xFFu;
            sb += (p >> b_shift) & 0xFFu;
        }

        uint32_t p0 = tmp[col];
        sr += (uint64_t)((p0 >> r_shift) & 0xFFu) * radius;
        sg += (uint64_t)((p0 >> g_shift) & 0xFFu) * radius;
        sb += (uint64_t)((p0 >> b_shift) & 0xFFu) * radius;

        for (uint32_t py = 0; py < bh; py++) {
            uint32_t r = (uint32_t)(sr / win);
            uint32_t g = (uint32_t)(sg / win);
            uint32_t b = (uint32_t)(sb / win);
            back_fb[(size_t)(y0 + (int32_t)py) * pitch32 + (uint32_t)x0 + col] =
                (r << r_shift) | (g << g_shift) | (b << b_shift);

            uint32_t rem = (py > radius) ? py - radius : 0;
            uint64_t add64 = (uint64_t)py + radius + 1u;
            uint32_t add = (add64 < bh) ? (uint32_t)add64 : bh - 1;
            uint32_t pr = tmp[(size_t)rem * bw + col];
            uint32_t pa = tmp[(size_t)add * bw + col];

            sr -= (pr >> r_shift) & 0xFFu;
            sg -= (pr >> g_shift) & 0xFFu;
            sb -= (pr >> b_shift) & 0xFFu;
            sr += (pa >> r_shift) & 0xFFu;
            sg += (pa >> g_shift) & 0xFFu;
            sb += (pa >> b_shift) & 0xFFu;
        }
    }

    px_written += pixels;
    kfree(tmp);
}

/*
 * Coverage of one pixel against a circle centred at (ccx, ccy) with
 * radius r, using 4x4 supersampling. Large radii are scaled before squaring
 * to keep the integer products within uint64_t.
 */
static uint8_t gfx_circle_coverage(int32_t px, int32_t py,
                                   int64_t ccx, int64_t ccy, uint32_t r) {
    uint64_t radius = (uint64_t)r * 4u;
    int scale = radius > UINT32_MAX;
    uint64_t scaled_radius = scale ? radius >> 1 : radius;
    uint64_t radius2 = scaled_radius * scaled_radius;
    int inside = 0;

    for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
            int64_t dx = (int64_t)px * 4 + sx * 2 + 1 - ccx * 4;
            int64_t dy = (int64_t)py * 4 + sy * 2 + 1 - ccy * 4;
            uint64_t ax = (uint64_t)(dx < 0 ? -dx : dx);
            uint64_t ay = (uint64_t)(dy < 0 ? -dy : dy);

            if (scale) {
                ax >>= 1;
                ay >>= 1;
            }

            if (ax > scaled_radius || ay > scaled_radius) continue;

            uint64_t ax2 = ax * ax;
            uint64_t ay2 = ay * ay;
            if (ay2 <= radius2 && ax2 <= radius2 - ay2) inside++;
        }
    }

    return (uint8_t)(inside * 255 / 16);
}

/* Blend a single pixel's coverage into the back buffer. */
static void gfx_aa_pixel(int32_t px, int32_t py, uint32_t color, uint8_t cov) {
    if (cov == 0) return;
    if (cov == 255) gfx_putpixel((uint32_t)px, (uint32_t)py, color);
    else gfx_blend_pixel(px, py, color, cov);
}

/*
 * AA rounded rectangle: interior strips are filled opaquely, then the four
 * clipped corner boxes are rasterized using per-pixel coverage.
 */
void gfx_fill_round_rect_aa(int32_t x, int32_t y, uint32_t w, uint32_t h,
                            uint32_t r, uint32_t color) {
    if (!back_fb || w == 0 || h == 0) return;

    if (r > w / 2u) r = w / 2u;
    if (r > h / 2u) r = h / 2u;

    if (r == 0) {
        gfx_fill_rect_i64(x, y, w, h, color);
        return;
    }

    int64_t ir = r;
    int64_t right = (int64_t)x + w;
    int64_t bottom = (int64_t)y + h;

    gfx_fill_rect_i64((int64_t)x + ir, y, w - 2u * r, r, color);
    gfx_fill_rect_i64((int64_t)x + ir, bottom - ir,
                      w - 2u * r, r, color);
    gfx_fill_rect_i64(x, (int64_t)y + ir,
                      w, h - 2u * r, color);

    struct {
        int64_t x, y, cx, cy;
    } corners[4] = {
        { x,          y,          (int64_t)x + ir,  (int64_t)y + ir },
        { right - ir, y,          right - ir,       (int64_t)y + ir },
        { x,          bottom-ir,  (int64_t)x + ir,  bottom - ir },
        { right-ir,   bottom-ir,  right - ir,       bottom - ir }
    };

    for (int c = 0; c < 4; c++) {
        int32_t bx0, by0, bx1, by1;
        if (!gfx_clip_rect_i64(corners[c].x, corners[c].y, r, r,
                               &bx0, &by0, &bx1, &by1, NULL, NULL)) {
            continue;
        }

        for (int32_t py = by0; py < by1; py++) {
            for (int32_t px = bx0; px < bx1; px++) {
                uint8_t cov = gfx_circle_coverage(px, py,
                                                  corners[c].cx,
                                                  corners[c].cy, r);
                gfx_aa_pixel(px, py, color, cov);
            }
        }
    }
}

/* Linear interpolation helpers for the four-corner gradient. */
static inline uint32_t gfx_lerp8(uint32_t a, uint32_t b, uint32_t t256) {
    return (a * (256u - t256) + b * t256) >> 8;
}

/* Four-corner bilinear gradient with 8.8 fixed-point interpolation. */
void gfx_gradient_rect_4(int32_t x, int32_t y, uint32_t w, uint32_t h,
                         uint32_t c_tl, uint32_t c_tr,
                         uint32_t c_bl, uint32_t c_br) {
    if (!back_fb || w == 0 || h == 0) return;

    int32_t x0, y0, x1, y1;
    uint32_t sx_off, sy_off;
    if (!gfx_clip_rect_i64(x, y, w, h,
                           &x0, &y0, &x1, &y1,
                           &sx_off, &sy_off)) {
        return;
    }

    uint32_t r_tl = (c_tl >> 16) & 0xFFu;
    uint32_t g_tl = (c_tl >> 8) & 0xFFu;
    uint32_t b_tl = c_tl & 0xFFu;
    uint32_t r_tr = (c_tr >> 16) & 0xFFu;
    uint32_t g_tr = (c_tr >> 8) & 0xFFu;
    uint32_t b_tr = c_tr & 0xFFu;
    uint32_t r_bl = (c_bl >> 16) & 0xFFu;
    uint32_t g_bl = (c_bl >> 8) & 0xFFu;
    uint32_t b_bl = c_bl & 0xFFu;
    uint32_t r_br = (c_br >> 16) & 0xFFu;
    uint32_t g_br = (c_br >> 8) & 0xFFu;
    uint32_t b_br = c_br & 0xFFu;

    size_t pitch32 = fb_pitch / sizeof(uint32_t);
    uint32_t dw = (uint32_t)(x1 - x0);
    uint32_t dh = (uint32_t)(y1 - y0);

    for (uint32_t j = 0; j < dh; j++) {
        uint32_t ty256 = (h > 1)
            ? (uint32_t)(((uint64_t)sy_off + j) * 256u / (h - 1u))
            : 0u;
        uint32_t *row =
            &back_fb[(size_t)(y0 + (int32_t)j) * pitch32 + (uint32_t)x0];

        for (uint32_t i = 0; i < dw; i++) {
            uint32_t tx256 = (w > 1)
                ? (uint32_t)(((uint64_t)sx_off + i) * 256u / (w - 1u))
                : 0u;

            uint32_t r_top = gfx_lerp8(r_tl, r_tr, tx256);
            uint32_t g_top = gfx_lerp8(g_tl, g_tr, tx256);
            uint32_t b_top = gfx_lerp8(b_tl, b_tr, tx256);
            uint32_t r_bot = gfx_lerp8(r_bl, r_br, tx256);
            uint32_t g_bot = gfx_lerp8(g_bl, g_br, tx256);
            uint32_t b_bot = gfx_lerp8(b_bl, b_br, tx256);

            uint32_t r = gfx_lerp8(r_top, r_bot, ty256);
            uint32_t g = gfx_lerp8(g_top, g_bot, ty256);
            uint32_t b = gfx_lerp8(b_top, b_bot, ty256);
            row[i] = make_color((r << 16) | (g << 8) | b);
        }
    }

    px_written += (uint64_t)dw * dh;
}

void gfx_gradient_v(
    uint32_t x,
    uint32_t y,
    uint32_t w,
    uint32_t h,
    color_t top,
    color_t bottom
) {
    if (!back_fb || h == 0) return;

    for (uint32_t row = 0; row < h; row++) {
        uint8_t t = (h > 1)
            ? (uint8_t)(((uint64_t)row * 255u) / (h - 1u))
            : 0;
        color_t c = gfx_blend(top, bottom, t);
        uint64_t py = (uint64_t)y + row;
        if (py > UINT32_MAX) break;
        gfx_fill_rect(x, (uint32_t)py, w, 1, c);
    }
}

void gfx_draw_circle(
    uint32_t cx,
    uint32_t cy,
    uint32_t r,
    color_t color
) {
    if (!back_fb || fb_width == 0 || fb_height == 0 ||
        clip_x0 >= clip_x1 || clip_y0 >= clip_y1) return;

    int64_t icx = cx;
    int64_t icy = cy;
    int64_t radius = r;

    /* Reject circles whose bounding box cannot intersect the active clip. */
    if (icx + radius < clip_x0 || icy + radius < clip_y0 ||
        icx - radius >= clip_x1 || icy - radius >= clip_y1) {
        return;
    }

    int64_t x = radius;
    int64_t y = 0;
    int64_t err = 0;

    while (x >= y) {
        gfx_putpixel_i64(icx + x, icy + y, color);
        gfx_putpixel_i64(icx + y, icy + x, color);
        gfx_putpixel_i64(icx - y, icy + x, color);
        gfx_putpixel_i64(icx - x, icy + y, color);
        gfx_putpixel_i64(icx - x, icy - y, color);
        gfx_putpixel_i64(icx - y, icy - x, color);
        gfx_putpixel_i64(icx + y, icy - x, color);
        gfx_putpixel_i64(icx + x, icy - y, color);

        if (err <= 0) {
            y++;
            err += 2 * y + 1;
        }

        if (err > 0) {
            x--;
            err -= 2 * x + 1;
        }
    }
}

void gfx_fill_circle(
    uint32_t cx,
    uint32_t cy,
    uint32_t r,
    color_t color
) {
    if (!back_fb || fb_width == 0 || fb_height == 0) return;

    int64_t icx = cx;
    int64_t icy = cy;
    int64_t radius = r;
    uint64_t radius2 = (uint64_t)r * r;

    int64_t x0 = icx - radius;
    int64_t y0 = icy - radius;
    int64_t x1 = icx + radius;
    int64_t y1 = icy + radius;

    if (x0 < clip_x0) x0 = clip_x0;
    if (y0 < clip_y0) y0 = clip_y0;
    if (x1 >= clip_x1) x1 = (int64_t)clip_x1 - 1;
    if (y1 >= clip_y1) y1 = (int64_t)clip_y1 - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= fb_width) x1 = (int64_t)fb_width - 1;
    if (y1 >= fb_height) y1 = (int64_t)fb_height - 1;
    if (x0 > x1 || y0 > y1) return;

    for (int64_t py = y0; py <= y1; py++) {
        int64_t dy = py - icy;
        uint64_t ady = (uint64_t)(dy < 0 ? -dy : dy);
        uint64_t dy2 = ady * ady;
        if (dy2 > radius2) continue;
        uint64_t max_dx2 = radius2 - dy2;

        for (int64_t px = x0; px <= x1; px++) {
            int64_t dx = px - icx;
            uint64_t adx = (uint64_t)(dx < 0 ? -dx : dx);
            if (adx * adx <= max_dx2) {
                gfx_putpixel((uint32_t)px, (uint32_t)py, color);
            }
        }
    }
}

void gfx_back_get_rect(
    int32_t x,
    int32_t y,
    uint32_t w,
    uint32_t h,
    uint32_t *dst
) {
    if (!back_fb || !dst || w == 0 || h == 0) return;

    uint64_t pixels = (uint64_t)w * h;
    if (pixels > (uint64_t)(SIZE_MAX / sizeof(uint32_t))) return;

    size_t pitch32 = fb_pitch / sizeof(uint32_t);

    for (uint32_t row = 0; row < h; row++) {
        int64_t sy = (int64_t)y + row;

        for (uint32_t col = 0; col < w; col++) {
            int64_t sx = (int64_t)x + col;
            uint32_t pix = 0;

            if (sx >= 0 && sy >= 0 && sx < fb_width && sy < fb_height) {
                pix = back_fb[(size_t)sy * pitch32 + (size_t)sx];
            }

            dst[(size_t)row * w + col] = pix;
        }
    }
}

void gfx_back_put_rect(
    int32_t x,
    int32_t y,
    uint32_t w,
    uint32_t h,
    const uint32_t *src
) {
    if (!back_fb || !src || w == 0 || h == 0) return;

    uint64_t pixels = (uint64_t)w * h;
    if (pixels > (uint64_t)(SIZE_MAX / sizeof(uint32_t))) return;

    size_t pitch32 = fb_pitch / sizeof(uint32_t);

    for (uint32_t row = 0; row < h; row++) {
        int64_t sy = (int64_t)y + row;

        for (uint32_t col = 0; col < w; col++) {
            int64_t sx = (int64_t)x + col;

            if (sx >= 0 && sy >= 0 && sx < fb_width && sy < fb_height) {
                back_fb[(size_t)sy * pitch32 + (size_t)sx] =
                    src[(size_t)row * w + col];
            }
        }
    }
}

void gfx_bsod_seize(void) {
    bsod_seized = 1;
}

void gfx_flip(void) {
    if (bsod_seized || !back_fb || !front_fb) return;

    size_t pitch32 = fb_pitch / sizeof(uint32_t);

    for (uint32_t y = 0; y < fb_height; y++) {
        memcpy(
            front_fb + (size_t)y * pitch32,
            back_fb + (size_t)y * pitch32,
            (size_t)fb_pitch
        );
    }

    if (virtio_gpu_available()) {
        (void)virtio_gpu_present(back_fb, fb_width, fb_height, fb_pitch);
    }
}

void gfx_flip_rect(
    int32_t x,
    int32_t y,
    uint32_t w,
    uint32_t h
) {
    if (bsod_seized || !back_fb || !front_fb || w == 0 || h == 0) return;

    int64_t x0 = x;
    int64_t y0 = y;
    int64_t x1 = x0 + (int64_t)w;
    int64_t y1 = y0 + (int64_t)h;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x0 > fb_width) x0 = fb_width;
    if (y0 > fb_height) y0 = fb_height;
    if (x1 > fb_width) x1 = fb_width;
    if (y1 > fb_height) y1 = fb_height;

    if (x0 >= x1 || y0 >= y1) return;

    uint32_t dx = (uint32_t)x0;
    uint32_t dy = (uint32_t)y0;
    uint32_t row_pixels = (uint32_t)(x1 - x0);
    uint32_t rows = (uint32_t)(y1 - y0);
    size_t pitch32 = fb_pitch / sizeof(uint32_t);

    for (uint32_t row = 0; row < rows; row++) {
        size_t offset = (size_t)(dy + row) * pitch32 + dx;
        memcpy(
            front_fb + offset,
            back_fb + offset,
            (size_t)row_pixels * sizeof(uint32_t)
        );
    }

    if (virtio_gpu_available()) {
        (void)virtio_gpu_present_rect(
            back_fb,
            fb_width,
            fb_height,
            fb_pitch,
            dx,
            dy,
            row_pixels,
            rows
        );
    }
}

void gfx_clear(color_t color) {
    gfx_fill_rect(0, 0, fb_width, fb_height, color);
}

uint32_t gfx_get_width(void) {
    return fb_width;
}

uint32_t gfx_get_height(void) {
    return fb_height;
}

uint32_t *gfx_get_back_buffer(void) {
    return back_fb;
}

uint32_t gfx_get_pitch_bytes(void) {
    return fb_pitch;
}
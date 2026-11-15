
/* graphics.c — 2D graphics library with double-buffering.
 *
 * Renders to an off-screen back buffer allocated via kmalloc, then copies
 * (flips) to the visible Limine framebuffer. This avoids tearing/flicker
 * during multi-element redraws. Pixel plotting, filled rectangles, lines
 * (Bresenham), and bitmap-font text are provided.
 */

#include <stdint.h>
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

#if defined(__x86_64__)
#include <emmintrin.h>
#include <smmintrin.h>
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
    int32_t sw = (int32_t)fb_width;
    int32_t sh = (int32_t)fb_height;

    if (x < 0) {
        w = (w > (uint32_t)-x) ? w + (uint32_t)x : 0;
        x = 0;
    }

    if (y < 0) {
        h = (h > (uint32_t)-y) ? h + (uint32_t)y : 0;
        y = 0;
    }

    clip_x0 = x;
    clip_y0 = y;
    clip_x1 = (x + (int32_t)w > sw) ? sw : x + (int32_t)w;
    clip_y1 = (y + (int32_t)h > sh) ? sh : y + (int32_t)h;
}

void gfx_clip_clear(void) {
    clip_x0 = 0;
    clip_y0 = 0;
    clip_x1 = (int32_t)fb_width;
    clip_y1 = (int32_t)fb_height;
}

uint64_t gfx_take_px_counter(void) {
    uint64_t v = px_written;
    px_written = 0;
    return v;
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
    boot_fb_t *fb = boot_get_framebuffer();

    if (fb == NULL || fb->bpp != 32) {
        return;
    }

#if defined(__i386__)
    front_fb = (uint32_t *)fb32_map_fb(
        (uint32_t)fb->phys_base,
        (uint32_t)(fb->pitch * fb->height)
    );

    if (!front_fb) {
        return;
    }
#else
    front_fb = (uint32_t *)(uintptr_t)(
        boot_get_hhdm_offset() + fb->phys_base
    );
#endif

    fb_width = (uint32_t)fb->width;
    fb_height = (uint32_t)fb->height;
    fb_pitch = (uint32_t)fb->pitch;

    r_shift = fb->red_shift;
    g_shift = fb->green_shift;
    b_shift = fb->blue_shift;

    gfx_clip_clear();

    uint64_t buf_bytes = (uint64_t)fb_pitch * fb_height;

    back_fb = kmalloc(buf_bytes);

    if (back_fb) {
        memset(back_fb, 0, (size_t)buf_bytes);
    }
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
    uint32_t pitch32 = fb_pitch / 4;

    back_fb[y * pitch32 + x] = packed;
    px_written++;
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

    for (; i + 4 <= n; i += 4) {
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
void gfx_fill_rect(uint32_t x, uint32_t y,
                   uint32_t w, uint32_t h,
                   color_t color) {
    if (!back_fb) {
        return;
    }

    int32_t x0 = (int32_t)x;
    int32_t y0 = (int32_t)y;
    int32_t x1 = x0 + (int32_t)w;
    int32_t y1 = y0 + (int32_t)h;

    if (x0 < clip_x0) x0 = clip_x0;
    if (y0 < clip_y0) y0 = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;

    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    color_t packed = make_color(color);
    uint32_t pitch32 = fb_pitch / 4;
    uint32_t rw = (uint32_t)(x1 - x0);

    for (int32_t py = y0; py < y1; py++) {
        gfx_memset32(
            back_fb + (uint32_t)py * pitch32 + (uint32_t)x0,
            packed,
            rw
        );
    }

    uint64_t rh = (uint32_t)(y1 - y0);
    px_written += (uint64_t)rw * rh;
}

/*
 * SSE4.1 implementation of the RGB888 blit operation.
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
static uint64_t gfx_blit_rgb888_sse41(
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
            src + (size_t)(sy_off + row) * src_stride + sx_off;

        uint32_t *d =
            back_fb + (size_t)(dy0 + (int32_t)row) * pitch32 +
            (uint32_t)dx0;

        const uint8_t *m = skip_mask
            ? skip_mask + (size_t)(sy_off + row) * mask_stride
            : NULL;

        uint32_t col = 0;

        if (m) {
            /*
             * Process pixels until the source mask position reaches
             * a byte boundary.
             *
             * The absolute source X coordinate is important here:
             * sx_off may be non-zero after clipping.
             */
            while (col < vis_w && ((sx_off + col) & 7u) != 0u) {
                uint32_t mask_bit = sx_off + col;
                uint32_t b = m[mask_bit >> 3];

                if (!((b >> (7u - (mask_bit & 7u))) & 1u)) {
                    d[col] = s[col];
                    stored++;
                }

                col++;
            }

            /*
             * The mask position is byte-aligned here.
             * Process eight pixels using two four-pixel SIMD operations.
             */
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

                __m128i all_bits = _mm_set1_epi32(-1);

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

            /*
             * Process remaining pixels.
             */
            for (; col < vis_w; col++) {
                uint32_t mask_bit = sx_off + col;
                uint32_t b = m[mask_bit >> 3];

                if (!((b >> (7u - (mask_bit & 7u))) & 1u)) {
                    d[col] = s[col];
                    stored++;
                }
            }
        } else {
            /*
             * No transparency mask: copy the row using SSE2 loads/stores.
             */
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
    if (!back_fb || !src || w == 0 || h == 0) {
        return 0;
    }

    uint32_t pitch32 = fb_pitch / 4;

    /*
     * Intersect with the clip rectangle.
     * Calculate source offsets once, then process only visible pixels.
     */
    int32_t dx0 = x;
    int32_t dy0 = y;
    int32_t dx1 = x + (int32_t)w;
    int32_t dy1 = y + (int32_t)h;

    int32_t sx_off = 0;
    int32_t sy_off = 0;

    if (dx0 < clip_x0) {
        sx_off = clip_x0 - dx0;
        dx0 = clip_x0;
    }

    if (dy0 < clip_y0) {
        sy_off = clip_y0 - dy0;
        dy0 = clip_y0;
    }

    if (dx1 > clip_x1) {
        dx1 = clip_x1;
    }

    if (dy1 > clip_y1) {
        dy1 = clip_y1;
    }

    if (dx0 >= dx1 || dy0 >= dy1) {
        return 0;
    }

    uint32_t vis_w = (uint32_t)(dx1 - dx0);
    uint32_t vis_h = (uint32_t)(dy1 - dy0);

    uint64_t stored = 0;

#if GFX_HAVE_SIMD
    /*
     * SIMD code is called only when the processor reports SSE4.1 support.
     * The caller itself remains compiled without SSE/SSE2.
     */
    {
        stored = gfx_blit_rgb888_sse41(
            dx0,
            dy0,
            vis_w,
            vis_h,
            (uint32_t)sx_off,
            (uint32_t)sy_off,
            src,
            src_stride,
            skip_mask,
            mask_stride,
            pitch32
        );

        px_written += stored;
        return stored;
    }
#endif

    /*
     * Scalar fallback.
     */
    for (uint32_t row = 0; row < vis_h; row++) {
        const uint32_t *s =
            src + (size_t)(sy_off + (int32_t)row) * src_stride +
            (uint32_t)sx_off;

        uint32_t *d =
            back_fb + (size_t)(dy0 + (int32_t)row) * pitch32 +
            (uint32_t)dx0;

        const uint8_t *m = skip_mask
            ? skip_mask + (size_t)(sy_off + (int32_t)row) * mask_stride
            : NULL;

        for (uint32_t col = 0; col < vis_w; col++) {
            if (m) {
                uint32_t mask_bit = (uint32_t)sx_off + col;
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
}

void gfx_draw_rect(uint32_t x, uint32_t y,
                   uint32_t w, uint32_t h,
                   color_t color) {
    if (w == 0 || h == 0) {
        return;
    }

    for (uint32_t i = 0; i < w; i++) {
        gfx_putpixel(x + i, y, color);
        gfx_putpixel(x + i, y + h - 1, color);
    }

    for (uint32_t i = 0; i < h; i++) {
        gfx_putpixel(x, y + i, color);
        gfx_putpixel(x + w - 1, y + i, color);
    }
}

/* Cohen-Sutherland outcodes. */
#define GFX_OC_LEFT   1
#define GFX_OC_RIGHT  2
#define GFX_OC_BOTTOM 4
#define GFX_OC_TOP    8

static int gfx_line_outcode(
    int32_t x,
    int32_t y,
    int32_t xmax,
    int32_t ymax
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

void gfx_draw_line(
    uint32_t x0u,
    uint32_t y0u,
    uint32_t x1u,
    uint32_t y1u,
    color_t color
) {
    /*
     * Recover signed coordinates before clipping.
     * Callers can pass negative coordinates encoded in uint32_t.
     */
    int32_t x0 = (int32_t)x0u;
    int32_t y0 = (int32_t)y0u;
    int32_t x1 = (int32_t)x1u;
    int32_t y1 = (int32_t)y1u;

    const int32_t xmax = (int32_t)fb_width - 1;
    const int32_t ymax = (int32_t)fb_height - 1;

    if (xmax < 0 || ymax < 0) {
        return;
    }

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

        int32_t nx;
        int32_t ny;

        if (c & GFX_OC_LEFT) {
            nx = 0;
            ny = (int32_t)(
                y0 + (int64_t)(y1 - y0) * (0 - x0) / (x1 - x0)
            );
        } else if (c & GFX_OC_RIGHT) {
            nx = xmax;
            ny = (int32_t)(
                y0 + (int64_t)(y1 - y0) * (xmax - x0) / (x1 - x0)
            );
        } else if (c & GFX_OC_BOTTOM) {
            ny = 0;
            nx = (int32_t)(
                x0 + (int64_t)(x1 - x0) * (0 - y0) / (y1 - y0)
            );
        } else {
            ny = ymax;
            nx = (int32_t)(
                x0 + (int64_t)(x1 - x0) * (ymax - y0) / (y1 - y0)
            );
        }

        if (c == c0) {
            x0 = nx;
            y0 = ny;
        } else {
            x1 = nx;
            y1 = ny;
        }
    }

    int32_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int32_t dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);

    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t sy = (y0 < y1) ? 1 : -1;

    int32_t err = dx - dy;

    for (;;) {
        gfx_putpixel((uint32_t)x0, (uint32_t)y0, color);

        if (x0 == x1 && y0 == y1) {
            break;
        }

        int32_t e2 = 2 * err;

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
    const struct psf_font *f = psf_get_font();

    if (!f) {
        return;
    }

    uint32_t cx = x;

    for (; *s; s++) {
        if (*s == '\n') {
            cx = x;
            y += f->height;
            continue;
        }

        uint32_t idx = (unsigned char)*s;

        if (idx >= f->num_glyphs) {
            idx = 0;
        }

        const uint8_t *glyph =
            f->data + (uint64_t)idx * f->bytes_per_glyph;

        for (uint32_t gy = 0; gy < f->height; gy++) {
            const uint8_t *row_bytes =
                glyph + gy * f->bytes_per_row;

            for (uint32_t gx = 0; gx < f->width; gx++) {
                uint8_t byte = row_bytes[gx / 8];
                uint8_t bit_pos = 7 - (gx % 8);

                if ((byte >> bit_pos) & 1) {
                    gfx_putpixel(cx + gx, y + gy, color);
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
    uint32_t w = 0;

    while (s && *s) {
        if (*s == '\n') {
            break;
        }

        w += glyph_w;
        s++;
    }

    return w;
}

void gfx_draw_text_centered(
    uint32_t y,
    const char *s,
    color_t color
) {
    uint32_t tw = gfx_text_width(s);

    uint32_t x = (tw < fb_width)
        ? ((fb_width - tw) / 2)
        : 0;

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

void gfx_gradient_v(
    uint32_t x,
    uint32_t y,
    uint32_t w,
    uint32_t h,
    color_t top,
    color_t bottom
) {
    if (!back_fb || h == 0) {
        return;
    }

    for (uint32_t row = 0; row < h; row++) {
        uint8_t t = (h > 1)
            ? (uint8_t)((row * 255u) / (h - 1u))
            : 0;

        color_t c = gfx_blend(top, bottom, t);

        gfx_fill_rect(x, y + row, w, 1, c);
    }
}

void gfx_draw_circle(
    uint32_t cx,
    uint32_t cy,
    uint32_t r,
    color_t color
) {
    int32_t x = (int32_t)r;
    int32_t y = 0;
    int32_t err = 0;

    int32_t icx = (int32_t)cx;
    int32_t icy = (int32_t)cy;

    while (x >= y) {
        gfx_putpixel((uint32_t)(icx + x), (uint32_t)(icy + y), color);
        gfx_putpixel((uint32_t)(icx + y), (uint32_t)(icy + x), color);
        gfx_putpixel((uint32_t)(icx - y), (uint32_t)(icy + x), color);
        gfx_putpixel((uint32_t)(icx - x), (uint32_t)(icy + y), color);
        gfx_putpixel((uint32_t)(icx - x), (uint32_t)(icy - y), color);
        gfx_putpixel((uint32_t)(icx - y), (uint32_t)(icy - x), color);
        gfx_putpixel((uint32_t)(icx + y), (uint32_t)(icy - x), color);
        gfx_putpixel((uint32_t)(icx + x), (uint32_t)(icy - y), color);

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
    int32_t rr = (int32_t)r;
    int32_t icx = (int32_t)cx;
    int32_t icy = (int32_t)cy;

    for (int32_t dy = -rr; dy <= rr; dy++) {
        for (int32_t dx = -rr; dx <= rr; dx++) {
            if (dx * dx + dy * dy <= rr * rr) {
                gfx_putpixel(
                    (uint32_t)(icx + dx),
                    (uint32_t)(icy + dy),
                    color
                );
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
    if (!back_fb || !dst || w == 0 || h == 0) {
        return;
    }

    uint32_t pitch32 = fb_pitch / 4;

    for (uint32_t row = 0; row < h; row++) {
        int32_t sy = y + (int32_t)row;

        for (uint32_t col = 0; col < w; col++) {
            int32_t sx = x + (int32_t)col;
            uint32_t pix = 0;

            if (sx >= 0 && sy >= 0 &&
                (uint32_t)sx < fb_width &&
                (uint32_t)sy < fb_height) {
                pix = back_fb[
                    (uint32_t)sy * pitch32 + (uint32_t)sx
                ];
            }

            dst[row * w + col] = pix;
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
    if (!back_fb || !src || w == 0 || h == 0) {
        return;
    }

    uint32_t pitch32 = fb_pitch / 4;

    for (uint32_t row = 0; row < h; row++) {
        int32_t sy = y + (int32_t)row;

        for (uint32_t col = 0; col < w; col++) {
            int32_t sx = x + (int32_t)col;

            if (sx >= 0 && sy >= 0 &&
                (uint32_t)sx < fb_width &&
                (uint32_t)sy < fb_height) {
                back_fb[
                    (uint32_t)sy * pitch32 + (uint32_t)sx
                ] = src[row * w + col];
            }
        }
    }
}

void gfx_bsod_seize(void) {
    bsod_seized = 1;
}

void gfx_flip(void) {
    if (bsod_seized) {
        return;
    }

    if (!back_fb || !front_fb) {
        return;
    }

    uint32_t pitch32 = fb_pitch / 4;

    for (uint32_t y = 0; y < fb_height; y++) {
        memcpy(
            front_fb + y * pitch32,
            back_fb + y * pitch32,
            (size_t)fb_pitch
        );
    }

    /*
     * Mirror the completed frame to the VirtIO-GPU scanout if available.
     * The Limine framebuffer copy is retained for compatibility.
     */
    if (virtio_gpu_available()) {
        (void)virtio_gpu_present(
            back_fb,
            fb_width,
            fb_height,
            fb_pitch
        );
    }
}

void gfx_flip_rect(
    int32_t x,
    int32_t y,
    uint32_t w,
    uint32_t h
) {
    if (bsod_seized) {
        return;
    }

    if (!back_fb || !front_fb) {
        return;
    }

    uint32_t pitch32 = fb_pitch / 4;

    int32_t x1 = x + (int32_t)w;
    int32_t y1 = y + (int32_t)h;

    if (x < 0) x = 0;
    if (y < 0) y = 0;

    if (x1 > (int32_t)fb_width) {
        x1 = (int32_t)fb_width;
    }

    if (y1 > (int32_t)fb_height) {
        y1 = (int32_t)fb_height;
    }

    if (x >= x1 || y >= y1) {
        return;
    }

    uint32_t row_pixels = (uint32_t)(x1 - x);

    for (int32_t row = y; row < y1; row++) {
        memcpy(
            front_fb + (uint32_t)row * pitch32 + (uint32_t)x,
            back_fb + (uint32_t)row * pitch32 + (uint32_t)x,
            row_pixels * sizeof(uint32_t)
        );
    }

    if (virtio_gpu_available()) {
        (void)virtio_gpu_present_rect(
            back_fb,
            fb_width,
            fb_height,
            fb_pitch,
            x,
            y,
            row_pixels,
            (uint32_t)(y1 - y)
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

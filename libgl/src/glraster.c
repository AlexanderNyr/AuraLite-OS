/* libgl/src/glraster.c — rasterisation of points, lines and triangles.
 *
 * Phase G2 provides points and lines (Bresenham) and draws triangles as
 * wireframe outlines.  Phase G3 replaces gl_raster_triangle() with a filled,
 * depth-tested edge-function rasterizer; nothing else in the pipeline has to
 * change when that happens.
 *
 * Everything here works in WINDOW coordinates with GL's bottom-left origin.
 * The flip to the framebuffer's top-left origin happens in gl_fb_row(), so
 * this file never deals with it directly.
 */

#include "GL/gl.h"
#include "glcontext.h"
#include "glvertex.h"

/* Write one pixel, clipping to the framebuffer and honouring the scissor box.
 *
 * Depth testing is deliberately NOT applied to points and lines in G2: there
 * is no depth buffer content to test against until the triangle rasterizer
 * exists, and applying it here would make wireframe output disappear.  G3
 * routes all three primitive types through a shared depth test.
 */
static void put_pixel(struct aglx_context *ctx, int x, int y, gl_pixel_t c) {
    if (x < 0 || y < 0 || x >= ctx->width || y >= ctx->height) return;

    if (ctx->scissor_test) {
        if (x < ctx->scissor_x || y < ctx->scissor_y ||
            x >= ctx->scissor_x + ctx->scissor_w ||
            y >= ctx->scissor_y + ctx->scissor_h) {
            return;
        }
    }
    gl_fb_row(ctx, y)[x] = c;
}

/* Window coordinate -> pixel index.
 *
 * GL pixel (i,j) covers the half-open square [i,i+1) x [j,j+1), so its centre
 * sits at (i+0.5, j+0.5) and the owning pixel of a window coordinate is
 * floor(v) — NOT round(v).  Rounding would shift everything half a pixel and
 * put a vertex specified at a pixel centre (the usual convention, and what
 * glOrtho over the pixel grid produces) into the neighbouring pixel.
 *
 * C truncates toward zero, which differs from floor for negatives, and window
 * coordinates do go negative when geometry falls outside the viewport — hence
 * the explicit adjustment rather than a bare cast.
 */
static int win_to_pixel(GLfloat v) {
    int i = (int)v;
    if (v < 0.0f && (GLfloat)i != v) i--;   /* emulate floor() */
    return i;
}

void gl_raster_point(struct aglx_context *ctx, const gl_vertex_t *v) {
    if (!v->valid) return;
    put_pixel(ctx, win_to_pixel(v->win.x), win_to_pixel(v->win.y),
              gl_pack_color(v->color));
}

/* ---- Cohen-Sutherland region codes, used to clip a segment to the
 * framebuffer BEFORE rasterising it.
 *
 * This is not just an optimisation.  Geometry close to the eye plane projects
 * to enormous window coordinates, and Bresenham walks one pixel per step: a
 * segment running from x = -3,000,000 to x = +3,000,000 costs six million
 * iterations to draw the handful of pixels that are actually on screen, which
 * looks exactly like a hang.  Clipping first bounds the work by the framebuffer
 * size no matter what the projection produces.
 *
 * Phase G4 adds proper frustum clipping in 3D, which handles the same problem
 * at its source; this 2D clip stays as the rasterizer's own safety net.
 */
#define CS_INSIDE 0
#define CS_LEFT   1
#define CS_RIGHT  2
#define CS_BOTTOM 4
#define CS_TOP    8

static int cs_code(const struct aglx_context *ctx, GLfloat x, GLfloat y) {
    int code = CS_INSIDE;
    if (x < 0.0f)                        code |= CS_LEFT;
    else if (x > (GLfloat)(ctx->width))  code |= CS_RIGHT;
    if (y < 0.0f)                        code |= CS_BOTTOM;
    else if (y > (GLfloat)(ctx->height)) code |= CS_TOP;
    return code;
}

/* Clip the segment to the framebuffer.  On success returns 1 and writes the
 * clipped endpoints plus their parametric positions (t0, t1) along the
 * ORIGINAL segment, so colour interpolation stays correct.  Returns 0 when the
 * segment is entirely outside. */
static int clip_line(const struct aglx_context *ctx,
                     GLfloat *x0, GLfloat *y0, GLfloat *x1, GLfloat *y1,
                     GLfloat *t0_out, GLfloat *t1_out) {
    GLfloat ox0 = *x0, oy0 = *y0, ox1 = *x1, oy1 = *y1;
    GLfloat dx = ox1 - ox0, dy = oy1 - oy0;

    int code0 = cs_code(ctx, *x0, *y0);
    int code1 = cs_code(ctx, *x1, *y1);

    /* Bounded iteration count: each pass removes at least one region bit. */
    for (int guard = 0; guard < 8; guard++) {
        if ((code0 | code1) == 0) break;          /* trivially inside  */
        if ((code0 & code1) != 0) return 0;       /* trivially outside */

        int outcode = code0 ? code0 : code1;
        GLfloat x = 0.0f, y = 0.0f;

        if (outcode & CS_TOP) {
            y = (GLfloat)ctx->height;
            x = (dy != 0.0f) ? ox0 + dx * (y - oy0) / dy : ox0;
        } else if (outcode & CS_BOTTOM) {
            y = 0.0f;
            x = (dy != 0.0f) ? ox0 + dx * (y - oy0) / dy : ox0;
        } else if (outcode & CS_RIGHT) {
            x = (GLfloat)ctx->width;
            y = (dx != 0.0f) ? oy0 + dy * (x - ox0) / dx : oy0;
        } else { /* CS_LEFT */
            x = 0.0f;
            y = (dx != 0.0f) ? oy0 + dy * (x - ox0) / dx : oy0;
        }

        if (outcode == code0) {
            *x0 = x; *y0 = y; code0 = cs_code(ctx, x, y);
        } else {
            *x1 = x; *y1 = y; code1 = cs_code(ctx, x, y);
        }
    }

    /* Where the clipped endpoints sit along the original segment. */
    GLfloat len2 = dx * dx + dy * dy;
    if (len2 > 1e-12f) {
        *t0_out = ((*x0 - ox0) * dx + (*y0 - oy0) * dy) / len2;
        *t1_out = ((*x1 - ox0) * dx + (*y1 - oy0) * dy) / len2;
    } else {
        *t0_out = 0.0f;
        *t1_out = 1.0f;
    }
    return 1;
}

/* Integer Bresenham with colour interpolation along the line. */
/* Integer Bresenham with colour interpolation along the line.
 *
 * Colour is interpolated by step count rather than by distance: the two are
 * equivalent for a straight line and the step counter is already there. */
void gl_raster_line(struct aglx_context *ctx,
                    const gl_vertex_t *a, const gl_vertex_t *b) {
    if (!a->valid || !b->valid) return;

    /* Clip to the framebuffer first: see the comment on clip_line().  Without
     * this, geometry near the eye plane produces window coordinates in the
     * millions and Bresenham would walk every one of those pixels. */
    GLfloat fx0 = a->win.x, fy0 = a->win.y;
    GLfloat fx1 = b->win.x, fy1 = b->win.y;
    GLfloat t0 = 0.0f, t1 = 1.0f;
    if (!clip_line(ctx, &fx0, &fy0, &fx1, &fy1, &t0, &t1)) return;

    /* Colours at the clipped endpoints, so interpolation matches what the
     * unclipped segment would have produced. */
    gl_color_t ca, cb2;
    ca.r  = a->color.r + (b->color.r - a->color.r) * t0;
    ca.g  = a->color.g + (b->color.g - a->color.g) * t0;
    ca.b  = a->color.b + (b->color.b - a->color.b) * t0;
    ca.a  = a->color.a + (b->color.a - a->color.a) * t0;
    cb2.r = a->color.r + (b->color.r - a->color.r) * t1;
    cb2.g = a->color.g + (b->color.g - a->color.g) * t1;
    cb2.b = a->color.b + (b->color.b - a->color.b) * t1;
    cb2.a = a->color.a + (b->color.a - a->color.a) * t1;

    int x0 = win_to_pixel(fx0), y0 = win_to_pixel(fy0);
    int x1 = win_to_pixel(fx1), y1 = win_to_pixel(fy1);

    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    /* Total steps, used to parameterise the colour interpolation. */
    int steps = (dx > dy) ? dx : dy;
    if (steps == 0) {
        /* Degenerate line: both endpoints land on the same pixel. */
        put_pixel(ctx, x0, y0, gl_pack_color(ca));
        return;
    }

    int flat = (ctx->shade_model == GL_FLAT);
    gl_pixel_t flat_color = gl_pack_color(cb2);  /* flat uses the last vertex */

    int step = 0;
    for (;;) {
        gl_pixel_t c;
        if (flat) {
            c = flat_color;
        } else {
            GLfloat t = (GLfloat)step / (GLfloat)steps;
            gl_color_t col;
            col.r = ca.r + (cb2.r - ca.r) * t;
            col.g = ca.g + (cb2.g - ca.g) * t;
            col.b = ca.b + (cb2.b - ca.b) * t;
            col.a = ca.a + (cb2.a - ca.a) * t;
            c = gl_pack_color(col);
        }
        put_pixel(ctx, x0, y0, c);

        if (x0 == x1 && y0 == y1) break;

        /* Guard against a runaway loop if the endpoints are absurd. */
        if (step > steps + 2) break;

        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
        step++;
    }
}

/* Phase G2: triangles are drawn as their three edges.
 *
 * This is intentionally a placeholder that produces correct, visible geometry
 * so the transform pipeline can be validated before the filled rasterizer
 * lands in G3.  glPolygonMode(GL_LINE) will keep this path afterwards. */
void gl_raster_triangle(struct aglx_context *ctx,
                        const gl_vertex_t *a, const gl_vertex_t *b,
                        const gl_vertex_t *c) {
    gl_raster_line(ctx, a, b);
    gl_raster_line(ctx, b, c);
    gl_raster_line(ctx, c, a);
}

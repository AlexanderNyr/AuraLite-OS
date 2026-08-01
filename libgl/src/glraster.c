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

#include <math.h>

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

/* Run the fragment shader for one point or line pixel.
 *
 * Points and lines went through this path unshaded until G11d: they wrote
 * v->color, which the shader vertex stage leaves at white, so a shaded
 * GL_LINE_LOOP or GL_POINTS came out WHITE instead of running the fragment
 * shader at all.  Nothing caught it because every shader test drew triangles.
 *
 * Returns 0 when the fragment was discarded.
 */
static int shade_pixel(struct aglx_context *ctx, const gl_vertex_t *v,
                       int px_x, int px_y, gl_pixel_t *out) {
    gl_color_t sc;
    sc.r = sc.g = sc.b = 0.0f;
    sc.a = 1.0f;

    /* A point or a line has no barycentric interpolation to do -- the
     * varyings are the vertex's own, or the endpoint-interpolated ones the
     * caller already computed. */
    if (!gl_shader_run_fragment(ctx, v->varying,
                                (GLfloat)px_x + 0.5f, (GLfloat)px_y + 0.5f,
                                v->win.z, 1, &sc)) {
        return 0;
    }
    *out = gl_pack_color(sc);
    return 1;
}

void gl_raster_point(struct aglx_context *ctx, const gl_vertex_t *v) {
    if (!v->valid) return;

    int px_x = win_to_pixel(v->win.x), px_y = win_to_pixel(v->win.y);

    if (gl_shader_active(ctx)) {
        gl_pixel_t c;
        if (shade_pixel(ctx, v, px_x, px_y, &c)) put_pixel(ctx, px_x, px_y, c);
        return;
    }

    put_pixel(ctx, px_x, px_y, gl_pack_color(v->color));
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
        if (gl_shader_active(ctx)) {
            gl_pixel_t sc;
            if (shade_pixel(ctx, a, x0, y0, &sc)) put_pixel(ctx, x0, y0, sc);
            return;
        }
        put_pixel(ctx, x0, y0, gl_pack_color(ca));
        return;
    }

    int flat = (ctx->shade_model == GL_FLAT);
    gl_pixel_t flat_color = gl_pack_color(cb2);  /* flat uses the last vertex */

    /* Shaded lines interpolate their varyings along the segment, using the
     * same clipped parameters t0/t1 the colours above use. */
    int use_shader = gl_shader_active(ctx);
    int nvary = use_shader ? a->varying_count : 0;
    if (nvary > GL_MAX_VARYING_FLOATS) nvary = GL_MAX_VARYING_FLOATS;

    gl_vertex_t sv;
    if (use_shader) {
        sv = *a;
        sv.varying_count = nvary;
    }

    int step = 0;
    for (;;) {
        if (use_shader) {
            GLfloat t = (GLfloat)step / (GLfloat)steps;
            GLfloat tt = t0 + (t1 - t0) * t;
            for (int k = 0; k < nvary; k++) {
                sv.varying[k] = a->varying[k]
                              + (b->varying[k] - a->varying[k]) * tt;
            }
            sv.win.z = a->win.z + (b->win.z - a->win.z) * tt;

            gl_pixel_t sc;
            if (shade_pixel(ctx, &sv, x0, y0, &sc)) put_pixel(ctx, x0, y0, sc);

            if (x0 == x1 && y0 == y1) break;
            if (step > steps + 2) break;
            int e2s = err * 2;
            if (e2s > -dy) { err -= dy; x0 += sx; }
            if (e2s <  dx) { err += dx; y0 += sy; }
            step++;
            continue;
        }

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

/* ============================================================================
 * Triangle rasterisation (phase G3)
 *
 * Edge-function rasterizer, not a scanline or painter's-algorithm one.
 *
 * For each edge the function
 *
 *     E(x,y) = (x - x0)*(y1 - y0) - (y - y0)*(x1 - x0)
 *
 * is the cross product of the edge vector with the vector to the sample point.
 * Its sign says which side of the edge the point lies on, and its magnitude is
 * twice the area of the sub-triangle, which is exactly the unnormalised
 * barycentric weight of the opposite vertex.  So one evaluation per edge gives
 * both the inside test and the interpolation weights.
 *
 * Why not painter's algorithm (what drivers/framebuffer/render3d.c uses):
 * sorting whole triangles by depth cannot resolve intersecting or cyclically
 * overlapping geometry.  A per-pixel depth buffer resolves both correctly and
 * is what GL specifies.
 * ==========================================================================*/

/* Twice the signed area of the triangle in screen space.  Positive means
 * counter-clockwise with GL's bottom-left origin and y growing up. */
static GLfloat signed_area2(const gl_vertex_t *a, const gl_vertex_t *b,
                            const gl_vertex_t *c) {
    return (b->win.x - a->win.x) * (c->win.y - a->win.y)
         - (b->win.y - a->win.y) * (c->win.x - a->win.x);
}

/* Depth comparison (§4.1.5).  Returns non-zero when the fragment passes. */
static int depth_passes(GLenum func, float src, float dst) {
    switch (func) {
    case GL_NEVER:    return 0;
    case GL_LESS:     return src <  dst;
    case GL_EQUAL:    return src == dst;
    case GL_LEQUAL:   return src <= dst;
    case GL_GREATER:  return src >  dst;
    case GL_NOTEQUAL: return src != dst;
    case GL_GEQUAL:   return src >= dst;
    case GL_ALWAYS:   return 1;
    default:          return 0;
    }
}

/* Top-left fill rule (§3.5.1).
 *
 * A pixel exactly on a shared edge must belong to exactly ONE of the two
 * triangles that share it: counting it twice double-shades the seam (visible
 * once blending arrives in G6), counting it zero times leaves a one-pixel gap.
 * GL resolves this by including edges that are "top" or "left" and excluding
 * the others.
 *
 * With a counter-clockwise winding and y growing upwards:
 *   - a top edge is horizontal and goes right-to-left (dy == 0 && dx < 0)
 *   - a left edge goes downwards (dy < 0)
 */
static int is_top_left(GLfloat dx, GLfloat dy) {
    return (dy == 0.0f && dx < 0.0f) || (dy < 0.0f);
}

/* ---- Per-triangle mipmap level of detail (phase G10) ----
 *
 * Hardware evaluates the LOD per FRAGMENT from dFdx/dFdy of the texture
 * coordinates.  A scanline rasterizer has no derivatives available, so this
 * computes ONE level for the whole primitive:
 *
 *     lod = log2( sqrt( texture-space area / screen-space area ) )
 *
 * The ratio of areas is the square of the average scale factor between the
 * two spaces, so its square root is "texels per pixel" and its base-2
 * logarithm is exactly the mipmap level at which one texel covers one pixel.
 *
 * WHEN THIS IS WRONG, AND WHY IT IS STILL THE RIGHT CHOICE HERE
 *
 * It is exact for a triangle at constant depth, and progressively wrong for a
 * foreshortened one: a ground plane receding to the horizon has a texel/pixel
 * ratio that varies by orders of magnitude across the primitive, and gets a
 * single averaged level where hardware would blend several.  The cure is to
 * tessellate such surfaces -- which is cheap -- rather than to pay a
 * derivative computation on every one of the millions of fragments a software
 * rasterizer already struggles with.  Per-fragment LOD is a possible follow-up
 * (carry du/dx through the edge functions); it is not free.
 *
 * The texture-space area uses the PROJECTED coordinates s/w, t/w rather than
 * the raw ones, so a perspective primitive is measured where it is actually
 * drawn.
 */
static GLfloat triangle_lod(const gl_texture_t *tex,
                            const gl_vertex_t *v0, const gl_vertex_t *v1,
                            const gl_vertex_t *v2,
                            GLfloat screen_area2, int unit) {
    if (!gl_texture_uses_mipmaps(tex)) return 0.0f;

    GLfloat tw = 0.0f, th = 0.0f;
    if (!gl_texture_base_size(tex, &tw, &th)) return 0.0f;
    if (tw <= 0.0f || th <= 0.0f) return 0.0f;

    /* Texture-space positions in TEXELS. */
    GLfloat ax = v0->s[unit] * tw, ay = v0->t[unit] * th;
    GLfloat bx = v1->s[unit] * tw, by = v1->t[unit] * th;
    GLfloat cx = v2->s[unit] * tw, cy = v2->t[unit] * th;

    GLfloat tex_area2 = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    if (tex_area2 < 0.0f) tex_area2 = -tex_area2;

    GLfloat scr_area2 = screen_area2 < 0.0f ? -screen_area2 : screen_area2;
    /* A degenerate screen triangle covers no pixels, so the level it would
     * pick never matters; returning 0 avoids a division by zero. */
    if (scr_area2 < 1e-12f || tex_area2 <= 0.0f) return 0.0f;

    GLfloat ratio = tex_area2 / scr_area2;
    if (ratio <= 1.0f) return 0.0f;            /* magnifying: level 0 */

    /* log2(sqrt(ratio)) == 0.5 * log2(ratio). */
    return 0.5f * log2f(ratio);
}

void gl_raster_triangle(struct aglx_context *ctx,
                        const gl_vertex_t *a, const gl_vertex_t *b,
                        const gl_vertex_t *c) {
    /* GL_LINE polygon mode keeps the phase-G2 wireframe behaviour. */
    if (ctx->polygon_mode == GL_LINE) {
        gl_raster_line(ctx, a, b);
        gl_raster_line(ctx, b, c);
        gl_raster_line(ctx, c, a);
        return;
    }
    if (ctx->polygon_mode == GL_POINT) {
        gl_raster_point(ctx, a);
        gl_raster_point(ctx, b);
        gl_raster_point(ctx, c);
        return;
    }

    GLfloat area = signed_area2(a, b, c);

    /* Degenerate: zero area covers no pixels.  Also rejects the NaN/inf that
     * a malformed projection can produce, since every comparison with NaN is
     * false and the != test below catches it. */
    if (!(area != 0.0f)) return;

    /* ---- Face culling (§3.5.1) ----
     * The winding as seen on screen determines the facing.  glFrontFace tells
     * us which screen winding counts as front. */
    int ccw = (area > 0.0f);
    int is_front = (ctx->front_face == GL_CCW) ? ccw : !ccw;

    if (ctx->cull_face) {
        if (ctx->cull_mode == GL_FRONT_AND_BACK) return;
        if (ctx->cull_mode == GL_BACK  && !is_front) return;
        if (ctx->cull_mode == GL_FRONT &&  is_front) return;
    }

    /* Work with a consistently counter-clockwise triangle from here on, so the
     * edge functions are positive inside and the fill rule has a single
     * orientation to reason about.  Swapping two vertices flips the winding. */
    const gl_vertex_t *v0 = a, *v1 = b, *v2 = c;
    if (!ccw) { const gl_vertex_t *t = v1; v1 = v2; v2 = t; area = -area; }

    /* ---- Bounding box, clipped to the framebuffer and the scissor box ----
     * This is what keeps the cost proportional to the triangle's on-screen
     * size rather than to the buffer, and what makes huge off-screen
     * coordinates harmless (the same class of bug fixed for lines in G2). */
    GLfloat fminx = v0->win.x, fmaxx = v0->win.x;
    GLfloat fminy = v0->win.y, fmaxy = v0->win.y;
    if (v1->win.x < fminx) fminx = v1->win.x;
    if (v1->win.x > fmaxx) fmaxx = v1->win.x;
    if (v2->win.x < fminx) fminx = v2->win.x;
    if (v2->win.x > fmaxx) fmaxx = v2->win.x;
    if (v1->win.y < fminy) fminy = v1->win.y;
    if (v1->win.y > fmaxy) fmaxy = v1->win.y;
    if (v2->win.y < fminy) fminy = v2->win.y;
    if (v2->win.y > fmaxy) fmaxy = v2->win.y;

    int minx = win_to_pixel(fminx), maxx = win_to_pixel(fmaxx);
    int miny = win_to_pixel(fminy), maxy = win_to_pixel(fmaxy);

    int clip_x0 = 0, clip_y0 = 0;
    int clip_x1 = ctx->width - 1, clip_y1 = ctx->height - 1;
    if (ctx->scissor_test) {
        if (ctx->scissor_x > clip_x0) clip_x0 = ctx->scissor_x;
        if (ctx->scissor_y > clip_y0) clip_y0 = ctx->scissor_y;
        int sx1 = ctx->scissor_x + ctx->scissor_w - 1;
        int sy1 = ctx->scissor_y + ctx->scissor_h - 1;
        if (sx1 < clip_x1) clip_x1 = sx1;
        if (sy1 < clip_y1) clip_y1 = sy1;
    }
    if (minx < clip_x0) minx = clip_x0;
    if (miny < clip_y0) miny = clip_y0;
    if (maxx > clip_x1) maxx = clip_x1;
    if (maxy > clip_y1) maxy = clip_y1;
    if (minx > maxx || miny > maxy) return;

    /* Edge vectors, used for both the incremental edge functions and the
     * fill-rule bias. */
    GLfloat e0dx = v2->win.x - v1->win.x, e0dy = v2->win.y - v1->win.y;
    GLfloat e1dx = v0->win.x - v2->win.x, e1dy = v0->win.y - v2->win.y;
    GLfloat e2dx = v1->win.x - v0->win.x, e2dy = v1->win.y - v0->win.y;

    /* A shared edge is included only for the triangle that owns it.
     *
     * This is expressed as "is the comparison >= or > ?" rather than as a
     * numeric epsilon added to the edge value.  An absolute epsilon does not
     * work: edge-function magnitudes scale with the triangle's area (they are
     * twice a sub-triangle area), so a constant such as 1e-6 is meaningless
     * next to values in the thousands, and whether it has any effect at all
     * depends on the float rounding of the target.  That is exactly what
     * happened here — the epsilon version tiled correctly on the host and left
     * a diagonal seam under AuraLite.
     *
     * Comparing exactly against zero, and only changing the strictness, is
     * scale-free and gives the same result everywhere. */
    int own0 = is_top_left(e0dx, e0dy);
    int own1 = is_top_left(e1dx, e1dy);
    int own2 = is_top_left(e2dx, e2dy);

    GLfloat inv_area = 1.0f / area;
    int flat = (ctx->shade_model == GL_FLAT);

    /* ---- Phase G11c: is a shader program driving this draw? ----
     *
     * When one is, the fragment shader replaces the whole texture/fog/alpha
     * block below, and the varyings are interpolated in its place.  Depth,
     * culling, scissor and blending are unchanged: a shader alters neither
     * the window coordinates nor the meaning of a colour. */
    int use_shader = gl_shader_active(ctx);
    int nvary = use_shader ? v0->varying_count : 0;
    if (nvary > GL_MAX_VARYING_FLOATS) nvary = GL_MAX_VARYING_FLOATS;

    /* Varyings interpolate perspective-correctly, exactly like texture
     * coordinates: divided through by w at the vertices and multiplied back
     * per pixel.  Interpolating them linearly in screen space would make a
     * shader's inputs swim on a perspective triangle, the same artefact G6
     * fixed for UVs. */
    GLfloat vary0[GL_MAX_VARYING_FLOATS];
    GLfloat vary1[GL_MAX_VARYING_FLOATS];
    GLfloat vary2[GL_MAX_VARYING_FLOATS];

    /* ---- Phase G6/G10 per-fragment state, hoisted out of the loop ---- */
    int do_blend = ctx->blend;
    int do_fog   = ctx->fog;
    int do_alpha = ctx->alpha_test;

    /* Perspective-correct interpolation needs 1/w at each vertex.  Screen-
     * space linear interpolation of s and t is only correct for an
     * axis-aligned, unrotated quad; for anything else the texture visibly
     * "swims" as the primitive turns.  The fix is to interpolate s/w, t/w and
     * 1/w linearly (those ARE linear in screen space) and divide at each
     * pixel. */
    GLfloat w0i = v0->inv_w, w1i = v1->inv_w, w2i = v2->inv_w;

    for (int k = 0; k < nvary; k++) {
        vary0[k] = v0->varying[k] * w0i;
        vary1[k] = v1->varying[k] * w1i;
        vary2[k] = v2->varying[k] * w2i;
    }

    /* One entry per texture unit (G10).  `tex[u]` is NULL when the unit
     * contributes nothing, and the whole texturing block is skipped when no
     * unit does. */
    gl_texture_t *tex[GL_MAX_TEXTURE_UNITS_IMPL];
    GLfloat s0u[GL_MAX_TEXTURE_UNITS_IMPL], s1u[GL_MAX_TEXTURE_UNITS_IMPL],
            s2u[GL_MAX_TEXTURE_UNITS_IMPL];
    GLfloat t0u[GL_MAX_TEXTURE_UNITS_IMPL], t1u[GL_MAX_TEXTURE_UNITS_IMPL],
            t2u[GL_MAX_TEXTURE_UNITS_IMPL];
    GLfloat r0u[GL_MAX_TEXTURE_UNITS_IMPL], r1u[GL_MAX_TEXTURE_UNITS_IMPL],
            r2u[GL_MAX_TEXTURE_UNITS_IMPL];
    GLfloat lod[GL_MAX_TEXTURE_UNITS_IMPL];
    int any_tex = 0;

    for (int u = 0; u < GL_MAX_TEXTURE_UNITS_IMPL; u++) {
        tex[u] = gl_texture_unit_source(ctx, u);
        lod[u] = 0.0f;
        s0u[u] = v0->s[u] * w0i; s1u[u] = v1->s[u] * w1i; s2u[u] = v2->s[u] * w2i;
        t0u[u] = v0->t[u] * w0i; t1u[u] = v1->t[u] * w1i; t2u[u] = v2->t[u] * w2i;
        r0u[u] = v0->r[u] * w0i; r1u[u] = v1->r[u] * w1i; r2u[u] = v2->r[u] * w2i;
        if (tex[u]) {
            any_tex = 1;
            lod[u] = triangle_lod(tex[u], v0, v1, v2, area, u);
        }
    }
    /* Eye-space depth for fog, interpolated the same way. */
    GLfloat z0 = v0->eye.z * w0i, z1 = v1->eye.z * w1i, z2 = v2->eye.z * w2i;
    /* Flat shading takes the colour of the LAST vertex of the primitive
     * (§2.14.7); that is `c` as originally passed, before any swap. */
    gl_color_t flat_src_color = c->color;

    int has_depth = (ctx->depth != (float *)0);
    int do_depth_test  = ctx->depth_test && has_depth;
    int do_depth_write = ctx->depth_mask && has_depth;

    /* Sample at pixel centres: pixel (i,j) is sampled at (i+0.5, j+0.5). */
    GLfloat px0 = (GLfloat)minx + 0.5f;
    GLfloat py0 = (GLfloat)miny + 0.5f;

    /* Edge function values at the first sample, then stepped incrementally:
     * one add per pixel instead of a full evaluation. */
    GLfloat w0_row = (py0 - v1->win.y) * e0dx - (px0 - v1->win.x) * e0dy;
    GLfloat w1_row = (py0 - v2->win.y) * e1dx - (px0 - v2->win.x) * e1dy;
    GLfloat w2_row = (py0 - v0->win.y) * e2dx - (px0 - v0->win.x) * e2dy;

    for (int y = miny; y <= maxy; y++) {
        GLfloat w0 = w0_row, w1 = w1_row, w2 = w2_row;

        gl_pixel_t *crow = gl_fb_row(ctx, y);
        float      *drow = has_depth ? gl_depth_row(ctx, y) : (float *)0;

        for (int x = minx; x <= maxx; x++) {
            /* Inside when every edge function is positive, or zero on an edge
             * this triangle owns under the top-left rule. */
            int in0 = own0 ? (w0 >= 0.0f) : (w0 > 0.0f);
            int in1 = own1 ? (w1 >= 0.0f) : (w1 > 0.0f);
            int in2 = own2 ? (w2 >= 0.0f) : (w2 > 0.0f);
            if (in0 && in1 && in2) {
                GLfloat l0 = w0 * inv_area;   /* barycentric weight of v0 */
                GLfloat l1 = w1 * inv_area;   /*                      v1 */
                GLfloat l2 = w2 * inv_area;   /*                      v2 */

                /* Depth interpolates linearly in window space (§3.5.1). */
                GLfloat z = l0 * v0->win.z + l1 * v1->win.z + l2 * v2->win.z;

                int write = 1;
                if (do_depth_test) {
                    if (!depth_passes(ctx->depth_func, z, drow[x])) write = 0;
                }

                if (write) {
                    /* ---- The shader path ----
                     *
                     * A bound program replaces everything from here to the
                     * alpha test: the fragment shader computes the colour,
                     * and discard is its own decision rather than an alpha
                     * comparison.  Blending and the depth write below still
                     * apply, because those are framebuffer operations. */
                    if (use_shader) {
                        GLfloat inv_w = l0 * w0i + l1 * w1i + l2 * w2i;
                        GLfloat rw = (inv_w > 1e-20f || inv_w < -1e-20f)
                                   ? 1.0f / inv_w : 0.0f;

                        GLfloat fv[GL_MAX_VARYING_FLOATS];
                        for (int k = 0; k < nvary; k++) {
                            fv[k] = (l0 * vary0[k] + l1 * vary1[k]
                                   + l2 * vary2[k]) * rw;
                        }

                        gl_color_t sc;
                        sc.r = sc.g = sc.b = 0.0f;
                        sc.a = 1.0f;
                        if (!gl_shader_run_fragment(ctx, fv,
                                                    (GLfloat)x + 0.5f,
                                                    (GLfloat)y + 0.5f,
                                                    z, is_front, &sc)) {
                            goto next_pixel;     /* the shader discarded */
                        }

                        if (do_depth_write) drow[x] = z;

                        if (do_blend) {
                            uint32_t d = crow[x];
                            gl_color_t dst;
                            dst.r = (GLfloat)((d >> 16) & 0xFF) / 255.0f;
                            dst.g = (GLfloat)((d >>  8) & 0xFF) / 255.0f;
                            dst.b = (GLfloat)( d        & 0xFF) / 255.0f;
                            dst.a = 1.0f;
                            sc = gl_blend(ctx, sc, dst);
                        }
                        crow[x] = gl_pack_color(sc);
                        goto next_pixel;
                    }

                    /* ---- Fragment colour ---- */
                    gl_color_t cc;
                    if (flat) {
                        cc = flat_src_color;
                    } else {
                        cc.r = l0 * v0->color.r + l1 * v1->color.r + l2 * v2->color.r;
                        cc.g = l0 * v0->color.g + l1 * v1->color.g + l2 * v2->color.g;
                        cc.b = l0 * v0->color.b + l1 * v1->color.b + l2 * v2->color.b;
                        cc.a = l0 * v0->color.a + l1 * v1->color.a + l2 * v2->color.a;
                    }

                    /* ---- Texturing, perspective-correct, unit by unit ----
                     *
                     * The units are applied IN ORDER: unit 0's output is the
                     * incoming fragment colour for unit 1.  That chaining is
                     * what makes GL_MODULATE on two units produce the product
                     * of both textures, and it is the whole of fixed-function
                     * multitexturing (§3.8.10). */
                    if (any_tex) {
                        GLfloat inv_w = l0 * w0i + l1 * w1i + l2 * w2i;
                        if (inv_w > 1e-20f || inv_w < -1e-20f) {
                            GLfloat rw = 1.0f / inv_w;
                            for (int u = 0; u < GL_MAX_TEXTURE_UNITS_IMPL; u++) {
                                if (!tex[u]) continue;
                                GLfloat ss = (l0 * s0u[u] + l1 * s1u[u]
                                            + l2 * s2u[u]) * rw;
                                GLfloat tt = (l0 * t0u[u] + l1 * t1u[u]
                                            + l2 * t2u[u]) * rw;
                                GLfloat rr = (l0 * r0u[u] + l1 * r1u[u]
                                            + l2 * r2u[u]) * rw;
                                gl_color_t tc =
                                    gl_texture_sample_lod(tex[u], ss, tt, rr,
                                                          lod[u]);
                                cc = gl_texture_env_unit(ctx, u, cc, tc);
                            }
                        }
                    }

                    /* ---- Fog ---- */
                    if (do_fog) {
                        GLfloat inv_w = l0 * w0i + l1 * w1i + l2 * w2i;
                        GLfloat eye_z = 0.0f;
                        if (inv_w > 1e-20f || inv_w < -1e-20f) {
                            eye_z = (l0 * z0 + l1 * z1 + l2 * z2) / inv_w;
                        }
                        /* Fog is a function of DISTANCE from the eye, and eye
                         * space puts the viewer at the origin looking down
                         * -z, so the distance is |eye.z|. */
                        GLfloat dist = eye_z < 0.0f ? -eye_z : eye_z;
                        cc = gl_fog_apply(ctx, cc, dist);
                    }

                    /* ---- Alpha test, before anything is written ---- */
                    if (do_alpha && !gl_alpha_test_passes(ctx, cc.a)) {
                        goto next_pixel;
                    }

                    /* The depth write happens only once the fragment has
                     * survived every discarding test. */
                    if (do_depth_write) drow[x] = z;

                    /* ---- Blending ---- */
                    if (do_blend) {
                        uint32_t d = crow[x];
                        gl_color_t dst;
                        dst.r = (GLfloat)((d >> 16) & 0xFF) / 255.0f;
                        dst.g = (GLfloat)((d >>  8) & 0xFF) / 255.0f;
                        dst.b = (GLfloat)( d        & 0xFF) / 255.0f;
                        dst.a = 1.0f;   /* no destination alpha channel */
                        cc = gl_blend(ctx, cc, dst);
                    }

                    crow[x] = gl_pack_color(cc);
                }
            next_pixel:;
            }
            /* Stepping one pixel right changes E by -dy ... */
            w0 -= e0dy;
            w1 -= e1dy;
            w2 -= e2dy;
        }
        /* ... and one pixel up changes it by +dx. */
        w0_row += e0dx;
        w1_row += e1dx;
        w2_row += e2dx;
    }
}

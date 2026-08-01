/* libgl/src/glclip.c — frustum clipping (GL 1.1 §2.11).
 *
 * Phase G4 of GL_PLAN.md.
 *
 * WHY THIS IS NEEDED
 *
 * Up to G3 a vertex with clip w <= 0 was simply marked invalid and any
 * primitive touching it was dropped whole.  That is safe but wrong: a triangle
 * with one vertex behind the eye and two in front is genuinely visible, and
 * dropping it makes geometry vanish as the camera approaches — the classic
 * "walls disappear when you walk into them" artefact.
 *
 * WHERE THE CLIP HAPPENS
 *
 * In CLIP space, before the perspective divide.  This matters: after the
 * divide the sign information in w is gone, and a vertex behind the eye
 * projects to a mirrored position in front of it, so no amount of 2D clipping
 * can recover the correct shape.  The six frustum planes in clip space are
 *
 *     -w <= x <= w      -w <= y <= w      -w <= z <= w
 *
 * which is what makes them cheap to test: each is one subtraction.
 *
 * ALGORITHM
 *
 * Sutherland-Hodgman: run the polygon through one plane at a time, emitting a
 * new polygon each pass.  For each edge, keep inside vertices and, where the
 * edge crosses the plane, emit the intersection.  All vertex attributes
 * (colour, normal, texture coordinates) are interpolated at the crossing with
 * the same parameter t, so a clipped triangle shades exactly as the unclipped
 * one would have.
 *
 * A triangle clipped by all six planes can become at most a 9-gon (each plane
 * adds at most one vertex), which is then fanned back into triangles.
 */

#include "GL/gl.h"
#include "GL/glmath.h"
#include "glcontext.h"
#include "glvertex.h"

/* A triangle can gain at most one vertex per clipping plane, so 3 + 6 = 9 is
 * the exact bound.  The extra slots are slack for defensive safety. */
#define GL_CLIP_MAX_VERTS 12

/* Signed distance to a clip plane; positive means inside.
 *
 * The planes are indexed 0..5 as -w<=x, x<=w, -w<=y, y<=w, -w<=z, z<=w.
 */
static GLfloat plane_distance(const gl_vertex_t *v, int plane) {
    switch (plane) {
    case 0: return v->clip.w + v->clip.x;   /*  x >= -w */
    case 1: return v->clip.w - v->clip.x;   /*  x <=  w */
    case 2: return v->clip.w + v->clip.y;   /*  y >= -w */
    case 3: return v->clip.w - v->clip.y;   /*  y <=  w */
    case 4: return v->clip.w + v->clip.z;   /*  z >= -w */
    case 5: return v->clip.w - v->clip.z;   /*  z <=  w */
    default: return 0.0f;
    }
}

/* Linear interpolation of a whole vertex at parameter t along a->b.
 *
 * Every attribute is interpolated here, not just position: a clipped triangle
 * must shade identically to the part of the original it replaces.  Texture
 * coordinates are interpolated linearly in clip space, which is correct
 * because perspective correction is applied later, per fragment (G6). */
static void lerp_vertex(const gl_vertex_t *a, const gl_vertex_t *b,
                        GLfloat t, gl_vertex_t *out) {
    out->clip.x = a->clip.x + (b->clip.x - a->clip.x) * t;
    out->clip.y = a->clip.y + (b->clip.y - a->clip.y) * t;
    out->clip.z = a->clip.z + (b->clip.z - a->clip.z) * t;
    out->clip.w = a->clip.w + (b->clip.w - a->clip.w) * t;

    out->color.r = a->color.r + (b->color.r - a->color.r) * t;
    out->color.g = a->color.g + (b->color.g - a->color.g) * t;
    out->color.b = a->color.b + (b->color.b - a->color.b) * t;
    out->color.a = a->color.a + (b->color.a - a->color.a) * t;

    out->normal.x = a->normal.x + (b->normal.x - a->normal.x) * t;
    out->normal.y = a->normal.y + (b->normal.y - a->normal.y) * t;
    out->normal.z = a->normal.z + (b->normal.z - a->normal.z) * t;

    out->s = a->s + (b->s - a->s) * t;
    out->t = a->t + (b->t - a->t) * t;

    /* win/inv_w are recomputed by the viewport transform after clipping. */
    out->valid = 1;
}

/* Clip a polygon against one plane.  Reads `in` (count n), writes `out`,
 * returns the new vertex count. */
static int clip_against_plane(const gl_vertex_t *in, int n,
                              gl_vertex_t *out, int plane) {
    if (n == 0) return 0;

    int out_n = 0;
    for (int i = 0; i < n; i++) {
        const gl_vertex_t *cur  = &in[i];
        const gl_vertex_t *next = &in[(i + 1) % n];

        GLfloat d_cur  = plane_distance(cur, plane);
        GLfloat d_next = plane_distance(next, plane);

        int cur_in  = (d_cur  >= 0.0f);
        int next_in = (d_next >= 0.0f);

        if (cur_in) {
            if (out_n < GL_CLIP_MAX_VERTS) out[out_n++] = *cur;
        }

        /* Crossing the plane in either direction produces one new vertex. */
        if (cur_in != next_in) {
            GLfloat denom = d_cur - d_next;
            /* denom cannot be zero when the signs differ, but guard anyway:
             * a NaN coordinate could make both comparisons false in a way
             * that reaches here. */
            if (denom != 0.0f && out_n < GL_CLIP_MAX_VERTS) {
                GLfloat t = d_cur / denom;
                lerp_vertex(cur, next, t, &out[out_n++]);
            }
        }
    }
    return out_n;
}

/* Apply the perspective divide and viewport transform to a clipped vertex.
 *
 * This duplicates the tail of gl_transform_vertex() because clipping happens
 * between the two halves of that function: transform to clip space, clip, then
 * project what survived. */
void gl_project_vertex(struct aglx_context *ctx, gl_vertex_t *v) {
    if (v->clip.w <= 1e-20f && v->clip.w >= -1e-20f) {
        v->valid = 0;
        return;
    }
    v->inv_w = 1.0f / v->clip.w;

    GLfloat nx = v->clip.x * v->inv_w;
    GLfloat ny = v->clip.y * v->inv_w;
    GLfloat nz = v->clip.z * v->inv_w;

    v->win.x = (GLfloat)ctx->viewport_x
             + ((nx + 1.0f) * 0.5f) * (GLfloat)ctx->viewport_w;
    v->win.y = (GLfloat)ctx->viewport_y
             + ((ny + 1.0f) * 0.5f) * (GLfloat)ctx->viewport_h;
    v->win.z = (nz + 1.0f) * 0.5f;
    v->valid = 1;
}

/* Is the vertex inside all six planes?  Used to skip the whole clip machinery
 * for the common case where nothing needs clipping. */
static int vertex_inside_frustum(const gl_vertex_t *v) {
    for (int p = 0; p < 6; p++) {
        if (plane_distance(v, p) < 0.0f) return 0;
    }
    return 1;
}

/* Clip a triangle and emit the resulting triangles through `emit`.
 *
 * The three input vertices must be in CLIP space with their attributes set;
 * win/inv_w are filled in here after clipping. */
void gl_clip_and_emit_triangle(struct aglx_context *ctx,
                               const gl_vertex_t *a, const gl_vertex_t *b,
                               const gl_vertex_t *c,
                               void (*emit)(struct aglx_context *,
                                            const gl_vertex_t *,
                                            const gl_vertex_t *,
                                            const gl_vertex_t *)) {
    /* Fast path: everything inside, which is the overwhelmingly common case.
     * Skipping the six-plane walk here keeps ordinary geometry at G3 speed. */
    if (vertex_inside_frustum(a) && vertex_inside_frustum(b) &&
        vertex_inside_frustum(c)) {
        gl_vertex_t v0 = *a, v1 = *b, v2 = *c;
        gl_project_vertex(ctx, &v0);
        gl_project_vertex(ctx, &v1);
        gl_project_vertex(ctx, &v2);
        if (v0.valid && v1.valid && v2.valid) emit(ctx, &v0, &v1, &v2);
        return;
    }

    /* Trivial reject: if every vertex is outside the SAME plane, the whole
     * triangle is outside.  (The converse is not true, hence the full clip
     * below for anything that survives this test.) */
    for (int p = 0; p < 6; p++) {
        if (plane_distance(a, p) < 0.0f &&
            plane_distance(b, p) < 0.0f &&
            plane_distance(c, p) < 0.0f) {
            return;
        }
    }

    gl_vertex_t buf_a[GL_CLIP_MAX_VERTS];
    gl_vertex_t buf_b[GL_CLIP_MAX_VERTS];

    buf_a[0] = *a; buf_a[1] = *b; buf_a[2] = *c;
    int n = 3;

    /* Ping-pong between the two buffers, one plane per pass. */
    gl_vertex_t *src = buf_a, *dst = buf_b;
    for (int p = 0; p < 6 && n > 0; p++) {
        n = clip_against_plane(src, n, dst, p);
        gl_vertex_t *tmp = src; src = dst; dst = tmp;
    }
    if (n < 3) return;      /* clipped away entirely */

    /* Project the survivors, then fan the polygon back into triangles. */
    for (int i = 0; i < n; i++) gl_project_vertex(ctx, &src[i]);

    for (int i = 1; i + 1 < n; i++) {
        if (src[0].valid && src[i].valid && src[i + 1].valid) {
            emit(ctx, &src[0], &src[i], &src[i + 1]);
        }
    }
}

/* Clip a line segment against the frustum and emit what survives.
 *
 * Lines use the same plane tests but a simpler algorithm: track a parametric
 * range [t0,t1] along the segment and shrink it plane by plane (Liang-Barsky).
 * This avoids allocating a vertex list for what can only ever stay a segment.
 */
void gl_clip_and_emit_line(struct aglx_context *ctx,
                           const gl_vertex_t *a, const gl_vertex_t *b,
                           void (*emit)(struct aglx_context *,
                                        const gl_vertex_t *,
                                        const gl_vertex_t *)) {
    if (vertex_inside_frustum(a) && vertex_inside_frustum(b)) {
        gl_vertex_t v0 = *a, v1 = *b;
        gl_project_vertex(ctx, &v0);
        gl_project_vertex(ctx, &v1);
        if (v0.valid && v1.valid) emit(ctx, &v0, &v1);
        return;
    }

    GLfloat t0 = 0.0f, t1 = 1.0f;

    for (int p = 0; p < 6; p++) {
        GLfloat da = plane_distance(a, p);
        GLfloat db = plane_distance(b, p);

        if (da < 0.0f && db < 0.0f) return;      /* fully outside this plane */
        if (da >= 0.0f && db >= 0.0f) continue;  /* fully inside this plane  */

        GLfloat denom = da - db;
        if (denom == 0.0f) continue;
        GLfloat t = da / denom;

        if (da < 0.0f) {
            /* Entering the half-space: raise the start of the range. */
            if (t > t0) t0 = t;
        } else {
            /* Leaving it: lower the end. */
            if (t < t1) t1 = t;
        }
        if (t0 > t1) return;                     /* range collapsed */
    }

    gl_vertex_t v0, v1;
    lerp_vertex(a, b, t0, &v0);
    lerp_vertex(a, b, t1, &v1);
    gl_project_vertex(ctx, &v0);
    gl_project_vertex(ctx, &v1);
    if (v0.valid && v1.valid) emit(ctx, &v0, &v1);
}

/* Clip a point: a point is either wholly inside or wholly outside. */
void gl_clip_and_emit_point(struct aglx_context *ctx, const gl_vertex_t *a,
                            void (*emit)(struct aglx_context *,
                                         const gl_vertex_t *)) {
    if (!vertex_inside_frustum(a)) return;
    gl_vertex_t v = *a;
    gl_project_vertex(ctx, &v);
    if (v.valid) emit(ctx, &v);
}

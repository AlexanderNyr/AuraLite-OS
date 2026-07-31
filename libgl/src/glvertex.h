/* libgl/src/glvertex.h — post-transform vertex and the internal rasterizer API.
 *
 * PRIVATE to libgl.
 *
 * COORDINATE SPACES, in the order the pipeline walks them (GL 1.1 §2.10):
 *
 *   object  --MODELVIEW-->  eye  --PROJECTION-->  clip
 *           --divide by w-->  NDC  --viewport-->  window
 *
 * `clip` is kept alongside `win` because phase G4 needs the pre-divide values
 * to clip against the frustum, and phase G6 needs 1/w for perspective-correct
 * attribute interpolation.  Storing them now avoids reworking the vertex type
 * later.
 *
 * WINDOW COORDINATES USE GL SEMANTICS: the origin is the BOTTOM-left corner
 * and y grows upwards.  AuraLite's framebuffer is the other way up, so the
 * flip happens at the moment a pixel is addressed, via gl_fb_row().  Doing it
 * there rather than in the viewport transform keeps window coordinates
 * meaningful for glScissor and a future glReadPixels, and costs nothing: the
 * rasterizer computes a row pointer once per scanline, not once per pixel.
 */
#ifndef AURALITE_GL_VERTEX_H
#define AURALITE_GL_VERTEX_H

#include <stddef.h>

#include "GL/gl.h"
#include "GL/glmath.h"
#include "glcontext.h"

/* A vertex that has been through the transform stage. */
typedef struct {
    glm_vec4   clip;    /* clip coordinates (after PROJECTION, before divide) */
    glm_vec3   win;     /* window coordinates: x,y in pixels, z in [0,1]      */
    GLfloat    inv_w;   /* 1/clip.w, kept for perspective-correct interp (G6) */
    gl_color_t color;   /* colour captured when the vertex was specified      */
    glm_vec3   normal;  /* eye-space normal (used from G5)                    */
    GLfloat    s, t;    /* texture coordinates (used from G6)                 */
    int        valid;   /* 0 when the vertex is behind the eye (w <= 0)       */
} gl_vertex_t;

/* Pointer to the first pixel of window-space row `y`, with the vertical flip
 * from GL's bottom-left origin to the framebuffer's top-left origin applied.
 * Callers must have already checked 0 <= y < ctx->height. */
static inline gl_pixel_t *gl_fb_row(struct aglx_context *ctx, int y) {
    return ctx->color + (size_t)(ctx->height - 1 - y) * (size_t)ctx->width;
}

/* Same, for the depth buffer.  Returns NULL when the context has no depth
 * buffer, so callers must check. */
static inline float *gl_depth_row(struct aglx_context *ctx, int y) {
    if (!ctx->depth) return (float *)0;
    return ctx->depth + (size_t)(ctx->height - 1 - y) * (size_t)ctx->width;
}

/* ---- Rasterizer entry points (libgl/src/glraster.c) ----
 *
 * These take vertices already in window coordinates and are responsible for
 * clipping to the framebuffer, so they can be handed anything.
 */
void gl_raster_point(struct aglx_context *ctx, const gl_vertex_t *v);
void gl_raster_line(struct aglx_context *ctx,
                    const gl_vertex_t *a, const gl_vertex_t *b);

/* Phase G2 draws triangles as wireframe outlines; phase G3 replaces the body
 * of this function with a filled, depth-tested, edge-function rasterizer.
 * The primitive-assembly code in glimm.c calls it either way and does not
 * change between the phases. */
void gl_raster_triangle(struct aglx_context *ctx,
                        const gl_vertex_t *a, const gl_vertex_t *b,
                        const gl_vertex_t *c);

/* ---- Transform stage (libgl/src/glimm.c) ---- */

/* Run object coordinates through MODELVIEW, PROJECTION, the perspective
 * divide and the viewport transform, producing a window-space vertex. */
void gl_transform_vertex(struct aglx_context *ctx,
                         GLfloat x, GLfloat y, GLfloat z, GLfloat w,
                         gl_vertex_t *out);

#endif /* AURALITE_GL_VERTEX_H */

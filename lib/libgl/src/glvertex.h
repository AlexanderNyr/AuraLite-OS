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
    glm_vec3   eye;     /* eye-space position, needed by lighting (G5)        */
    glm_vec3   win;     /* window coordinates: x,y in pixels, z in [0,1]      */
    GLfloat    inv_w;   /* 1/clip.w, kept for perspective-correct interp (G6) */
    gl_color_t color;   /* colour captured when the vertex was specified      */
    glm_vec3   normal;  /* eye-space normal (used from G5)                    */
    /* Texture coordinates, one set per texture unit (G6 used a single s,t;
     * G10 made it per-unit for multitexturing).  `r` is the third coordinate,
     * used by 3D textures and as the Z component of a cube-map direction. */
    GLfloat    s[GL_MAX_TEXTURE_UNITS_IMPL];
    GLfloat    t[GL_MAX_TEXTURE_UNITS_IMPL];
    GLfloat    r[GL_MAX_TEXTURE_UNITS_IMPL];

    /* ---- Varyings (phase G11c) ----
     *
     * A vertex shader's outputs, interpolated across the primitive and read
     * by the fragment shader.  Stored as a flat float array because the
     * clipper and the rasterizer only ever need to lerp them: neither has any
     * reason to know that floats 4..6 are somebody's `vNormal`.  The program's
     * varying table maps names to offsets, once, at link time.
     *
     * `varying_count` is 0 on the fixed-function path, so the extra work in
     * the clipper and rasterizer costs a loop that runs zero times. */
    GLfloat    varying[GL_MAX_VARYING_FLOATS];
    int        varying_count;

    int        valid;   /* 0 when the vertex is behind the eye (w <= 0)       */
} gl_vertex_t;

/* Pointer to the first pixel of window-space row `y`.
 *
 * GL window coordinates put y=0 at the BOTTOM.  Whether that needs flipping
 * depends on the target: the window's framebuffer stores row 0 at the top, a
 * texture stores it at the bottom.  ctx->target_flip_y says which, and is
 * maintained by glfbo.c when the render target changes (phase G12).  Before
 * G12 there was only ever one kind of target and the flip was unconditional.
 *
 * Callers must have already checked 0 <= y < ctx->height. */
static inline size_t gl_row_index(const struct aglx_context *ctx, int y) {
    return ctx->target_flip_y ? (size_t)(ctx->height - 1 - y) : (size_t)y;
}

static inline gl_pixel_t *gl_fb_row(struct aglx_context *ctx, int y) {
    return ctx->color + gl_row_index(ctx, y) * (size_t)ctx->width;
}

/* Same, for the depth buffer.  Returns NULL when the context has no depth
 * buffer, so callers must check. */
static inline float *gl_depth_row(struct aglx_context *ctx, int y) {
    if (!ctx->depth) return (float *)0;
    return ctx->depth + gl_row_index(ctx, y) * (size_t)ctx->width;
}

/* Same, for the stencil plane.  Returns NULL when the context has no stencil
 * buffer, so callers must check. */
static inline uint8_t *gl_stencil_row(struct aglx_context *ctx, int y) {
    if (!ctx->stencil) return (uint8_t *)0;
    return ctx->stencil + gl_row_index(ctx, y) * (size_t)ctx->width;
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

/* Run object coordinates through MODELVIEW and PROJECTION, leaving the vertex
 * in CLIP space.  The perspective divide and viewport transform deliberately
 * do NOT happen here: from phase G4 clipping runs between the two halves, and
 * clipping must see the pre-divide values (after the divide the sign of w is
 * gone and a vertex behind the eye is indistinguishable from one in front). */
void gl_transform_vertex(struct aglx_context *ctx,
                         GLfloat x, GLfloat y, GLfloat z, GLfloat w,
                         gl_vertex_t *out);

/* ---- Backend dispatch (libgl/src/glbackend.c), phase G9 ----
 *
 * Each returns 0 when the backend handled the operation and non-zero when the
 * caller must run its software path.  Keeping the fallback decision here lets
 * a hardware backend implement operations one at a time.
 */
int  gl_backend_try_clear(struct aglx_context *ctx, GLbitfield mask);
int  gl_backend_try_present(struct aglx_context *ctx);
void gl_backend_notify_destroy(struct aglx_context *ctx);
void gl_backend_init_defaults(void);
void gl_virgl_register(void);

/* ---- Vertex arrays and display lists (phase G7) ---- */
void gl_array_set_defaults(struct aglx_context *ctx);
void gl_array_free_all(struct aglx_context *ctx);

/* Record a command into the list being compiled.  Returns 1 when the caller
 * must NOT execute the command (GL_COMPILE), 0 to proceed (no list open, or
 * GL_COMPILE_AND_EXECUTE). */
int gl_list_record(struct aglx_context *ctx, GLuint op,
                   const GLfloat *f, int nf, const GLint *i, int ni);
int gl_list_compiling(const struct aglx_context *ctx);

GLuint gl_lop_begin(void);
GLuint gl_lop_end(void);
GLuint gl_lop_vertex4f(void);
GLuint gl_lop_color4f(void);
GLuint gl_lop_normal3f(void);
GLuint gl_lop_texcoord2f(void);
GLuint gl_lop_push_matrix(void);
GLuint gl_lop_pop_matrix(void);
GLuint gl_lop_load_identity(void);
GLuint gl_lop_translatef(void);
GLuint gl_lop_rotatef(void);
GLuint gl_lop_scalef(void);
GLuint gl_lop_matrix_mode(void);
GLuint gl_lop_enable(void);
GLuint gl_lop_disable(void);
GLuint gl_lop_multitexcoord(void);
GLuint gl_lop_active_texture(void);

/* ---- Texturing (libgl/src/gltexture.c), phases G6 and G10 ----
 *
 * gl_texture_unit_source() returns the texture unit `unit` should sample, or
 * NULL when that unit contributes nothing.  Target priority follows the
 * specification (§3.8.15): cube map beats 3D beats 2D when several are
 * enabled on the same unit.
 */
gl_texture_t *gl_texture_unit_source(struct aglx_context *ctx, int unit);

/* Sample with an explicit level of detail.  `lod` is the continuous mipmap
 * level; the filters decide whether to round it, or to blend two levels.
 * Pass lod <= 0 for magnification. */
gl_color_t gl_texture_sample_lod(const gl_texture_t *t,
                                 GLfloat s, GLfloat tc, GLfloat rc,
                                 GLfloat lod);

/* G6-compatible entry point: no mipmapping, `magnifying` selects the filter. */
gl_color_t gl_texture_sample(const gl_texture_t *t, GLfloat s, GLfloat tc,
                             int magnifying);

/* Apply unit `unit`'s environment to `frag` using `tex`.
 * `primary` is the fragment colour before any texture unit ran — COMBINE's
 * PRIMARY_COLOR source, and PREVIOUS on unit 0. */
gl_color_t gl_texture_env_unit(const struct aglx_context *ctx, int unit,
                               gl_color_t frag, gl_color_t tex,
                               gl_color_t primary);
gl_color_t gl_texture_env(const struct aglx_context *ctx,
                          gl_color_t frag, gl_color_t tex);

/* Level-0 dimensions of the image the sampler will read, used by the
 * rasterizer to size the per-triangle LOD.  Returns 0 when unavailable. */
int gl_texture_base_size(const gl_texture_t *t, GLfloat *w_out, GLfloat *h_out);

/* Does this texture have a usable mipmap chain AND a mipmapping min filter? */
int gl_texture_uses_mipmaps(const gl_texture_t *t);

void gl_texture_set_defaults(struct aglx_context *ctx);
void gl_texture_free_all(struct aglx_context *ctx);

/* ---- Shader pipeline (libgl/src/glshaderpipe.c), phase G11c ----
 *
 * The rasterizer and the clipper consult gl_shader_active() to decide which
 * of the two fragment paths to run; everything else about them is unchanged.
 */
int  gl_shader_active(struct aglx_context *ctx);
int  gl_shader_varying_floats(struct aglx_context *ctx);
int  gl_shader_run_vertex(struct aglx_context *ctx, int index,
                          gl_vertex_t *out);
/* GL2 phase L5: 1 when the bound program must be shaded BEFORE the stencil
 * and depth operations (it can discard, or write gl_FragDepth once the
 * language grows one); 0 -- including the no-program case -- when early-Z is
 * correct. */
int  gl_shader_may_kill_early_z(struct aglx_context *ctx);

/* Test-only side channel for the early-Z gate (L5): how many times the
 * fragment interpreter has actually run since the last reset.  The early-Z
 * host test counts invocations; production code never reads this. */
void gl_shader_fs_count_reset(void);
long gl_shader_fs_count(void);

int  gl_shader_run_fragment(struct aglx_context *ctx, const float *varyings,
                            float x, float y, float z, int front_facing,
                            gl_color_t *out);
/* Push a shader-produced vertex into primitive assembly, bypassing the
 * fixed-function transform and attribute latching. */
void gl_imm_submit_vertex(struct aglx_context *ctx, const gl_vertex_t *v);

/* glBegin/glEnd as the DRAW CALLS use them.  glBegin refuses to open a batch
 * while a program is bound -- immediate mode has no attributes for a vertex
 * shader to read -- and glDrawArrays needs an exemption from that rule. */
void gl_imm_begin_internal(GLenum mode);
void gl_imm_end_internal(void);

/* 1 while a glBegin/glEnd pair is open. */
int  gl_imm_in_begin(void);

void gl_shader_set_defaults(struct aglx_context *ctx);
void gl_shader_free_all(struct aglx_context *ctx);

/* ---- Framebuffer objects (libgl/src/glfbo.c), phase G12 ----
 *
 * The rasterizer never calls into these: an FBO works by re-pointing
 * ctx->color/depth/width/height, so the drawing code stays unaware of it.
 * Only the context lifecycle and the entry points that must refuse to draw
 * into an incomplete target touch this interface.
 */
void gl_fbo_set_defaults(struct aglx_context *ctx);
void gl_fbo_free_all(struct aglx_context *ctx);

/* Re-point the render target at the current binding.  Called after the window
 * buffers change (create, resize). */
void gl_fbo_refresh(struct aglx_context *ctx);

/* 0 when the bound framebuffer is incomplete and nothing may be drawn. */
int gl_fbo_target_ok(struct aglx_context *ctx);

/* ---- Per-fragment operations (libgl/src/glfrag.c), phase G6 ---- */
int        gl_alpha_test_passes(const struct aglx_context *ctx, GLfloat alpha);
gl_color_t gl_blend(const struct aglx_context *ctx,
                    gl_color_t src, gl_color_t dst);
GLfloat    gl_fog_factor(const struct aglx_context *ctx, GLfloat z);
gl_color_t gl_fog_apply(const struct aglx_context *ctx,
                        gl_color_t c, GLfloat z);
void       gl_frag_set_defaults(struct aglx_context *ctx);

/* ---- Lighting (libgl/src/gllight.c), phase G5 ----
 *
 * Evaluated per vertex in EYE space, as GL 1.1 specifies; the rasterizer then
 * Gouraud-interpolates the result.  `back_face` selects the back material and
 * reverses the normal.
 */
gl_color_t gl_light_vertex(struct aglx_context *ctx,
                           glm_vec3 eye_pos, glm_vec3 normal,
                           gl_color_t vertex_color, int back_face);
void gl_lighting_set_defaults(struct aglx_context *ctx);

/* ---- Clipping (libgl/src/glclip.c), phase G4 ----
 *
 * Each takes CLIP-space vertices, clips against the six frustum planes,
 * applies the perspective divide and viewport transform to what survives, and
 * calls `emit` with window-space vertices.  A clipped triangle may emit
 * several triangles; a fully outside primitive emits none.
 */
void gl_project_vertex(struct aglx_context *ctx, gl_vertex_t *v);

void gl_clip_and_emit_triangle(struct aglx_context *ctx,
                               const gl_vertex_t *a, const gl_vertex_t *b,
                               const gl_vertex_t *c,
                               void (*emit)(struct aglx_context *,
                                            const gl_vertex_t *,
                                            const gl_vertex_t *,
                                            const gl_vertex_t *));

void gl_clip_and_emit_line(struct aglx_context *ctx,
                           const gl_vertex_t *a, const gl_vertex_t *b,
                           void (*emit)(struct aglx_context *,
                                        const gl_vertex_t *,
                                        const gl_vertex_t *));

void gl_clip_and_emit_point(struct aglx_context *ctx, const gl_vertex_t *a,
                            void (*emit)(struct aglx_context *,
                                         const gl_vertex_t *));

#endif /* AURALITE_GL_VERTEX_H */

/* libgl/src/glshaderpipe.c — running shaders inside the pipeline.
 *
 * Phase G11c of GL_PLAN.md.  This is the seam between the interpreter (G11b)
 * and the rasterizer: it builds the glsl_env_t a shader sees, runs the vertex
 * stage in place of the transform, and the fragment stage in place of
 * texturing, lighting and fog.
 *
 * WHAT REPLACES WHAT
 *
 *   vertex shader   -> gl_transform_vertex()'s matrix multiplies
 *   varyings        -> interpolated by the EXISTING perspective-correct
 *                      machinery in glraster.c, which needed one loop added
 *   fragment shader -> the texture/fog/lighting block in the inner loop
 *
 * Clipping, culling, the depth test, blending and the scissor box are
 * untouched: they operate on window coordinates and a colour, and a shader
 * changes neither of those contracts.  That is why this phase is a few
 * hundred lines rather than a rewrite.
 *
 * THE ENVIRONMENT IS BUILT PER DRAW, NOT PER FRAGMENT
 *
 * A glsl_env_t holds pointers to the context and the program, so constructing
 * one is free; what is NOT free is resolving a name to a uniform slot, and
 * that happens inside read_var on every access.  For a fragment shader at
 * 76 800 pixels that is the dominant cost after the interpretation itself.
 * It is left as a name lookup here because G11b's interface is by name and
 * changing it is G11d's business, not this phase's -- and because the honest
 * measurement (see docs/opengl.md) is that the shader path is two orders of
 * magnitude off the fixed-function one either way.
 */

#include <string.h>

#include "GL/gl.h"
#include "GL/auraglx.h"
#include "glcontext.h"
#include "glvertex.h"
#include "glsl.h"

/* Declared in glshader.c. */
const gl_uniform_t     *gl_program_find_uniform(const gl_program_t *p,
                                                const char *name);
const gl_varying_t     *gl_program_find_varying(const gl_program_t *p,
                                                const char *name);
const gl_attrib_info_t *gl_program_find_attrib(const gl_program_t *p,
                                               const char *name);
void *gl_program_vertex_unit(struct aglx_context *ctx, const gl_program_t *p);
void *gl_program_fragment_unit(struct aglx_context *ctx,
                               const gl_program_t *p);
gl_program_t *gl_program_current(struct aglx_context *ctx);

/* ============================================================================
 * The environment a shader runs in
 * ==========================================================================*/

typedef struct {
    glsl_env_t            env;       /* must be first: env->user points back */
    struct aglx_context  *ctx;
    gl_program_t         *prog;

    /* Vertex stage inputs. */
    const float          *attribs;   /* GL_MAX_VERTEX_ATTRIBS_IMPL * 4       */

    /* Fragment stage inputs: the interpolated varyings for this pixel. */
    const float          *varyings;

    /* Outputs. */
    float                 position[4];
    float                 point_size;
    float                 frag_color[4];
    float                 frag_coord[4];
    int                   front_facing;

    /* Where a vertex shader's varying writes go. */
    float                *out_varyings;

    int                   is_vertex;
} shader_env_t;

static int env_read(glsl_env_t *e, const char *name, glsl_value_t *out) {
    shader_env_t *se = (shader_env_t *)e->user;
    gl_program_t *p = se->prog;

    /* Built-ins first: they are the most frequently read names, and none of
     * them can be shadowed by a user declaration (the compiler rejects a
     * gl_ prefix). */
    if (name[0] == 'g' && name[1] == 'l' && name[2] == '_') {
        if (strcmp(name, "gl_Position") == 0) {
            for (int i = 0; i < 4; i++) out->v[i] = se->position[i];
            return 1;
        }
        if (strcmp(name, "gl_PointSize") == 0) {
            out->v[0] = se->point_size;
            return 1;
        }
        if (strcmp(name, "gl_FragColor") == 0) {
            for (int i = 0; i < 4; i++) out->v[i] = se->frag_color[i];
            return 1;
        }
        if (strcmp(name, "gl_FragCoord") == 0) {
            for (int i = 0; i < 4; i++) out->v[i] = se->frag_coord[i];
            return 1;
        }
        if (strcmp(name, "gl_FrontFacing") == 0) {
            out->v[0] = se->front_facing ? 1.0f : 0.0f;
            return 1;
        }
        return 0;
    }

    /* A uniform: both stages share the program's store. */
    const gl_uniform_t *u = gl_program_find_uniform(p, name);
    if (u) {
        for (int i = 0; i < u->size && i < 16; i++) {
            out->v[i] = p->uniform_data[u->offset + i];
        }
        return 1;
    }

    /* A varying: written by the vertex stage, read by the fragment stage. */
    const gl_varying_t *v = gl_program_find_varying(p, name);
    if (v) {
        const float *src = se->is_vertex ? se->out_varyings : se->varyings;
        if (src) {
            for (int i = 0; i < v->size && i < 16; i++) {
                out->v[i] = src[v->offset + i];
            }
        }
        return 1;
    }

    /* An attribute, vertex stage only. */
    if (se->is_vertex && se->attribs) {
        const gl_attrib_info_t *a = gl_program_find_attrib(p, name);
        if (a && a->location >= 0 &&
            a->location < GL_MAX_VERTEX_ATTRIBS_IMPL) {
            const float *src = se->attribs + a->location * 4;
            for (int i = 0; i < a->size && i < 4; i++) out->v[i] = src[i];
            return 1;
        }
    }

    return 0;                        /* unknown: the caller's zeros stand */
}

static void env_write(glsl_env_t *e, const char *name,
                      const glsl_value_t *val) {
    shader_env_t *se = (shader_env_t *)e->user;

    if (name[0] == 'g' && name[1] == 'l' && name[2] == '_') {
        if (strcmp(name, "gl_Position") == 0) {
            for (int i = 0; i < 4; i++) se->position[i] = val->v[i];
            return;
        }
        if (strcmp(name, "gl_PointSize") == 0) {
            se->point_size = val->v[0];
            return;
        }
        if (strcmp(name, "gl_FragColor") == 0) {
            for (int i = 0; i < 4; i++) se->frag_color[i] = val->v[i];
            return;
        }
        return;                      /* the rest are read-only */
    }

    const gl_varying_t *v = gl_program_find_varying(se->prog, name);
    if (v && se->out_varyings) {
        for (int i = 0; i < v->size && i < 16; i++) {
            se->out_varyings[v->offset + i] = val->v[i];
        }
    }
}

/* texture2D and friends land here.  The unit number is the sampler uniform's
 * value, which glUniform1i set, so this reaches the same texture the
 * fixed-function path would. */
static void env_sample(glsl_env_t *e, int unit, int is_cube,
                       const float *coord, int ncoord, float *rgba) {
    shader_env_t *se = (shader_env_t *)e->user;
    struct aglx_context *ctx = se->ctx;

    rgba[0] = rgba[1] = rgba[2] = 0.0f;
    rgba[3] = 1.0f;

    if (unit < 0 || unit >= GL_MAX_TEXTURE_UNITS_IMPL) return;

    /* A shader samples whatever is BOUND to the unit, regardless of whether
     * GL_TEXTURE_2D is enabled: the enables are a fixed-function concept and
     * have no meaning once a program is in use (§3.8.15 of ES 2.0). */
    gl_texunit_t *tu = &ctx->texunits[unit];
    GLuint name = is_cube ? tu->binding_cube : tu->binding_2d;
    if (name == 0) return;

    gl_texture_t *t = NULL;
    for (int i = 0; i < GL_MAX_TEXTURES_IMPL; i++) {
        if (ctx->textures[i].used && ctx->textures[i].name == name) {
            t = &ctx->textures[i];
            break;
        }
    }
    if (!t || !t->img[0][0].texels) return;

    GLfloat r = ncoord > 2 ? coord[2] : 0.0f;
    /* Magnification: a shader has no per-triangle LOD plumbed through yet,
     * and picking level 0 is both correct for magnification and the
     * conservative choice for minification -- sharper than it should be,
     * rather than blurrier. */
    gl_color_t c = gl_texture_sample_lod(t, coord[0], ncoord > 1 ? coord[1] : 0.0f,
                                         r, 0.0f);
    rgba[0] = c.r; rgba[1] = c.g; rgba[2] = c.b; rgba[3] = c.a;
}

static void env_init(shader_env_t *se, struct aglx_context *ctx,
                     gl_program_t *p, int is_vertex) {
    memset(se, 0, sizeof *se);
    se->env.read_var  = env_read;
    se->env.write_var = env_write;
    se->env.sample    = env_sample;
    se->env.user      = se;
    se->ctx = ctx;
    se->prog = p;
    se->is_vertex = is_vertex;
    /* GL's defaults for the outputs, so a shader that writes only some of
     * them produces something defined. */
    se->position[3]  = 1.0f;
    se->point_size   = 1.0f;
    se->frag_color[3] = 1.0f;
    se->front_facing = 1;
}

/* ============================================================================
 * Reading a generic vertex attribute
 * ==========================================================================*/

/* Buffer lookup, mirroring glarray.c's. */
static gl_buffer_t *find_buffer(struct aglx_context *ctx, GLuint name) {
    if (name == 0) return (gl_buffer_t *)0;
    for (int i = 0; i < GL_MAX_BUFFERS_IMPL; i++) {
        if (ctx->buffers[i].used && ctx->buffers[i].name == name) {
            return &ctx->buffers[i];
        }
    }
    return (gl_buffer_t *)0;
}

static int type_bytes(GLenum type) {
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE:   return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_INT: case GL_UNSIGNED_INT:
    case GL_FLOAT:                         return 4;
    default:                               return 0;
    }
}

/* Read element `index` of attribute `slot` into four floats.
 *
 * A disabled array supplies the generic value set by glVertexAttrib4f, which
 * defaults to (0,0,0,1) -- so a shader reading an attribute the application
 * never supplied gets a valid homogeneous point rather than a degenerate one.
 */
static void read_attrib(struct aglx_context *ctx, int slot, int index,
                        float *out) {
    const gl_vertex_attrib_t *a = &ctx->vattrib[slot];

    out[0] = a->generic[0]; out[1] = a->generic[1];
    out[2] = a->generic[2]; out[3] = a->generic[3];

    if (!a->enabled) return;

    const unsigned char *base;
    if (a->buffer != 0) {
        gl_buffer_t *b = find_buffer(ctx, a->buffer);
        if (!b || !b->data) return;
        GLsizeiptr off = (GLsizeiptr)(uintptr_t)a->ptr;
        if (off < 0 || off > b->size) return;
        base = (const unsigned char *)b->data + off;
    } else {
        if (!a->ptr) return;
        base = (const unsigned char *)a->ptr;
    }

    int esz = type_bytes(a->type);
    if (esz == 0) return;
    GLsizei stride = a->stride ? a->stride : (GLsizei)(esz * a->size);
    const unsigned char *e = base + (size_t)index * (size_t)stride;

    /* Components the array does not supply keep their default: a vec4
     * attribute fed from a 3-component array reads w as 1.0, which is what
     * makes `attribute vec4 aPos` work against xyz data. */
    for (int i = 0; i < a->size && i < 4; i++) {
        const unsigned char *q = e + (size_t)i * (size_t)esz;
        float f = 0.0f;
        switch (a->type) {
        case GL_FLOAT: { float v; memcpy(&v, q, sizeof v); f = v; break; }
        case GL_BYTE: {
            signed char v; memcpy(&v, q, sizeof v);
            f = a->normalized ? (float)v / 127.0f : (float)v;
            break;
        }
        case GL_UNSIGNED_BYTE: {
            unsigned char v = *q;
            f = a->normalized ? (float)v / 255.0f : (float)v;
            break;
        }
        case GL_SHORT: {
            short v; memcpy(&v, q, sizeof v);
            f = a->normalized ? (float)v / 32767.0f : (float)v;
            break;
        }
        case GL_UNSIGNED_SHORT: {
            unsigned short v; memcpy(&v, q, sizeof v);
            f = a->normalized ? (float)v / 65535.0f : (float)v;
            break;
        }
        case GL_INT: { int v; memcpy(&v, q, sizeof v); f = (float)v; break; }
        case GL_UNSIGNED_INT: {
            unsigned int v; memcpy(&v, q, sizeof v); f = (float)v; break;
        }
        default: break;
        }
        out[i] = f;
    }
}

/* ============================================================================
 * The vertex stage
 * ==========================================================================*/

/* Run the vertex shader for element `index`, filling `out` with clip
 * coordinates and varyings.  Returns 0 when the shader could not run, which
 * the caller must treat as "drop this vertex" rather than drawing garbage. */
int gl_shader_run_vertex(struct aglx_context *ctx, int index,
                         gl_vertex_t *out) {
    gl_program_t *p = gl_program_current(ctx);
    if (!p) return 0;

    glsl_unit_t *vu = (glsl_unit_t *)gl_program_vertex_unit(ctx, p);
    if (!vu) return 0;

    float attribs[GL_MAX_VERTEX_ATTRIBS_IMPL * 4];
    for (int i = 0; i < GL_MAX_VERTEX_ATTRIBS_IMPL; i++) {
        read_attrib(ctx, i, index, attribs + i * 4);
    }

    memset(out, 0, sizeof *out);

    shader_env_t se;
    env_init(&se, ctx, p, 1);
    se.attribs = attribs;
    se.out_varyings = out->varying;

    if (glsl_run(vu, &se.env) == GLSL_RUN_ERROR) return 0;

    out->clip = glm_vec4_make(se.position[0], se.position[1],
                              se.position[2], se.position[3]);
    /* Eye position is what the fixed-function lighting and fog use.  A shader
     * pipeline has neither, but the clipper and the rasterizer read the field,
     * so it is filled with something defined rather than left as heap noise. */
    out->eye = glm_vec3_make(se.position[0], se.position[1], se.position[2]);
    out->varying_count = p->varying_floats;
    out->valid = 1;
    out->color.r = out->color.g = out->color.b = out->color.a = 1.0f;
    return 1;
}

/* ============================================================================
 * The fragment stage
 * ==========================================================================*/

/* Run the fragment shader for one pixel.
 *
 * `varyings` are already interpolated and perspective-corrected by the
 * rasterizer.  Returns 1 to keep the fragment, 0 to discard it.
 */
int gl_shader_run_fragment(struct aglx_context *ctx, const float *varyings,
                           float x, float y, float z, int front_facing,
                           gl_color_t *out) {
    gl_program_t *p = gl_program_current(ctx);
    if (!p) return 0;

    glsl_unit_t *fu = (glsl_unit_t *)gl_program_fragment_unit(ctx, p);
    if (!fu) return 0;

    shader_env_t se;
    env_init(&se, ctx, p, 0);
    se.varyings = varyings;
    se.frag_coord[0] = x;
    se.frag_coord[1] = y;
    se.frag_coord[2] = z;
    /* gl_FragCoord.w is 1/w, not w (§3.8.1 of ES 2.0) -- a detail that only
     * matters to shaders doing their own perspective work, and exactly the
     * kind of thing that is wrong everywhere if it is wrong once. */
    se.frag_coord[3] = 1.0f;
    se.front_facing = front_facing;

    glsl_run_status_t st = glsl_run(fu, &se.env);
    if (st == GLSL_RUN_DISCARD) return 0;
    if (st == GLSL_RUN_ERROR) {
        /* A shader that hit a runtime limit produces magenta rather than
         * whatever was in the buffer: an obviously wrong colour is far easier
         * to notice than a stale one, and the info log says what happened. */
        out->r = 1.0f; out->g = 0.0f; out->b = 1.0f; out->a = 1.0f;
        return 1;
    }

    out->r = se.frag_color[0];
    out->g = se.frag_color[1];
    out->b = se.frag_color[2];
    out->a = se.frag_color[3];
    return 1;
}

/* Is a shader program driving this draw? */
int gl_shader_active(struct aglx_context *ctx) {
    return gl_program_current(ctx) != (gl_program_t *)0;
}

/* How many varying floats the bound program carries, or 0. */
int gl_shader_varying_floats(struct aglx_context *ctx) {
    gl_program_t *p = gl_program_current(ctx);
    return p ? p->varying_floats : 0;
}

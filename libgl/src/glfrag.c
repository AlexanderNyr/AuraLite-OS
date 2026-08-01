/* libgl/src/glfrag.c — per-fragment operations: alpha test, blending, fog.
 *
 * Phase G6 of GL_PLAN.md.
 *
 * These are the stages that sit between "the rasterizer has produced a colour"
 * and "the colour is written to the framebuffer" (GL 1.1 §4.1), in this order:
 *
 *     fog  ->  alpha test  ->  depth test  ->  blending  ->  write
 *
 * Fog is applied before the alpha test because it only changes RGB, never
 * alpha; the depth test lives in the rasterizer since it also gates the depth
 * write.
 */

#include <math.h>

#include "GL/gl.h"
#include "glcontext.h"
#include "glvertex.h"

void gl_frag_set_defaults(struct aglx_context *ctx) {
    ctx->blend      = GL_FALSE;
    ctx->blend_src  = GL_ONE;
    ctx->blend_dst  = GL_ZERO;

    ctx->alpha_test = GL_FALSE;
    ctx->alpha_func = GL_ALWAYS;
    ctx->alpha_ref  = 0.0f;

    ctx->fog         = GL_FALSE;
    ctx->fog_mode    = GL_EXP;
    ctx->fog_density = 1.0f;
    ctx->fog_start   = 0.0f;
    ctx->fog_end     = 1.0f;
    ctx->fog_color.r = ctx->fog_color.g = ctx->fog_color.b = 0.0f;
    ctx->fog_color.a = 0.0f;
}

/* ============================================================================
 * Alpha test (§4.1.4)
 * ==========================================================================*/

int gl_alpha_test_passes(const struct aglx_context *ctx, GLfloat alpha) {
    if (!ctx->alpha_test) return 1;
    GLfloat ref = ctx->alpha_ref;
    switch (ctx->alpha_func) {
    case GL_NEVER:    return 0;
    case GL_LESS:     return alpha <  ref;
    case GL_EQUAL:    return alpha == ref;
    case GL_LEQUAL:   return alpha <= ref;
    case GL_GREATER:  return alpha >  ref;
    case GL_NOTEQUAL: return alpha != ref;
    case GL_GEQUAL:   return alpha >= ref;
    case GL_ALWAYS:   return 1;
    default:          return 1;
    }
}

void glAlphaFunc(GLenum func, GLclampf ref) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    switch (func) {
    case GL_NEVER: case GL_LESS: case GL_EQUAL: case GL_LEQUAL:
    case GL_GREATER: case GL_NOTEQUAL: case GL_GEQUAL: case GL_ALWAYS:
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    ctx->alpha_func = func;
    ctx->alpha_ref  = gl_clampf(ref);
}

/* ============================================================================
 * Blending (§4.1.7)
 * ==========================================================================*/

/* Resolve a blend factor to its per-channel multipliers.
 *
 * Both the source and destination factors use the same table; `is_src` only
 * matters for GL_SRC_ALPHA_SATURATE, which is defined for the source only. */
static void blend_factor(GLenum factor, gl_color_t src, gl_color_t dst,
                         int is_src, GLfloat out[4]) {
    GLfloat r = 0, g = 0, b = 0, a = 0;
    switch (factor) {
    case GL_ZERO:                                       break;
    case GL_ONE:                r = g = b = a = 1.0f;   break;
    case GL_SRC_COLOR:          r = src.r; g = src.g; b = src.b; a = src.a; break;
    case GL_ONE_MINUS_SRC_COLOR:
        r = 1 - src.r; g = 1 - src.g; b = 1 - src.b; a = 1 - src.a; break;
    case GL_DST_COLOR:          r = dst.r; g = dst.g; b = dst.b; a = dst.a; break;
    case GL_ONE_MINUS_DST_COLOR:
        r = 1 - dst.r; g = 1 - dst.g; b = 1 - dst.b; a = 1 - dst.a; break;
    case GL_SRC_ALPHA:          r = g = b = a = src.a;         break;
    case GL_ONE_MINUS_SRC_ALPHA:r = g = b = a = 1.0f - src.a;  break;
    case GL_DST_ALPHA:          r = g = b = a = dst.a;         break;
    case GL_ONE_MINUS_DST_ALPHA:r = g = b = a = 1.0f - dst.a;  break;
    case GL_SRC_ALPHA_SATURATE: {
        /* min(As, 1-Ad) on RGB, exactly 1 on alpha (§4.1.7). */
        GLfloat f = src.a;
        GLfloat lim = 1.0f - dst.a;
        if (lim < f) f = lim;
        if (f < 0.0f) f = 0.0f;
        r = g = b = is_src ? f : 0.0f;
        a = 1.0f;
        break;
    }
    default:
        r = g = b = a = 1.0f;
        break;
    }
    out[0] = r; out[1] = g; out[2] = b; out[3] = a;
}

/* Blend `src` over `dst` and return the result.  Both are non-premultiplied
 * RGBA in [0,1]; the result is clamped, as the specification requires. */
gl_color_t gl_blend(const struct aglx_context *ctx,
                    gl_color_t src, gl_color_t dst) {
    GLfloat sf[4], df[4];
    blend_factor(ctx->blend_src, src, dst, 1, sf);
    blend_factor(ctx->blend_dst, src, dst, 0, df);

    gl_color_t out;
    out.r = gl_clampf(src.r * sf[0] + dst.r * df[0]);
    out.g = gl_clampf(src.g * sf[1] + dst.g * df[1]);
    out.b = gl_clampf(src.b * sf[2] + dst.b * df[2]);
    out.a = gl_clampf(src.a * sf[3] + dst.a * df[3]);
    return out;
}

void glBlendFunc(GLenum sfactor, GLenum dfactor) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    /* GL_SRC_ALPHA_SATURATE is only legal as a source factor (§4.1.7). */
    switch (sfactor) {
    case GL_ZERO: case GL_ONE:
    case GL_DST_COLOR: case GL_ONE_MINUS_DST_COLOR:
    case GL_SRC_ALPHA: case GL_ONE_MINUS_SRC_ALPHA:
    case GL_DST_ALPHA: case GL_ONE_MINUS_DST_ALPHA:
    case GL_SRC_ALPHA_SATURATE:
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    switch (dfactor) {
    case GL_ZERO: case GL_ONE:
    case GL_SRC_COLOR: case GL_ONE_MINUS_SRC_COLOR:
    case GL_SRC_ALPHA: case GL_ONE_MINUS_SRC_ALPHA:
    case GL_DST_ALPHA: case GL_ONE_MINUS_DST_ALPHA:
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    ctx->blend_src = sfactor;
    ctx->blend_dst = dfactor;
}

/* ============================================================================
 * Fog (§3.10)
 * ==========================================================================*/

/* The fog factor f in [0,1]: 1 means no fog, 0 means fully fogged.
 * `z` is the eye-space distance from the viewer, taken as |eye.z|. */
GLfloat gl_fog_factor(const struct aglx_context *ctx, GLfloat z) {
    GLfloat f;
    switch (ctx->fog_mode) {
    case GL_LINEAR: {
        GLfloat denom = ctx->fog_end - ctx->fog_start;
        if (denom == 0.0f) return 1.0f;
        f = (ctx->fog_end - z) / denom;
        break;
    }
    case GL_EXP:
        f = expf(-ctx->fog_density * z);
        break;
    case GL_EXP2: {
        GLfloat dz = ctx->fog_density * z;
        f = expf(-(dz * dz));
        break;
    }
    default:
        return 1.0f;
    }
    return gl_clampf(f);
}

/* Mix the fragment colour towards the fog colour.  Alpha is untouched
 * (§3.10): fog changes what you see, not how transparent it is. */
gl_color_t gl_fog_apply(const struct aglx_context *ctx,
                        gl_color_t c, GLfloat z) {
    GLfloat f = gl_fog_factor(ctx, z);
    gl_color_t out;
    out.r = f * c.r + (1.0f - f) * ctx->fog_color.r;
    out.g = f * c.g + (1.0f - f) * ctx->fog_color.g;
    out.b = f * c.b + (1.0f - f) * ctx->fog_color.b;
    out.a = c.a;
    return out;
}

void glFogi(GLenum pname, GLint param) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (pname != GL_FOG_MODE) { gl_set_error(GL_INVALID_ENUM); return; }
    if (param != GL_LINEAR && param != GL_EXP && param != GL_EXP2) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    ctx->fog_mode = (GLenum)param;
}

void glFogf(GLenum pname, GLfloat param) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    switch (pname) {
    case GL_FOG_MODE:
        glFogi(pname, (GLint)param);
        break;
    case GL_FOG_DENSITY:
        if (param < 0.0f) { gl_set_error(GL_INVALID_VALUE); return; }
        ctx->fog_density = param;
        break;
    case GL_FOG_START:
        ctx->fog_start = param;
        break;
    case GL_FOG_END:
        ctx->fog_end = param;
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
}

void glFogfv(GLenum pname, const GLfloat *params) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!params) { gl_set_error(GL_INVALID_VALUE); return; }

    if (pname == GL_FOG_COLOR) {
        ctx->fog_color.r = params[0];
        ctx->fog_color.g = params[1];
        ctx->fog_color.b = params[2];
        ctx->fog_color.a = params[3];
        return;
    }
    glFogf(pname, params[0]);
}

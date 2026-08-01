/* libgl/src/gltexture.c — texture objects and sampling (GL 1.1 §3.8).
 *
 * Phase G6 of GL_PLAN.md.
 *
 * STORAGE DECISION
 *
 * Texels are unpacked to 32-bit 0xAARRGGBB at glTexImage2D time, regardless of
 * the format the application supplied.  A software rasterizer samples a
 * texture millions of times per frame and uploads it once, so paying the
 * conversion once and keeping the inner loop branch-free is clearly the right
 * trade.  The cost is memory for GL_LUMINANCE images, which is acceptable at
 * the sizes this OS deals with.
 *
 * TEXTURE COORDINATE ORIGIN
 *
 * GL puts texture coordinate (0,0) at the BOTTOM-left of the image and
 * glTexImage2D receives rows starting from that same bottom row.  This module
 * stores rows exactly as supplied, so row 0 of `texels` is the bottom row, and
 * the v coordinate maps to it directly with no flip.  Getting this wrong shows
 * up as vertically mirrored textures.
 */

#include <stdlib.h>
#include <string.h>

#include "GL/gl.h"
#include "glcontext.h"
#include "glvertex.h"

/* ============================================================================
 * Object management
 * ==========================================================================*/

void gl_texture_set_defaults(struct aglx_context *ctx) {
    ctx->texture_2d        = GL_FALSE;
    ctx->texture_binding   = 0;
    ctx->next_texture_name = 1;        /* 0 is reserved for "no texture" */
    ctx->tex_env_mode      = GL_MODULATE;
    ctx->tex_env_color.r   = 0.0f;
    ctx->tex_env_color.g   = 0.0f;
    ctx->tex_env_color.b   = 0.0f;
    ctx->tex_env_color.a   = 0.0f;

    for (int i = 0; i < GL_MAX_TEXTURES_IMPL; i++) {
        ctx->textures[i].name   = 0;
        ctx->textures[i].used   = 0;
        ctx->textures[i].texels = (uint32_t *)0;
        ctx->textures[i].width  = 0;
        ctx->textures[i].height = 0;
        /* GL defaults: mipmapped minification and repeat wrapping (§3.8.4).
         * Mipmaps do not exist yet, so GL_NEAREST_MIPMAP_LINEAR degrades to
         * GL_LINEAR at sample time. */
        ctx->textures[i].min_filter = GL_LINEAR;
        ctx->textures[i].mag_filter = GL_LINEAR;
        ctx->textures[i].wrap_s     = GL_REPEAT;
        ctx->textures[i].wrap_t     = GL_REPEAT;
    }
}

void gl_texture_free_all(struct aglx_context *ctx) {
    for (int i = 0; i < GL_MAX_TEXTURES_IMPL; i++) {
        free(ctx->textures[i].texels);
        ctx->textures[i].texels = (uint32_t *)0;
        ctx->textures[i].used   = 0;
    }
}

static gl_texture_t *find_texture(struct aglx_context *ctx, GLuint name) {
    if (name == 0) return (gl_texture_t *)0;
    for (int i = 0; i < GL_MAX_TEXTURES_IMPL; i++) {
        if (ctx->textures[i].used && ctx->textures[i].name == name) {
            return &ctx->textures[i];
        }
    }
    return (gl_texture_t *)0;
}

/* The texture the rasterizer should sample, or NULL when texturing is off or
 * nothing usable is bound. */
gl_texture_t *gl_texture_current(struct aglx_context *ctx) {
    if (!ctx->texture_2d) return (gl_texture_t *)0;
    gl_texture_t *t = find_texture(ctx, ctx->texture_binding);
    if (!t || !t->texels) return (gl_texture_t *)0;
    return t;
}

void glGenTextures(GLsizei n, GLuint *textures) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!textures || n == 0) return;

    GLsizei made = 0;
    for (int i = 0; i < GL_MAX_TEXTURES_IMPL && made < n; i++) {
        if (ctx->textures[i].used) continue;
        gl_texture_t *t = &ctx->textures[i];
        t->used   = 1;
        t->name   = ctx->next_texture_name++;
        t->texels = (uint32_t *)0;
        t->width  = 0;
        t->height = 0;
        t->min_filter = GL_LINEAR;
        t->mag_filter = GL_LINEAR;
        t->wrap_s     = GL_REPEAT;
        t->wrap_t     = GL_REPEAT;
        textures[made++] = t->name;
    }
    /* Running out of slots is an implementation limit, not a GL error code in
     * the specification; GL_OUT_OF_MEMORY is the closest honest answer. */
    if (made < n) {
        for (GLsizei k = made; k < n; k++) textures[k] = 0;
        gl_set_error(GL_OUT_OF_MEMORY);
    }
}

void glDeleteTextures(GLsizei n, const GLuint *textures) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!textures) return;

    for (GLsizei k = 0; k < n; k++) {
        gl_texture_t *t = find_texture(ctx, textures[k]);
        if (!t) continue;              /* silently ignored, per §3.8.12 */
        free(t->texels);
        t->texels = (uint32_t *)0;
        t->used   = 0;
        t->name   = 0;
        /* Deleting the bound texture reverts the binding to 0. */
        if (ctx->texture_binding == textures[k]) ctx->texture_binding = 0;
    }
}

GLboolean glIsTexture(GLuint texture) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return GL_FALSE;
    return find_texture(ctx, texture) ? GL_TRUE : GL_FALSE;
}

void glBindTexture(GLenum target, GLuint texture) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D) { gl_set_error(GL_INVALID_ENUM); return; }

    if (texture == 0) { ctx->texture_binding = 0; return; }

    if (!find_texture(ctx, texture)) {
        /* GL allows binding a name that was never generated: it springs into
         * existence as an empty texture object (§3.8.12).  Honour that rather
         * than erroring, since applications do rely on it. */
        for (int i = 0; i < GL_MAX_TEXTURES_IMPL; i++) {
            if (ctx->textures[i].used) continue;
            gl_texture_t *t = &ctx->textures[i];
            t->used = 1;
            t->name = texture;
            t->texels = (uint32_t *)0;
            t->width = t->height = 0;
            t->min_filter = t->mag_filter = GL_LINEAR;
            t->wrap_s = t->wrap_t = GL_REPEAT;
            if (texture >= ctx->next_texture_name) {
                ctx->next_texture_name = texture + 1;
            }
            ctx->texture_binding = texture;
            return;
        }
        gl_set_error(GL_OUT_OF_MEMORY);
        return;
    }
    ctx->texture_binding = texture;
}

/* ============================================================================
 * Image upload
 * ==========================================================================*/

/* Bytes per pixel for the supported source formats. */
static int format_components(GLenum format) {
    switch (format) {
    case GL_RGB:             return 3;
    case GL_RGBA:            return 4;
    case GL_LUMINANCE:       return 1;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_ALPHA:           return 1;
    default:                 return 0;
    }
}

/* Convert one source pixel to 0xAARRGGBB. */
static uint32_t unpack_pixel(const unsigned char *p, GLenum format) {
    switch (format) {
    case GL_RGB:
        return 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    case GL_RGBA:
        return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16)
             | ((uint32_t)p[1] << 8)  | p[2];
    case GL_LUMINANCE: {
        uint32_t l = p[0];
        return 0xFF000000u | (l << 16) | (l << 8) | l;
    }
    case GL_LUMINANCE_ALPHA: {
        uint32_t l = p[0];
        return ((uint32_t)p[1] << 24) | (l << 16) | (l << 8) | l;
    }
    case GL_ALPHA:
        return ((uint32_t)p[0] << 24) | 0x00FFFFFFu;
    default:
        return 0xFF000000u;
    }
}

void glTexImage2D(GLenum target, GLint level, GLint internalFormat,
                  GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    (void)internalFormat;   /* the stored format is always RGBA8 here */

    if (target != GL_TEXTURE_2D) { gl_set_error(GL_INVALID_ENUM); return; }
    if (level != 0) {
        /* Mipmap levels are accepted syntactically but not stored: there is no
         * mipmap chain yet, and silently treating level 3 as the base image
         * would be worse than ignoring it. */
        if (level < 0) gl_set_error(GL_INVALID_VALUE);
        return;
    }
    if (border != 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (width < 0 || height < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (type != GL_UNSIGNED_BYTE) { gl_set_error(GL_INVALID_ENUM); return; }

    int comps = format_components(format);
    if (comps == 0) { gl_set_error(GL_INVALID_ENUM); return; }

    gl_texture_t *t = find_texture(ctx, ctx->texture_binding);
    if (!t) { gl_set_error(GL_INVALID_OPERATION); return; }

    free(t->texels);
    t->texels = (uint32_t *)0;
    t->width  = width;
    t->height = height;

    if (width == 0 || height == 0) return;   /* legal: empties the texture */

    size_t count = (size_t)width * (size_t)height;
    /* Guard the multiplication: 4096x4096 is 64 MiB, already generous. */
    if (width > 8192 || height > 8192) { gl_set_error(GL_INVALID_VALUE); return; }

    t->texels = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!t->texels) { gl_set_error(GL_OUT_OF_MEMORY); return; }

    if (!pixels) {
        /* A NULL pointer allocates storage with undefined contents (§3.8.1).
         * Zeroing is friendlier than leaving heap garbage on screen. */
        memset(t->texels, 0, count * sizeof(uint32_t));
        return;
    }

    const unsigned char *src = (const unsigned char *)pixels;
    for (size_t i = 0; i < count; i++) {
        t->texels[i] = unpack_pixel(src + i * (size_t)comps, format);
    }
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format,
                     GLenum type, const GLvoid *pixels) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (target != GL_TEXTURE_2D) { gl_set_error(GL_INVALID_ENUM); return; }
    if (level != 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (type != GL_UNSIGNED_BYTE) { gl_set_error(GL_INVALID_ENUM); return; }
    if (width < 0 || height < 0 || xoffset < 0 || yoffset < 0) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    if (!pixels) { gl_set_error(GL_INVALID_VALUE); return; }

    int comps = format_components(format);
    if (comps == 0) { gl_set_error(GL_INVALID_ENUM); return; }

    gl_texture_t *t = find_texture(ctx, ctx->texture_binding);
    if (!t || !t->texels) { gl_set_error(GL_INVALID_OPERATION); return; }

    if (xoffset + width > t->width || yoffset + height > t->height) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }

    const unsigned char *src = (const unsigned char *)pixels;
    for (GLsizei row = 0; row < height; row++) {
        uint32_t *dst = t->texels + (size_t)(yoffset + row) * t->width + xoffset;
        for (GLsizei col = 0; col < width; col++) {
            dst[col] = unpack_pixel(src + ((size_t)row * width + col) * comps,
                                    format);
        }
    }
}

void glTexParameteri(GLenum target, GLenum pname, GLint param) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (target != GL_TEXTURE_2D) { gl_set_error(GL_INVALID_ENUM); return; }

    gl_texture_t *t = find_texture(ctx, ctx->texture_binding);
    if (!t) { gl_set_error(GL_INVALID_OPERATION); return; }

    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
    case GL_TEXTURE_MAG_FILTER:
        if (param != GL_NEAREST && param != GL_LINEAR) {
            gl_set_error(GL_INVALID_ENUM);
            return;
        }
        if (pname == GL_TEXTURE_MIN_FILTER) t->min_filter = (GLenum)param;
        else                                t->mag_filter = (GLenum)param;
        break;
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
        if (param != GL_REPEAT && param != GL_CLAMP &&
            param != GL_CLAMP_TO_EDGE) {
            gl_set_error(GL_INVALID_ENUM);
            return;
        }
        if (pname == GL_TEXTURE_WRAP_S) t->wrap_s = (GLenum)param;
        else                            t->wrap_t = (GLenum)param;
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
}

void glTexEnvi(GLenum target, GLenum pname, GLint param) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (target != GL_TEXTURE_ENV)      { gl_set_error(GL_INVALID_ENUM); return; }
    if (pname  != GL_TEXTURE_ENV_MODE) { gl_set_error(GL_INVALID_ENUM); return; }

    switch (param) {
    case GL_MODULATE: case GL_REPLACE: case GL_DECAL: case GL_BLEND:
        ctx->tex_env_mode = (GLenum)param;
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
}

void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!params) { gl_set_error(GL_INVALID_VALUE); return; }
    if (target != GL_TEXTURE_ENV) { gl_set_error(GL_INVALID_ENUM); return; }

    if (pname == GL_TEXTURE_ENV_COLOR) {
        ctx->tex_env_color.r = params[0];
        ctx->tex_env_color.g = params[1];
        ctx->tex_env_color.b = params[2];
        ctx->tex_env_color.a = params[3];
    } else if (pname == GL_TEXTURE_ENV_MODE) {
        glTexEnvi(target, pname, (GLint)params[0]);
    } else {
        gl_set_error(GL_INVALID_ENUM);
    }
}

/* ============================================================================
 * Sampling
 * ==========================================================================*/

/* Apply a wrap mode to an integer texel index. */
static int wrap_coord(int i, int size, GLenum mode) {
    if (size <= 0) return 0;
    if (mode == GL_REPEAT) {
        i %= size;
        if (i < 0) i += size;          /* C's % keeps the sign of the operand */
        return i;
    }
    /* GL_CLAMP and GL_CLAMP_TO_EDGE both clamp to the edge texel here.  True
     * GL_CLAMP blends with a border colour, which this implementation does not
     * store; clamping to the edge is the closer of the two behaviours. */
    if (i < 0) return 0;
    if (i >= size) return size - 1;
    return i;
}

static uint32_t texel_at(const gl_texture_t *t, int x, int y) {
    x = wrap_coord(x, t->width,  t->wrap_s);
    y = wrap_coord(y, t->height, t->wrap_t);
    return t->texels[(size_t)y * t->width + x];
}

/* Sample the texture at (s,t), returning a floating-point RGBA colour.
 *
 * `magnifying` selects the mag or min filter.  A real implementation picks
 * that from the texture-space derivatives; with no mipmaps and no derivative
 * tracking, this implementation uses the magnification filter throughout,
 * which is what the caller passes.
 */
gl_color_t gl_texture_sample(const gl_texture_t *t, GLfloat s, GLfloat tc,
                             int magnifying) {
    gl_color_t out;
    out.r = out.g = out.b = out.a = 1.0f;
    if (!t || !t->texels || t->width <= 0 || t->height <= 0) return out;

    GLenum filter = magnifying ? t->mag_filter : t->min_filter;

    /* Texture space: (0,0) is the corner of texel (0,0), and texel centres sit
     * at (i+0.5)/size.  Hence the -0.5 when locating the sample. */
    GLfloat fx = s * (GLfloat)t->width  - 0.5f;
    GLfloat fy = tc * (GLfloat)t->height - 0.5f;

    if (filter == GL_NEAREST) {
        /* Round to the nearest texel centre.  floorf(x + 0.5) rather than a
         * cast, because casting truncates towards zero and would misbehave for
         * negative coordinates produced by GL_REPEAT. */
        int xi = (int)(fx + 0.5f);
        int yi = (int)(fy + 0.5f);
        if (fx + 0.5f < 0.0f) xi--;
        if (fy + 0.5f < 0.0f) yi--;
        uint32_t p = texel_at(t, xi, yi);
        out.a = (GLfloat)((p >> 24) & 0xFF) / 255.0f;
        out.r = (GLfloat)((p >> 16) & 0xFF) / 255.0f;
        out.g = (GLfloat)((p >>  8) & 0xFF) / 255.0f;
        out.b = (GLfloat)( p        & 0xFF) / 255.0f;
        return out;
    }

    /* Bilinear: blend the four texels surrounding the sample point. */
    int x0 = (int)fx; if (fx < 0.0f && (GLfloat)x0 != fx) x0--;
    int y0 = (int)fy; if (fy < 0.0f && (GLfloat)y0 != fy) y0--;
    GLfloat ax = fx - (GLfloat)x0;
    GLfloat ay = fy - (GLfloat)y0;

    uint32_t p00 = texel_at(t, x0,     y0);
    uint32_t p10 = texel_at(t, x0 + 1, y0);
    uint32_t p01 = texel_at(t, x0,     y0 + 1);
    uint32_t p11 = texel_at(t, x0 + 1, y0 + 1);

    GLfloat w00 = (1.0f - ax) * (1.0f - ay);
    GLfloat w10 = ax * (1.0f - ay);
    GLfloat w01 = (1.0f - ax) * ay;
    GLfloat w11 = ax * ay;

    #define CHAN(p, shift) ((GLfloat)(((p) >> (shift)) & 0xFF) / 255.0f)
    out.a = CHAN(p00,24)*w00 + CHAN(p10,24)*w10 + CHAN(p01,24)*w01 + CHAN(p11,24)*w11;
    out.r = CHAN(p00,16)*w00 + CHAN(p10,16)*w10 + CHAN(p01,16)*w01 + CHAN(p11,16)*w11;
    out.g = CHAN(p00, 8)*w00 + CHAN(p10, 8)*w10 + CHAN(p01, 8)*w01 + CHAN(p11, 8)*w11;
    out.b = CHAN(p00, 0)*w00 + CHAN(p10, 0)*w10 + CHAN(p01, 0)*w01 + CHAN(p11, 0)*w11;
    #undef CHAN
    return out;
}

/* Combine the sampled texel with the interpolated fragment colour according to
 * the texture environment mode (§3.8.9). */
gl_color_t gl_texture_env(const struct aglx_context *ctx,
                          gl_color_t frag, gl_color_t tex) {
    gl_color_t out = frag;
    switch (ctx->tex_env_mode) {
    case GL_REPLACE:
        out = tex;
        break;
    case GL_DECAL:
        /* Texture alpha selects between fragment and texture colour; the
         * fragment's own alpha is preserved. */
        out.r = frag.r * (1.0f - tex.a) + tex.r * tex.a;
        out.g = frag.g * (1.0f - tex.a) + tex.g * tex.a;
        out.b = frag.b * (1.0f - tex.a) + tex.b * tex.a;
        out.a = frag.a;
        break;
    case GL_BLEND:
        /* Interpolate towards the environment colour by the texel value. */
        out.r = frag.r * (1.0f - tex.r) + ctx->tex_env_color.r * tex.r;
        out.g = frag.g * (1.0f - tex.g) + ctx->tex_env_color.g * tex.g;
        out.b = frag.b * (1.0f - tex.b) + ctx->tex_env_color.b * tex.b;
        out.a = frag.a * tex.a;
        break;
    case GL_MODULATE:
    default:
        out.r = frag.r * tex.r;
        out.g = frag.g * tex.g;
        out.b = frag.b * tex.b;
        out.a = frag.a * tex.a;
        break;
    }
    return out;
}

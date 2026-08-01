/* libgl/src/gltexture.c — texture objects and sampling.
 *
 * Phase G6 of GL_PLAN.md (GL 1.1 §3.8), extended to GL 1.3 in phase G10:
 * mipmaps, multitexturing, 3D textures, cube maps and GL_CLAMP_TO_BORDER.
 *
 * STORAGE DECISION
 *
 * Texels are unpacked to 32-bit 0xAARRGGBB at upload time, regardless of the
 * format the application supplied.  A software rasterizer samples a texture
 * millions of times per frame and uploads it once, so paying the conversion
 * once and keeping the inner loop branch-free is clearly the right trade.  The
 * cost is memory for GL_LUMINANCE images, which is acceptable at the sizes
 * this OS deals with.
 *
 * TEXTURE COORDINATE ORIGIN
 *
 * GL puts texture coordinate (0,0) at the BOTTOM-left of the image and
 * glTexImage2D receives rows starting from that same bottom row.  This module
 * stores rows exactly as supplied, so row 0 of `texels` is the bottom row, and
 * the v coordinate maps to it directly with no flip.  Getting this wrong shows
 * up as vertically mirrored textures.
 *
 * ONE OBJECT TYPE FOR THREE TARGETS
 *
 * 2D, 3D and cube-map textures share gl_texture_t.  A cube map fills all six
 * face chains; 2D and 3D use face 0.  Three parallel object types would mean
 * three copies of the name allocator, the parameter setter and the deleter,
 * and a sampler that has to switch on which it was handed.  Five unused
 * pointers per non-cube texture is a much smaller price.
 *
 * MIPMAP LEVEL SELECTION — READ THIS BEFORE "FIXING" ALIASING
 *
 * Hardware picks a level per FRAGMENT from the screen-space derivatives of the
 * texture coordinates.  A scanline rasterizer has no dFdx/dFdy, so this
 * implementation computes ONE level per TRIANGLE, in glraster.c, from the
 * ratio of the primitive's texture-space area to its screen-space area.  That
 * is exact for a triangle at constant depth and progressively wrong for a
 * strongly foreshortened one: a ground plane stretching to the horizon gets a
 * single level where hardware would blend several.  Tessellate large receding
 * surfaces.  This limitation is deliberate and documented in docs/opengl.md.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "GL/gl.h"
#include "glcontext.h"
#include "glvertex.h"

/* ============================================================================
 * Small helpers
 * ==========================================================================*/

/* Index of a cube face target, or -1 when `target` is not a face. */
static int cube_face_index(GLenum target) {
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
        target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) {
        return (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
    }
    return -1;
}

/* The object target an image target belongs to: a cube FACE target uploads
 * into a cube MAP object. */
static GLenum image_target_object(GLenum target) {
    if (cube_face_index(target) >= 0) return GL_TEXTURE_CUBE_MAP;
    return target;
}

static int target_is_valid(GLenum target) {
    return target == GL_TEXTURE_2D || target == GL_TEXTURE_3D ||
           target == GL_TEXTURE_CUBE_MAP;
}

static void image_clear(gl_teximage_t *im) {
    free(im->texels);
    im->texels = (uint32_t *)0;
    im->width = im->height = im->depth = 0;
}

static void texture_reset(gl_texture_t *t, GLuint name, int used) {
    for (int f = 0; f < GL_CUBE_FACES; f++) {
        for (int l = 0; l < GL_MAX_MIPMAP_LEVELS; l++) {
            t->img[f][l].texels = (uint32_t *)0;
            t->img[f][l].width = t->img[f][l].height = t->img[f][l].depth = 0;
        }
    }
    t->name   = name;
    t->used   = used;
    t->target = 0;
    t->levels = 0;
    /* GL defaults: mipmapped minification and repeat wrapping (§3.8.4).  With
     * no chain uploaded the mipmap filters degrade to their non-mipmapped
     * equivalent at sample time, so the default is safe from the start. */
    t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    t->mag_filter = GL_LINEAR;
    t->wrap_s = t->wrap_t = t->wrap_r = GL_REPEAT;
    t->border_color.r = t->border_color.g = 0.0f;
    t->border_color.b = t->border_color.a = 0.0f;
    t->base_level = 0;
    t->max_level  = GL_MAX_MIPMAP_LEVELS - 1;
}

static void texture_free(gl_texture_t *t) {
    for (int f = 0; f < GL_CUBE_FACES; f++) {
        for (int l = 0; l < GL_MAX_MIPMAP_LEVELS; l++) {
            image_clear(&t->img[f][l]);
        }
    }
    t->levels = 0;
}

/* ============================================================================
 * Object management
 * ==========================================================================*/

static void texunit_defaults(gl_texunit_t *u) {
    u->enabled_2d = u->enabled_3d = u->enabled_cube = GL_FALSE;
    u->binding_2d = u->binding_3d = u->binding_cube = 0;
    u->env_mode   = GL_MODULATE;
    u->env_color.r = u->env_color.g = u->env_color.b = u->env_color.a = 0.0f;
}

void gl_texture_set_defaults(struct aglx_context *ctx) {
    ctx->next_texture_name = 1;        /* 0 is reserved for "no texture" */
    ctx->active_texture = 0;
    ctx->client_active_texture = 0;

    for (int u = 0; u < GL_MAX_TEXTURE_UNITS_IMPL; u++) {
        texunit_defaults(&ctx->texunits[u]);
    }
    for (int i = 0; i < GL_MAX_TEXTURES_IMPL; i++) {
        texture_reset(&ctx->textures[i], 0, 0);
    }
}

void gl_texture_free_all(struct aglx_context *ctx) {
    for (int i = 0; i < GL_MAX_TEXTURES_IMPL; i++) {
        texture_free(&ctx->textures[i]);
        ctx->textures[i].used = 0;
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

/* The binding slot on unit `u` for a given object target. */
static GLuint *binding_slot(gl_texunit_t *u, GLenum target) {
    switch (target) {
    case GL_TEXTURE_2D:       return &u->binding_2d;
    case GL_TEXTURE_3D:       return &u->binding_3d;
    case GL_TEXTURE_CUBE_MAP: return &u->binding_cube;
    default:                  return (GLuint *)0;
    }
}

/* The texture object the active unit has bound to `target`, or NULL. */
static gl_texture_t *bound_texture(struct aglx_context *ctx, GLenum target) {
    gl_texunit_t *u = &ctx->texunits[ctx->active_texture];
    GLuint *slot = binding_slot(u, target);
    if (!slot) return (gl_texture_t *)0;
    return find_texture(ctx, *slot);
}

/* Which texture unit `unit` should sample, honouring the target priority in
 * §3.8.15: cube map beats 3D beats 2D. */
gl_texture_t *gl_texture_unit_source(struct aglx_context *ctx, int unit) {
    if (unit < 0 || unit >= GL_MAX_TEXTURE_UNITS_IMPL) return (gl_texture_t *)0;
    gl_texunit_t *u = &ctx->texunits[unit];

    gl_texture_t *t = (gl_texture_t *)0;
    if (u->enabled_cube)     t = find_texture(ctx, u->binding_cube);
    else if (u->enabled_3d)  t = find_texture(ctx, u->binding_3d);
    else if (u->enabled_2d)  t = find_texture(ctx, u->binding_2d);

    if (!t || !t->img[0][0].texels) return (gl_texture_t *)0;
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
        texture_reset(&ctx->textures[i], ctx->next_texture_name++, 1);
        textures[made++] = ctx->textures[i].name;
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
        texture_free(t);
        t->used = 0;
        t->name = 0;
        /* Deleting a texture reverts EVERY unit that had it bound to 0
         * (§3.8.12) — not just the active one, which is the easy mistake. */
        for (int u = 0; u < GL_MAX_TEXTURE_UNITS_IMPL; u++) {
            gl_texunit_t *tu = &ctx->texunits[u];
            if (tu->binding_2d   == textures[k]) tu->binding_2d   = 0;
            if (tu->binding_3d   == textures[k]) tu->binding_3d   = 0;
            if (tu->binding_cube == textures[k]) tu->binding_cube = 0;
        }
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
    if (!target_is_valid(target)) { gl_set_error(GL_INVALID_ENUM); return; }

    gl_texunit_t *u = &ctx->texunits[ctx->active_texture];
    GLuint *slot = binding_slot(u, target);

    if (texture == 0) { *slot = 0; return; }

    gl_texture_t *t = find_texture(ctx, texture);
    if (!t) {
        /* GL allows binding a name that was never generated: it springs into
         * existence as an empty texture object (§3.8.12).  Honour that rather
         * than erroring, since applications do rely on it. */
        for (int i = 0; i < GL_MAX_TEXTURES_IMPL; i++) {
            if (ctx->textures[i].used) continue;
            texture_reset(&ctx->textures[i], texture, 1);
            if (texture >= ctx->next_texture_name) {
                ctx->next_texture_name = texture + 1;
            }
            t = &ctx->textures[i];
            break;
        }
        if (!t) { gl_set_error(GL_OUT_OF_MEMORY); return; }
    }

    /* A texture object's target is fixed by its first binding (§3.8.12).
     * Re-binding it to a different target is an error, not a conversion. */
    if (t->target == 0) t->target = target;
    else if (t->target != target) { gl_set_error(GL_INVALID_OPERATION); return; }

    *slot = texture;
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

/* Recompute how many levels of face 0 are populated, starting at level 0 and
 * stopping at the first gap.  A chain with a hole in it is not a chain. */
static void recount_levels(gl_texture_t *t) {
    int n = 0;
    while (n < GL_MAX_MIPMAP_LEVELS && t->img[0][n].texels) n++;
    t->levels = n;
}

/* Shared body of glTexImage2D and glTexImage3D. */
static void teximage(GLenum target, GLint level, GLsizei width, GLsizei height,
                     GLsizei depth, GLint border, GLenum format, GLenum type,
                     const GLvoid *pixels, int is_3d) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    GLenum obj_target = image_target_object(target);
    int face = cube_face_index(target);
    if (face < 0) face = 0;

    if (is_3d) {
        if (target != GL_TEXTURE_3D) { gl_set_error(GL_INVALID_ENUM); return; }
    } else if (obj_target != GL_TEXTURE_2D && obj_target != GL_TEXTURE_CUBE_MAP) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }

    if (level < 0 || level >= GL_MAX_MIPMAP_LEVELS) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    if (border != 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (width < 0 || height < 0 || depth < 0) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    if (type != GL_UNSIGNED_BYTE) { gl_set_error(GL_INVALID_ENUM); return; }

    int comps = format_components(format);
    if (comps == 0) { gl_set_error(GL_INVALID_ENUM); return; }

    gl_texture_t *t = bound_texture(ctx, obj_target);
    if (!t) { gl_set_error(GL_INVALID_OPERATION); return; }

    /* Guard the multiplication before it happens: 8192^2 is already 256 MiB
     * unpacked, and a 3D texture multiplies by depth on top of that. */
    if (width > 8192 || height > 8192 || depth > 512) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }

    gl_teximage_t *im = &t->img[face][level];
    image_clear(im);
    im->width  = width;
    im->height = height;
    im->depth  = depth;

    if (width == 0 || height == 0 || depth == 0) {   /* legal: empties it */
        im->width = im->height = im->depth = 0;
        recount_levels(t);
        return;
    }

    size_t count = (size_t)width * (size_t)height * (size_t)depth;
    im->texels = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!im->texels) {
        im->width = im->height = im->depth = 0;
        gl_set_error(GL_OUT_OF_MEMORY);
        recount_levels(t);
        return;
    }

    if (!pixels) {
        /* A NULL pointer allocates storage with undefined contents (§3.8.1).
         * Zeroing is friendlier than leaving heap garbage on screen. */
        memset(im->texels, 0, count * sizeof(uint32_t));
    } else {
        const unsigned char *src = (const unsigned char *)pixels;
        for (size_t i = 0; i < count; i++) {
            im->texels[i] = unpack_pixel(src + i * (size_t)comps, format);
        }
    }

    /* Uploading level 0 anew invalidates any chain built from the old one:
     * levels below would still describe the previous image.  Dropping them is
     * the only safe answer, and it matches what drivers do. */
    if (level == 0) {
        for (int l = 1; l < GL_MAX_MIPMAP_LEVELS; l++) {
            image_clear(&t->img[face][l]);
        }
    }
    recount_levels(t);
}

void glTexImage2D(GLenum target, GLint level, GLint internalFormat,
                  GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels) {
    (void)internalFormat;   /* the stored format is always RGBA8 here */
    teximage(target, level, width, height, 1, border, format, type, pixels, 0);
}

void glTexImage3D(GLenum target, GLint level, GLint internalFormat,
                  GLsizei width, GLsizei height, GLsizei depth, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels) {
    (void)internalFormat;
    teximage(target, level, width, height, depth, border, format, type,
             pixels, 1);
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format,
                     GLenum type, const GLvoid *pixels) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    GLenum obj_target = image_target_object(target);
    int face = cube_face_index(target);
    if (face < 0) face = 0;

    if (obj_target != GL_TEXTURE_2D && obj_target != GL_TEXTURE_CUBE_MAP) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    if (level < 0 || level >= GL_MAX_MIPMAP_LEVELS) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    if (type != GL_UNSIGNED_BYTE) { gl_set_error(GL_INVALID_ENUM); return; }
    if (width < 0 || height < 0 || xoffset < 0 || yoffset < 0) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    if (!pixels) { gl_set_error(GL_INVALID_VALUE); return; }

    int comps = format_components(format);
    if (comps == 0) { gl_set_error(GL_INVALID_ENUM); return; }

    gl_texture_t *t = bound_texture(ctx, obj_target);
    if (!t) { gl_set_error(GL_INVALID_OPERATION); return; }

    gl_teximage_t *im = &t->img[face][level];
    if (!im->texels) { gl_set_error(GL_INVALID_OPERATION); return; }

    if (xoffset + width > im->width || yoffset + height > im->height) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }

    const unsigned char *src = (const unsigned char *)pixels;
    for (GLsizei row = 0; row < height; row++) {
        uint32_t *dst = im->texels + (size_t)(yoffset + row) * im->width + xoffset;
        for (GLsizei col = 0; col < width; col++) {
            dst[col] = unpack_pixel(src + ((size_t)row * width + col) * comps,
                                    format);
        }
    }
}

/* ============================================================================
 * Mipmap generation
 *
 * A box filter: each texel of level n+1 is the mean of the 2x2 (or 2x2x2 for
 * a volume) block above it.  The mean is what makes the chain testable — a box
 * filter preserves the average colour of the image, so every level of a
 * generated chain must have the same mean as level 0, and the unit tests
 * assert exactly that.
 * ==========================================================================*/

static uint32_t average4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t sum = ((a >> shift) & 0xFF) + ((b >> shift) & 0xFF)
                     + ((c >> shift) & 0xFF) + ((d >> shift) & 0xFF);
        /* +2 before the divide rounds to nearest, so a chain of halvings does
         * not drift darker level after level. */
        out |= (((sum + 2) >> 2) & 0xFF) << shift;
    }
    return out;
}

/* Downsample `src` by two in each dimension into a freshly allocated image.
 * A dimension of 1 stays 1, which is what makes non-square textures work. */
static int downsample(const gl_teximage_t *src, gl_teximage_t *dst) {
    GLsizei w = src->width  > 1 ? src->width  / 2 : 1;
    GLsizei h = src->height > 1 ? src->height / 2 : 1;
    GLsizei d = src->depth  > 1 ? src->depth  / 2 : 1;

    size_t count = (size_t)w * (size_t)h * (size_t)d;
    uint32_t *out = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!out) return 0;

    GLsizei sw = src->width, sh = src->height, sd = src->depth;
    for (GLsizei z = 0; z < d; z++) {
        GLsizei z0 = (sd > 1) ? z * 2 : 0;
        GLsizei z1 = (sd > 1) ? z0 + 1 : z0;
        for (GLsizei y = 0; y < h; y++) {
            GLsizei y0 = (sh > 1) ? y * 2 : 0;
            GLsizei y1 = (sh > 1) ? y0 + 1 : y0;
            for (GLsizei x = 0; x < w; x++) {
                GLsizei x0 = (sw > 1) ? x * 2 : 0;
                GLsizei x1 = (sw > 1) ? x0 + 1 : x0;

                #define SRC(X, Y, Z) \
                    src->texels[((size_t)(Z) * sh + (Y)) * sw + (X)]
                uint32_t v = average4(SRC(x0, y0, z0), SRC(x1, y0, z0),
                                      SRC(x0, y1, z0), SRC(x1, y1, z0));
                if (sd > 1) {
                    uint32_t v2 = average4(SRC(x0, y0, z1), SRC(x1, y0, z1),
                                           SRC(x0, y1, z1), SRC(x1, y1, z1));
                    v = average4(v, v, v2, v2);   /* mean of the two slices */
                }
                #undef SRC
                out[((size_t)z * h + y) * w + x] = v;
            }
        }
    }

    dst->texels = out;
    dst->width  = w;
    dst->height = h;
    dst->depth  = d;
    return 1;
}

/* Build levels 1..N for one face from its level 0. */
static int build_chain_face(gl_texture_t *t, int face) {
    if (!t->img[face][0].texels) return 0;

    for (int l = 1; l < GL_MAX_MIPMAP_LEVELS; l++) {
        image_clear(&t->img[face][l]);
    }
    int level = 0;
    while (level + 1 < GL_MAX_MIPMAP_LEVELS) {
        const gl_teximage_t *src = &t->img[face][level];
        if (src->width <= 1 && src->height <= 1 && src->depth <= 1) break;
        if (!downsample(src, &t->img[face][level + 1])) return 0;
        level++;
    }
    return 1;
}

void glGenerateMipmap(GLenum target) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!target_is_valid(target)) { gl_set_error(GL_INVALID_ENUM); return; }

    gl_texture_t *t = bound_texture(ctx, target);
    if (!t || !t->img[0][0].texels) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }

    int faces = (target == GL_TEXTURE_CUBE_MAP) ? GL_CUBE_FACES : 1;
    for (int f = 0; f < faces; f++) {
        if (!t->img[f][0].texels) continue;
        if (!build_chain_face(t, f)) { gl_set_error(GL_OUT_OF_MEMORY); return; }
    }
    recount_levels(t);
}

/* ============================================================================
 * Parameters
 * ==========================================================================*/

static int is_min_filter(GLint p) {
    switch (p) {
    case GL_NEAREST: case GL_LINEAR:
    case GL_NEAREST_MIPMAP_NEAREST: case GL_LINEAR_MIPMAP_NEAREST:
    case GL_NEAREST_MIPMAP_LINEAR:  case GL_LINEAR_MIPMAP_LINEAR:
        return 1;
    default:
        return 0;
    }
}

static int is_wrap_mode(GLint p) {
    return p == GL_REPEAT || p == GL_CLAMP || p == GL_CLAMP_TO_EDGE ||
           p == GL_CLAMP_TO_BORDER;
}

void glTexParameteri(GLenum target, GLenum pname, GLint param) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!target_is_valid(target)) { gl_set_error(GL_INVALID_ENUM); return; }

    gl_texture_t *t = bound_texture(ctx, target);
    if (!t) { gl_set_error(GL_INVALID_OPERATION); return; }

    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
        if (!is_min_filter(param)) { gl_set_error(GL_INVALID_ENUM); return; }
        t->min_filter = (GLenum)param;
        break;
    case GL_TEXTURE_MAG_FILTER:
        /* Magnification has no mipmap variants: there is no smaller level to
         * magnify from (§3.8.8). */
        if (param != GL_NEAREST && param != GL_LINEAR) {
            gl_set_error(GL_INVALID_ENUM);
            return;
        }
        t->mag_filter = (GLenum)param;
        break;
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
    case GL_TEXTURE_WRAP_R:
        if (!is_wrap_mode(param)) { gl_set_error(GL_INVALID_ENUM); return; }
        if      (pname == GL_TEXTURE_WRAP_S) t->wrap_s = (GLenum)param;
        else if (pname == GL_TEXTURE_WRAP_T) t->wrap_t = (GLenum)param;
        else                                 t->wrap_r = (GLenum)param;
        break;
    case GL_TEXTURE_BASE_LEVEL:
        if (param < 0 || param >= GL_MAX_MIPMAP_LEVELS) {
            gl_set_error(GL_INVALID_VALUE);
            return;
        }
        t->base_level = param;
        break;
    case GL_TEXTURE_MAX_LEVEL:
        if (param < 0) { gl_set_error(GL_INVALID_VALUE); return; }
        t->max_level = param < GL_MAX_MIPMAP_LEVELS
                     ? param : GL_MAX_MIPMAP_LEVELS - 1;
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
}

void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!params) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!target_is_valid(target)) { gl_set_error(GL_INVALID_ENUM); return; }

    if (pname == GL_TEXTURE_BORDER_COLOR) {
        gl_texture_t *t = bound_texture(ctx, target);
        if (!t) { gl_set_error(GL_INVALID_OPERATION); return; }
        t->border_color.r = params[0];
        t->border_color.g = params[1];
        t->border_color.b = params[2];
        t->border_color.a = params[3];
        return;
    }
    /* Everything else has integer semantics; GL defines the float entry point
     * as taking the same values rounded. */
    glTexParameteri(target, pname, (GLint)params[0]);
}

/* ============================================================================
 * Texture units and environment
 * ==========================================================================*/

/* Map GL_TEXTUREi to a unit index, or -1. */
static int unit_index(GLenum texture) {
    if (texture < GL_TEXTURE0) return -1;
    int idx = (int)(texture - GL_TEXTURE0);
    if (idx >= GL_MAX_TEXTURE_UNITS_IMPL) return -1;
    return idx;
}

void glActiveTexture(GLenum texture) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    int idx = unit_index(texture);
    if (idx < 0) { gl_set_error(GL_INVALID_ENUM); return; }
    ctx->active_texture = idx;
}

void glClientActiveTexture(GLenum texture) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    int idx = unit_index(texture);
    if (idx < 0) { gl_set_error(GL_INVALID_ENUM); return; }
    ctx->client_active_texture = idx;
}

void glTexEnvi(GLenum target, GLenum pname, GLint param) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (target != GL_TEXTURE_ENV)      { gl_set_error(GL_INVALID_ENUM); return; }
    if (pname  != GL_TEXTURE_ENV_MODE) { gl_set_error(GL_INVALID_ENUM); return; }

    switch (param) {
    case GL_MODULATE: case GL_REPLACE: case GL_DECAL: case GL_BLEND:
        ctx->texunits[ctx->active_texture].env_mode = (GLenum)param;
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
        gl_texunit_t *u = &ctx->texunits[ctx->active_texture];
        u->env_color.r = params[0];
        u->env_color.g = params[1];
        u->env_color.b = params[2];
        u->env_color.a = params[3];
    } else if (pname == GL_TEXTURE_ENV_MODE) {
        glTexEnvi(target, pname, (GLint)params[0]);
    } else {
        gl_set_error(GL_INVALID_ENUM);
    }
}

/* ============================================================================
 * Sampling
 * ==========================================================================*/

/* Apply a wrap mode to an integer texel index.  Returns 1 when the sample is
 * inside the image, 0 when GL_CLAMP_TO_BORDER put it outside — in which case
 * the caller must substitute the border colour rather than a texel. */
static int wrap_coord(int i, int size, GLenum mode, int *out) {
    if (size <= 0) { *out = 0; return 1; }
    if (mode == GL_REPEAT) {
        i %= size;
        if (i < 0) i += size;          /* C's % keeps the sign of the operand */
        *out = i;
        return 1;
    }
    if (mode == GL_CLAMP_TO_BORDER && (i < 0 || i >= size)) {
        *out = 0;
        return 0;                      /* outside: use the border colour */
    }
    /* GL_CLAMP and GL_CLAMP_TO_EDGE both clamp to the edge texel.  True
     * GL_CLAMP blends with the border only at the very edge of the image; with
     * GL_CLAMP_TO_BORDER now available for the applications that want borders,
     * treating GL_CLAMP as clamp-to-edge remains the closer approximation. */
    if (i < 0)      *out = 0;
    else if (i >= size) *out = size - 1;
    else            *out = i;
    return 1;
}

static gl_color_t unpack_color(uint32_t p) {
    gl_color_t c;
    c.a = (GLfloat)((p >> 24) & 0xFF) / 255.0f;
    c.r = (GLfloat)((p >> 16) & 0xFF) / 255.0f;
    c.g = (GLfloat)((p >>  8) & 0xFF) / 255.0f;
    c.b = (GLfloat)( p        & 0xFF) / 255.0f;
    return c;
}

/* One texel of one image, with wrapping and border handling. */
static gl_color_t texel_at(const gl_texture_t *t, const gl_teximage_t *im,
                           int x, int y, int z) {
    int xi, yi, zi;
    int in = 1;
    in &= wrap_coord(x, im->width,  t->wrap_s, &xi);
    in &= wrap_coord(y, im->height, t->wrap_t, &yi);
    in &= wrap_coord(z, im->depth,  t->wrap_r, &zi);
    if (!in) return t->border_color;
    return unpack_color(
        im->texels[((size_t)zi * im->height + yi) * im->width + xi]);
}

/* Nearest / linear sampling of ONE image (one level of one face). */
static gl_color_t sample_image(const gl_texture_t *t, const gl_teximage_t *im,
                               GLfloat s, GLfloat tc, GLfloat rc, int linear) {
    gl_color_t out;
    out.r = out.g = out.b = out.a = 1.0f;
    if (!im->texels || im->width <= 0 || im->height <= 0) return out;

    int is_3d = (im->depth > 1);

    /* Texture space: (0,0) is the corner of texel (0,0), and texel centres sit
     * at (i+0.5)/size.  Hence the -0.5 when locating the sample. */
    GLfloat fx = s  * (GLfloat)im->width  - 0.5f;
    GLfloat fy = tc * (GLfloat)im->height - 0.5f;
    GLfloat fz = is_3d ? rc * (GLfloat)im->depth - 0.5f : 0.0f;

    if (!linear) {
        /* Round to the nearest texel centre.  floorf(x + 0.5) rather than a
         * cast, because casting truncates towards zero and would misbehave for
         * negative coordinates produced by GL_REPEAT. */
        int xi = (int)floorf(fx + 0.5f);
        int yi = (int)floorf(fy + 0.5f);
        int zi = is_3d ? (int)floorf(fz + 0.5f) : 0;
        return texel_at(t, im, xi, yi, zi);
    }

    /* Bilinear (or trilinear inside the volume): blend the surrounding texels. */
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    GLfloat ax = fx - (GLfloat)x0;
    GLfloat ay = fy - (GLfloat)y0;

    int z0 = 0;
    GLfloat az = 0.0f;
    if (is_3d) { z0 = (int)floorf(fz); az = fz - (GLfloat)z0; }

    int zsteps = is_3d ? 2 : 1;
    gl_color_t acc;
    acc.r = acc.g = acc.b = acc.a = 0.0f;

    for (int dz = 0; dz < zsteps; dz++) {
        GLfloat wz = is_3d ? (dz ? az : 1.0f - az) : 1.0f;
        if (wz == 0.0f) continue;
        for (int dy = 0; dy < 2; dy++) {
            GLfloat wy = dy ? ay : 1.0f - ay;
            for (int dx = 0; dx < 2; dx++) {
                GLfloat wx = dx ? ax : 1.0f - ax;
                GLfloat w = wx * wy * wz;
                if (w == 0.0f) continue;
                gl_color_t c = texel_at(t, im, x0 + dx, y0 + dy, z0 + dz);
                acc.r += c.r * w; acc.g += c.g * w;
                acc.b += c.b * w; acc.a += c.a * w;
            }
        }
    }
    return acc;
}

/* ---- Cube map face selection (§3.8.6) ----
 *
 * The face is the one the direction vector's MAJOR axis points at, and the
 * remaining two components, divided by the major one, give the 2D coordinate.
 * The sign conventions below are the specification's table verbatim; they look
 * arbitrary because they are — they exist to make the six faces of a cube map
 * assembled from a real environment join up without seams.
 */
static int cube_face_from_dir(GLfloat x, GLfloat y, GLfloat z,
                              GLfloat *s_out, GLfloat *t_out) {
    GLfloat ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
    int face;
    GLfloat sc, tc, ma;

    if (ax >= ay && ax >= az) {
        ma = ax;
        if (x > 0.0f) { face = 0; sc = -z; tc = -y; }   /* +X */
        else          { face = 1; sc =  z; tc = -y; }   /* -X */
    } else if (ay >= az) {
        ma = ay;
        if (y > 0.0f) { face = 2; sc =  x; tc =  z; }   /* +Y */
        else          { face = 3; sc =  x; tc = -z; }   /* -Y */
    } else {
        ma = az;
        if (z > 0.0f) { face = 4; sc =  x; tc = -y; }   /* +Z */
        else          { face = 5; sc = -x; tc = -y; }   /* -Z */
    }

    if (ma == 0.0f) ma = 1.0f;         /* a zero direction: pick face 0 centre */
    *s_out = 0.5f * (sc / ma + 1.0f);
    *t_out = 0.5f * (tc / ma + 1.0f);
    return face;
}

/* Is `t` both mipmapped by request and mipmapped in fact? */
int gl_texture_uses_mipmaps(const gl_texture_t *t) {
    if (!t) return 0;
    switch (t->min_filter) {
    case GL_NEAREST_MIPMAP_NEAREST: case GL_LINEAR_MIPMAP_NEAREST:
    case GL_NEAREST_MIPMAP_LINEAR:  case GL_LINEAR_MIPMAP_LINEAR:
        return t->levels > 1;
    default:
        return 0;
    }
}

int gl_texture_base_size(const gl_texture_t *t, GLfloat *w_out, GLfloat *h_out) {
    if (!t || !t->img[0][0].texels) return 0;
    if (w_out) *w_out = (GLfloat)t->img[0][0].width;
    if (h_out) *h_out = (GLfloat)t->img[0][0].height;
    return 1;
}

/* Does the min filter blend two levels, and does it filter within a level? */
static void decode_min_filter(GLenum f, int *linear_in_level, int *blend_levels) {
    switch (f) {
    case GL_NEAREST:                 *linear_in_level = 0; *blend_levels = 0; break;
    case GL_LINEAR:                  *linear_in_level = 1; *blend_levels = 0; break;
    case GL_NEAREST_MIPMAP_NEAREST:  *linear_in_level = 0; *blend_levels = 0; break;
    case GL_LINEAR_MIPMAP_NEAREST:   *linear_in_level = 1; *blend_levels = 0; break;
    case GL_NEAREST_MIPMAP_LINEAR:   *linear_in_level = 0; *blend_levels = 1; break;
    case GL_LINEAR_MIPMAP_LINEAR:    *linear_in_level = 1; *blend_levels = 1; break;
    default:                         *linear_in_level = 1; *blend_levels = 0; break;
    }
}

gl_color_t gl_texture_sample_lod(const gl_texture_t *t,
                                 GLfloat s, GLfloat tc, GLfloat rc,
                                 GLfloat lod) {
    gl_color_t out;
    out.r = out.g = out.b = out.a = 1.0f;
    if (!t) return out;

    /* Cube maps replace (s,t,r) with a direction and pick a face from it. */
    int face = 0;
    if (t->target == GL_TEXTURE_CUBE_MAP) {
        GLfloat fs, ft;
        face = cube_face_from_dir(s, tc, rc, &fs, &ft);
        s = fs; tc = ft;
        if (!t->img[face][0].texels) face = 0;   /* incomplete cube map */
    }

    const gl_teximage_t *base = &t->img[face][0];
    if (!base->texels) return out;

    /* Magnification, or minification without a usable chain: one level. */
    int mag = (lod <= 0.0f);
    if (mag) {
        return sample_image(t, base, s, tc, rc, t->mag_filter == GL_LINEAR);
    }

    int linear_in_level, blend_levels;
    decode_min_filter(t->min_filter, &linear_in_level, &blend_levels);

    /* Count the levels available on THIS face, which for a cube map may be
     * shorter than face 0's chain. */
    int avail = 0;
    while (avail < GL_MAX_MIPMAP_LEVELS && t->img[face][avail].texels) avail++;

    int top = avail - 1;
    if (top > t->max_level) top = t->max_level;
    int bottom = t->base_level <= top ? t->base_level : top;

    if (top <= bottom || !gl_texture_uses_mipmaps(t)) {
        /* No chain to walk: honour the in-level part of the filter and stop.
         * This is the "incomplete texture" fallback of §3.8.10 and is what
         * makes a plain glTexImage2D with the default min filter still draw. */
        const gl_teximage_t *im = &t->img[face][bottom];
        if (!im->texels) im = base;
        return sample_image(t, im, s, tc, rc, linear_in_level);
    }

    if (lod < (GLfloat)bottom) lod = (GLfloat)bottom;
    if (lod > (GLfloat)top)    lod = (GLfloat)top;

    if (!blend_levels) {
        /* _MIPMAP_NEAREST: round to the nearest level.  GL rounds x.5 DOWN
         * here (§3.8.8), unlike the usual convention. */
        int level = (int)ceilf(lod + 0.5f) - 1;
        if (level < bottom) level = bottom;
        if (level > top)    level = top;
        return sample_image(t, &t->img[face][level], s, tc, rc, linear_in_level);
    }

    /* _MIPMAP_LINEAR: sample both bracketing levels and blend. */
    int l0 = (int)floorf(lod);
    if (l0 < bottom) l0 = bottom;
    if (l0 > top)    l0 = top;
    int l1 = l0 + 1 <= top ? l0 + 1 : top;
    GLfloat frac = lod - (GLfloat)l0;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    gl_color_t a = sample_image(t, &t->img[face][l0], s, tc, rc, linear_in_level);
    if (l1 == l0 || frac == 0.0f) return a;
    gl_color_t b = sample_image(t, &t->img[face][l1], s, tc, rc, linear_in_level);

    gl_color_t r;
    r.r = a.r + (b.r - a.r) * frac;
    r.g = a.g + (b.g - a.g) * frac;
    r.b = a.b + (b.b - a.b) * frac;
    r.a = a.a + (b.a - a.a) * frac;
    return r;
}

gl_color_t gl_texture_sample(const gl_texture_t *t, GLfloat s, GLfloat tc,
                             int magnifying) {
    /* The G6 entry point, kept so callers that have no LOD to offer still
     * work: a magnifying sample is lod 0, a minifying one asks for level 0 of
     * the chain with the min filter applied within the level. */
    return gl_texture_sample_lod(t, s, tc, 0.0f, magnifying ? 0.0f : 1e-6f);
}

/* Combine the sampled texel with the incoming fragment colour according to the
 * unit's texture environment mode (§3.8.9). */
gl_color_t gl_texture_env_unit(const struct aglx_context *ctx, int unit,
                               gl_color_t frag, gl_color_t tex) {
    gl_color_t out = frag;
    if (unit < 0 || unit >= GL_MAX_TEXTURE_UNITS_IMPL) return out;
    const gl_texunit_t *u = &ctx->texunits[unit];

    switch (u->env_mode) {
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
        out.r = frag.r * (1.0f - tex.r) + u->env_color.r * tex.r;
        out.g = frag.g * (1.0f - tex.g) + u->env_color.g * tex.g;
        out.b = frag.b * (1.0f - tex.b) + u->env_color.b * tex.b;
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

gl_color_t gl_texture_env(const struct aglx_context *ctx,
                          gl_color_t frag, gl_color_t tex) {
    return gl_texture_env_unit(ctx, 0, frag, tex);
}

/* libgl/src/glfbo.c — framebuffer objects, renderbuffers and glReadPixels.
 *
 * Phase G12 of GL_PLAN.md.  GL 3.0 / EXT_framebuffer_object core subset.
 *
 * WHY THIS PHASE IS SMALL
 *
 * The rasterizer writes through ctx->color, ctx->depth, ctx->stencil,
 * ctx->width and ctx->height, and never asks where they came from.
 * Render-to-texture is therefore a matter of pointing those somewhere else
 * and pointing them back afterwards.
 *
 * THE ONE THING THAT MAKES IT NON-TRIVIAL: RESOLUTION TIMING
 *
 * An FBO stores REFERENCES (texture name, level, face), not pointers.  A
 * texture can be re-uploaded at a different size, or deleted outright, while
 * still attached.  So the attachment is resolved to a real pixel pointer at
 * BIND time, not at attach time, and re-resolved on every bind.  Caching the
 * pointer at attach time would be faster and would produce a use-after-free
 * the first time an application re-specified an attached texture — which is
 * a thing applications legitimately do.
 *
 * WHAT IS DELIBERATELY ABSENT
 *
 * - Multiple colour attachments.  The fixed-function pipeline writes one
 *   colour, so a second attachment would receive nothing.  The loop bounds
 *   are already written against GL_MAX_COLOR_ATTACHMENTS_IMPL so that a
 *   shader path (G11) can raise it without restructuring anything.
 * - Packed depth-stencil (D24S8).  Stencil is a separate 8-bit plane
 *   (GL2 L1); attaching GL_STENCIL_INDEX8 is real.
 * - Multisampling and blitting between framebuffers.
 */

#include <stdlib.h>
#include <string.h>

#include "GL/gl.h"
#include "GL/auraglx.h"
#include "glcontext.h"
#include "glvertex.h"

/* ============================================================================
 * Object lookup
 * ==========================================================================*/

static gl_framebuffer_t *find_fbo(struct aglx_context *ctx, GLuint name) {
    if (name == 0) return (gl_framebuffer_t *)0;
    for (int i = 0; i < GL_MAX_FRAMEBUFFERS_IMPL; i++) {
        if (ctx->framebuffers[i].used && ctx->framebuffers[i].name == name) {
            return &ctx->framebuffers[i];
        }
    }
    return (gl_framebuffer_t *)0;
}

static gl_renderbuffer_t *find_rbo(struct aglx_context *ctx, GLuint name) {
    if (name == 0) return (gl_renderbuffer_t *)0;
    for (int i = 0; i < GL_MAX_RENDERBUFFERS_IMPL; i++) {
        if (ctx->renderbuffers[i].used && ctx->renderbuffers[i].name == name) {
            return &ctx->renderbuffers[i];
        }
    }
    return (gl_renderbuffer_t *)0;
}

/* Texture lookup lives in gltexture.c; this needs its own because the
 * attachment code must reach a texture by name without going through the
 * per-unit bindings. */
static gl_texture_t *find_texture_by_name(struct aglx_context *ctx,
                                          GLuint name) {
    if (name == 0) return (gl_texture_t *)0;
    for (int i = 0; i < GL_MAX_TEXTURES_IMPL; i++) {
        if (ctx->textures[i].used && ctx->textures[i].name == name) {
            return &ctx->textures[i];
        }
    }
    return (gl_texture_t *)0;
}

static void attachment_clear(gl_attachment_t *a) {
    a->kind      = GL_ATTACH_NONE;
    a->name      = 0;
    a->textarget = 0;
    a->level     = 0;
}

void gl_fbo_set_defaults(struct aglx_context *ctx) {
    for (int i = 0; i < GL_MAX_FRAMEBUFFERS_IMPL; i++) {
        ctx->framebuffers[i].name = 0;
        ctx->framebuffers[i].used = 0;
        for (int a = 0; a < GL_MAX_COLOR_ATTACHMENTS_IMPL; a++) {
            attachment_clear(&ctx->framebuffers[i].color[a]);
        }
        attachment_clear(&ctx->framebuffers[i].depth);
        attachment_clear(&ctx->framebuffers[i].stencil);
    }
    for (int i = 0; i < GL_MAX_RENDERBUFFERS_IMPL; i++) {
        ctx->renderbuffers[i].name   = 0;
        ctx->renderbuffers[i].used   = 0;
        ctx->renderbuffers[i].format = 0;
        ctx->renderbuffers[i].width  = 0;
        ctx->renderbuffers[i].height = 0;
        ctx->renderbuffers[i].color  = (gl_pixel_t *)0;
        ctx->renderbuffers[i].depth  = (float *)0;
        ctx->renderbuffers[i].stencil = (uint8_t *)0;
    }
    ctx->next_framebuffer_name  = 1;
    ctx->next_renderbuffer_name = 1;
    ctx->framebuffer_binding    = 0;
    ctx->renderbuffer_binding   = 0;
}

void gl_fbo_free_all(struct aglx_context *ctx) {
    for (int i = 0; i < GL_MAX_RENDERBUFFERS_IMPL; i++) {
        free(ctx->renderbuffers[i].color);
        free(ctx->renderbuffers[i].depth);
        free(ctx->renderbuffers[i].stencil);
        ctx->renderbuffers[i].color   = (gl_pixel_t *)0;
        ctx->renderbuffers[i].depth   = (float *)0;
        ctx->renderbuffers[i].stencil = (uint8_t *)0;
        ctx->renderbuffers[i].used    = 0;
    }
}

/* ============================================================================
 * Attachment resolution
 *
 * Turn an attachment record into a pixel pointer plus dimensions.  Returns 0
 * when the attachment cannot be resolved — a deleted texture, a level that was
 * never uploaded, a renderbuffer with no storage.
 * ==========================================================================*/

typedef struct {
    gl_pixel_t *color;
    float      *depth;
    uint8_t    *stencil;
    GLsizei     width, height;
} resolved_t;

static int resolve_attachment(struct aglx_context *ctx,
                              const gl_attachment_t *a, resolved_t *out) {
    out->color   = (gl_pixel_t *)0;
    out->depth   = (float *)0;
    out->stencil = (uint8_t *)0;
    out->width = out->height = 0;

    if (a->kind == GL_ATTACH_NONE) return 0;

    if (a->kind == GL_ATTACH_RENDERBUFFER) {
        gl_renderbuffer_t *rb = find_rbo(ctx, a->name);
        if (!rb) return 0;
        if (!rb->color && !rb->depth && !rb->stencil) return 0;
        out->color   = rb->color;
        out->depth   = rb->depth;
        out->stencil = rb->stencil;
        out->width   = rb->width;
        out->height  = rb->height;
        return 1;
    }

    /* A texture attachment.  Texels are stored as 0xAARRGGBB, and the
     * framebuffer wants 0x00RRGGBB — the same 32 bits with the alpha byte
     * unused, so the image can be rendered into directly with no conversion
     * and then sampled directly afterwards.  That equivalence is why
     * render-to-texture costs nothing here, and it is worth stating because
     * it is the reason this function can hand back t->texels unmodified.
     *
     * The cost of the shortcut: rendering into a texture leaves its alpha
     * bytes as whatever the rasterizer wrote, which is zero.  Sampling the
     * result with GL_MODULATE would therefore multiply alpha by zero.  The
     * colour attachment is forced opaque on unbind (see gl_fbo_finish_color)
     * so applications get the behaviour they expect. */
    gl_texture_t *t = find_texture_by_name(ctx, a->name);
    if (!t) return 0;

    int face = 0;
    if (a->textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
        a->textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) {
        face = (int)(a->textarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
    }
    if (a->level < 0 || a->level >= GL_MAX_MIPMAP_LEVELS) return 0;

    gl_teximage_t *im = &t->img[face][a->level];
    if (!im->texels || im->width <= 0 || im->height <= 0) return 0;
    /* A 3D texture slice is not attachable here: there is no way to say which
     * slice, and glFramebufferTexture3D is not implemented. */
    if (im->depth > 1) return 0;

    out->color  = (gl_pixel_t *)im->texels;
    out->width  = im->width;
    out->height = im->height;
    return 1;
}

/* ============================================================================
 * Completeness (§4.4.4)
 * ==========================================================================*/

static GLenum framebuffer_status(struct aglx_context *ctx,
                                 gl_framebuffer_t *fb) {
    if (!fb) return GL_FRAMEBUFFER_COMPLETE;   /* framebuffer 0 always is */

    int have_any = 0;
    GLsizei w = 0, h = 0;

    for (int i = 0; i < GL_MAX_COLOR_ATTACHMENTS_IMPL; i++) {
        gl_attachment_t *a = &fb->color[i];
        if (a->kind == GL_ATTACH_NONE) continue;

        resolved_t r;
        if (!resolve_attachment(ctx, a, &r)) {
            return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        }
        /* A depth renderbuffer bound to a colour attachment point has no
         * colour storage: that is an unusable attachment, not a missing one. */
        if (!r.color) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;

        if (!have_any) { w = r.width; h = r.height; have_any = 1; }
        else if (r.width != w || r.height != h) {
            return GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS;
        }
    }

    if (fb->depth.kind != GL_ATTACH_NONE) {
        resolved_t r;
        if (!resolve_attachment(ctx, &fb->depth, &r)) {
            return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        }
        /* Depth must come from a depth renderbuffer.  Attaching a colour
         * texture as depth is a type error the application wants told about,
         * not silently reinterpreted as float. */
        if (!r.depth) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;

        if (!have_any) { w = r.width; h = r.height; have_any = 1; }
        else if (r.width != w || r.height != h) {
            return GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS;
        }
    }

    if (fb->stencil.kind != GL_ATTACH_NONE) {
        resolved_t r;
        if (!resolve_attachment(ctx, &fb->stencil, &r)) {
            return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
        }
        if (!r.stencil) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;

        if (!have_any) { w = r.width; h = r.height; have_any = 1; }
        else if (r.width != w || r.height != h) {
            return GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS;
        }
    }

    /* An FBO with nothing attached has no pixels to write and is incomplete
     * (§4.4.4) — this catches the common bug of generating and binding an FBO
     * and then forgetting the attachment, which would otherwise render into
     * the window and look baffling. */
    if (!have_any) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;

    return GL_FRAMEBUFFER_COMPLETE;
}

GLenum glCheckFramebufferStatus(GLenum target) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return 0;
    if (target != GL_FRAMEBUFFER) { gl_set_error(GL_INVALID_ENUM); return 0; }

    if (ctx->framebuffer_binding == 0) return GL_FRAMEBUFFER_COMPLETE;
    gl_framebuffer_t *fb = find_fbo(ctx, ctx->framebuffer_binding);
    if (!fb) return GL_FRAMEBUFFER_UNDEFINED;
    return framebuffer_status(ctx, fb);
}

/* ============================================================================
 * Applying a binding
 * ==========================================================================*/

/* Rendering into a texture writes 0x00RRGGBB, leaving the alpha byte zero.
 * The same memory is read back as 0xAARRGGBB when the texture is sampled, so
 * without this the result would sample as fully transparent.  Forcing alpha
 * opaque when the attachment is released is the least surprising answer: the
 * application rendered opaque geometry and expects an opaque texture. */
static void gl_fbo_finish_color(struct aglx_context *ctx,
                                gl_framebuffer_t *fb) {
    if (!fb) return;
    for (int i = 0; i < GL_MAX_COLOR_ATTACHMENTS_IMPL; i++) {
        gl_attachment_t *a = &fb->color[i];
        if (a->kind != GL_ATTACH_TEXTURE) continue;
        resolved_t r;
        if (!resolve_attachment(ctx, a, &r) || !r.color) continue;
        size_t n = (size_t)r.width * (size_t)r.height;
        for (size_t k = 0; k < n; k++) r.color[k] |= 0xFF000000u;
    }
}

/* Point the rasterizer at whatever the current binding says.  Called on every
 * bind, and by the attachment entry points when they change the framebuffer
 * that is already bound. */
static void gl_fbo_apply(struct aglx_context *ctx) {
    if (ctx->framebuffer_binding == 0) {
        ctx->color   = ctx->win_color;
        ctx->depth   = ctx->win_depth;
        ctx->stencil = ctx->win_stencil;
        ctx->width   = ctx->win_width;
        ctx->height  = ctx->win_height;
        /* The window's framebuffer stores row 0 at the top. */
        ctx->target_flip_y = 1;
        return;
    }

    gl_framebuffer_t *fb = find_fbo(ctx, ctx->framebuffer_binding);
    if (!fb || framebuffer_status(ctx, fb) != GL_FRAMEBUFFER_COMPLETE) {
        /* An incomplete framebuffer must not render anywhere.  GL says the
         * results of rendering are undefined and glClear/draw generate
         * GL_INVALID_FRAMEBUFFER_OPERATION; this implementation keeps the
         * previous target's dimensions but a NULL colour pointer, and the
         * draw entry points check for that.  Rendering into the window
         * instead would be far worse: the mistake would be invisible. */
        ctx->color   = (gl_pixel_t *)0;
        ctx->depth   = (float *)0;
        ctx->stencil = (uint8_t *)0;
        ctx->width   = 0;
        ctx->height  = 0;
        return;
    }

    /* An FBO renders into a texture or a renderbuffer, both of which store
     * row 0 at the BOTTOM -- the same convention GL window coordinates use --
     * so no vertical flip applies.  This is what makes a render-to-texture
     * come out the right way up when it is sampled afterwards. */
    ctx->target_flip_y = 0;

    resolved_t rc;
    if (resolve_attachment(ctx, &fb->color[0], &rc) && rc.color) {
        ctx->color  = rc.color;
        ctx->width  = rc.width;
        ctx->height = rc.height;
    } else {
        ctx->color  = (gl_pixel_t *)0;
        ctx->width  = ctx->height = 0;
    }

    resolved_t rd;
    if (fb->depth.kind != GL_ATTACH_NONE &&
        resolve_attachment(ctx, &fb->depth, &rd) && rd.depth) {
        ctx->depth = rd.depth;
        /* A depth-only FBO sets the size from the depth attachment. */
        if (!ctx->color) { ctx->width = rd.width; ctx->height = rd.height; }
    } else {
        /* No depth attachment means no depth buffer, exactly like a context
         * created without AGLX_DEPTH: the depth test silently does nothing
         * rather than writing through the window's depth buffer, which would
         * corrupt it. */
        ctx->depth = (float *)0;
    }

    resolved_t rs;
    if (fb->stencil.kind != GL_ATTACH_NONE &&
        resolve_attachment(ctx, &fb->stencil, &rs) && rs.stencil) {
        ctx->stencil = rs.stencil;
        if (!ctx->color && !ctx->depth) {
            ctx->width = rs.width; ctx->height = rs.height;
        }
    } else {
        ctx->stencil = (uint8_t *)0;
    }
}

/* Is the current draw target usable?  The draw and clear entry points call
 * this so an incomplete FBO produces an error instead of a NULL dereference. */
int gl_fbo_target_ok(struct aglx_context *ctx) {
    return ctx->color != (gl_pixel_t *)0 && ctx->width > 0 && ctx->height > 0;
}

/* Called by aglxResize() and aglxCreateContext() after they change the window
 * buffers, so a context that is not FBO-bound follows the new ones. */
void gl_fbo_refresh(struct aglx_context *ctx) {
    gl_fbo_apply(ctx);
}

/* ============================================================================
 * Framebuffer entry points
 * ==========================================================================*/

void glGenFramebuffers(GLsizei n, GLuint *framebuffers) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!framebuffers || n == 0) return;

    GLsizei made = 0;
    for (int i = 0; i < GL_MAX_FRAMEBUFFERS_IMPL && made < n; i++) {
        if (ctx->framebuffers[i].used) continue;
        gl_framebuffer_t *fb = &ctx->framebuffers[i];
        fb->used = 1;
        fb->name = ctx->next_framebuffer_name++;
        for (int a = 0; a < GL_MAX_COLOR_ATTACHMENTS_IMPL; a++) {
            attachment_clear(&fb->color[a]);
        }
        attachment_clear(&fb->depth);
        attachment_clear(&fb->stencil);
        framebuffers[made++] = fb->name;
    }
    if (made < n) {
        for (GLsizei k = made; k < n; k++) framebuffers[k] = 0;
        gl_set_error(GL_OUT_OF_MEMORY);
    }
}

void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!framebuffers) return;

    for (GLsizei k = 0; k < n; k++) {
        gl_framebuffer_t *fb = find_fbo(ctx, framebuffers[k]);
        if (!fb) continue;
        /* Deleting the bound framebuffer reverts the binding to 0 (§4.4.1),
         * which must also re-point the rasterizer at the window. */
        if (ctx->framebuffer_binding == framebuffers[k]) {
            gl_fbo_finish_color(ctx, fb);
            ctx->framebuffer_binding = 0;
            gl_fbo_apply(ctx);
        }
        fb->used = 0;
        fb->name = 0;
    }
}

GLboolean glIsFramebuffer(GLuint framebuffer) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return GL_FALSE;
    return find_fbo(ctx, framebuffer) ? GL_TRUE : GL_FALSE;
}

void glBindFramebuffer(GLenum target, GLuint framebuffer) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (target != GL_FRAMEBUFFER) { gl_set_error(GL_INVALID_ENUM); return; }

    /* Leaving a texture-backed FBO is the moment to fix up its alpha. */
    if (ctx->framebuffer_binding != 0 &&
        ctx->framebuffer_binding != framebuffer) {
        gl_fbo_finish_color(ctx, find_fbo(ctx, ctx->framebuffer_binding));
    }

    if (framebuffer == 0) {
        ctx->framebuffer_binding = 0;
        gl_fbo_apply(ctx);
        return;
    }

    gl_framebuffer_t *fb = find_fbo(ctx, framebuffer);
    if (!fb) {
        /* Like textures and buffers, binding an unused name creates the
         * object (§4.4.1). */
        for (int i = 0; i < GL_MAX_FRAMEBUFFERS_IMPL; i++) {
            if (ctx->framebuffers[i].used) continue;
            fb = &ctx->framebuffers[i];
            fb->used = 1;
            fb->name = framebuffer;
            for (int a = 0; a < GL_MAX_COLOR_ATTACHMENTS_IMPL; a++) {
                attachment_clear(&fb->color[a]);
            }
            attachment_clear(&fb->depth);
            attachment_clear(&fb->stencil);
            if (framebuffer >= ctx->next_framebuffer_name) {
                ctx->next_framebuffer_name = framebuffer + 1;
            }
            break;
        }
        if (!fb || !fb->used) { gl_set_error(GL_OUT_OF_MEMORY); return; }
    }

    ctx->framebuffer_binding = framebuffer;
    gl_fbo_apply(ctx);
}

/* Which attachment slot does this enum name?  NULL for one this
 * implementation does not have. */
static gl_attachment_t *attachment_slot(gl_framebuffer_t *fb,
                                        GLenum attachment, GLenum *err) {
    *err = GL_NO_ERROR;
    if (attachment >= GL_COLOR_ATTACHMENT0 &&
        attachment < GL_COLOR_ATTACHMENT0 + GL_MAX_COLOR_ATTACHMENTS_IMPL) {
        return &fb->color[attachment - GL_COLOR_ATTACHMENT0];
    }
    if (attachment == GL_DEPTH_ATTACHMENT) return &fb->depth;
    if (attachment == GL_STENCIL_ATTACHMENT) return &fb->stencil;
    *err = GL_INVALID_ENUM;
    return (gl_attachment_t *)0;
}

void glFramebufferTexture2D(GLenum target, GLenum attachment,
                            GLenum textarget, GLuint texture, GLint level) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (target != GL_FRAMEBUFFER) { gl_set_error(GL_INVALID_ENUM); return; }

    /* Attaching to the default framebuffer is meaningless: the window's
     * storage belongs to the window system. */
    if (ctx->framebuffer_binding == 0) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }
    gl_framebuffer_t *fb = find_fbo(ctx, ctx->framebuffer_binding);
    if (!fb) { gl_set_error(GL_INVALID_OPERATION); return; }

    int is_cube_face = (textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
                        textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z);
    if (textarget != GL_TEXTURE_2D && !is_cube_face) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    if (level < 0 || level >= GL_MAX_MIPMAP_LEVELS) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }

    GLenum err;
    gl_attachment_t *slot = attachment_slot(fb, attachment, &err);
    if (!slot) { gl_set_error(err); return; }

    if (texture == 0) {
        attachment_clear(slot);
    } else {
        if (!find_texture_by_name(ctx, texture)) {
            gl_set_error(GL_INVALID_OPERATION);
            return;
        }
        slot->kind      = GL_ATTACH_TEXTURE;
        slot->name      = texture;
        slot->textarget = textarget;
        slot->level     = level;
    }
    /* Changing an attachment on the FBO that is currently bound must take
     * effect immediately, not at the next bind. */
    gl_fbo_apply(ctx);
}

void glFramebufferRenderbuffer(GLenum target, GLenum attachment,
                               GLenum renderbuffertarget, GLuint renderbuffer) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (target != GL_FRAMEBUFFER)            { gl_set_error(GL_INVALID_ENUM); return; }
    if (renderbuffertarget != GL_RENDERBUFFER) { gl_set_error(GL_INVALID_ENUM); return; }

    if (ctx->framebuffer_binding == 0) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }
    gl_framebuffer_t *fb = find_fbo(ctx, ctx->framebuffer_binding);
    if (!fb) { gl_set_error(GL_INVALID_OPERATION); return; }

    GLenum err;
    gl_attachment_t *slot = attachment_slot(fb, attachment, &err);
    if (!slot) { gl_set_error(err); return; }

    if (renderbuffer == 0) {
        attachment_clear(slot);
    } else {
        if (!find_rbo(ctx, renderbuffer)) {
            gl_set_error(GL_INVALID_OPERATION);
            return;
        }
        slot->kind      = GL_ATTACH_RENDERBUFFER;
        slot->name      = renderbuffer;
        slot->textarget = 0;
        slot->level     = 0;
    }
    gl_fbo_apply(ctx);
}

/* ============================================================================
 * Renderbuffer entry points
 * ==========================================================================*/

void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!renderbuffers || n == 0) return;

    GLsizei made = 0;
    for (int i = 0; i < GL_MAX_RENDERBUFFERS_IMPL && made < n; i++) {
        if (ctx->renderbuffers[i].used) continue;
        gl_renderbuffer_t *rb = &ctx->renderbuffers[i];
        rb->used   = 1;
        rb->name   = ctx->next_renderbuffer_name++;
        rb->format = 0;
        rb->width  = rb->height = 0;
        rb->color   = (gl_pixel_t *)0;
        rb->depth   = (float *)0;
        rb->stencil = (uint8_t *)0;
        renderbuffers[made++] = rb->name;
    }
    if (made < n) {
        for (GLsizei k = made; k < n; k++) renderbuffers[k] = 0;
        gl_set_error(GL_OUT_OF_MEMORY);
    }
}

void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!renderbuffers) return;

    for (GLsizei k = 0; k < n; k++) {
        gl_renderbuffer_t *rb = find_rbo(ctx, renderbuffers[k]);
        if (!rb) continue;

        /* Detach it from every framebuffer first.  Leaving the reference
         * behind would make the FBO resolve a freed object on its next bind —
         * the exact use-after-free that resolving lazily is meant to avoid,
         * reintroduced by the back door. */
        for (int i = 0; i < GL_MAX_FRAMEBUFFERS_IMPL; i++) {
            if (!ctx->framebuffers[i].used) continue;
            gl_framebuffer_t *fb = &ctx->framebuffers[i];
            for (int a = 0; a < GL_MAX_COLOR_ATTACHMENTS_IMPL; a++) {
                if (fb->color[a].kind == GL_ATTACH_RENDERBUFFER &&
                    fb->color[a].name == renderbuffers[k]) {
                    attachment_clear(&fb->color[a]);
                }
            }
            if (fb->depth.kind == GL_ATTACH_RENDERBUFFER &&
                fb->depth.name == renderbuffers[k]) {
                attachment_clear(&fb->depth);
            }
            if (fb->stencil.kind == GL_ATTACH_RENDERBUFFER &&
                fb->stencil.name == renderbuffers[k]) {
                attachment_clear(&fb->stencil);
            }
        }

        free(rb->color);
        free(rb->depth);
        free(rb->stencil);
        rb->color   = (gl_pixel_t *)0;
        rb->depth   = (float *)0;
        rb->stencil = (uint8_t *)0;
        rb->used  = 0;
        rb->name  = 0;
        if (ctx->renderbuffer_binding == renderbuffers[k]) {
            ctx->renderbuffer_binding = 0;
        }
    }
    /* A detachment may have made the bound framebuffer incomplete. */
    gl_fbo_apply(ctx);
}

GLboolean glIsRenderbuffer(GLuint renderbuffer) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return GL_FALSE;
    return find_rbo(ctx, renderbuffer) ? GL_TRUE : GL_FALSE;
}

void glBindRenderbuffer(GLenum target, GLuint renderbuffer) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (target != GL_RENDERBUFFER) { gl_set_error(GL_INVALID_ENUM); return; }

    if (renderbuffer == 0) { ctx->renderbuffer_binding = 0; return; }

    if (!find_rbo(ctx, renderbuffer)) {
        for (int i = 0; i < GL_MAX_RENDERBUFFERS_IMPL; i++) {
            if (ctx->renderbuffers[i].used) continue;
            gl_renderbuffer_t *rb = &ctx->renderbuffers[i];
            rb->used   = 1;
            rb->name   = renderbuffer;
            rb->format = 0;
            rb->width  = rb->height = 0;
            rb->color   = (gl_pixel_t *)0;
            rb->depth   = (float *)0;
            rb->stencil = (uint8_t *)0;
            if (renderbuffer >= ctx->next_renderbuffer_name) {
                ctx->next_renderbuffer_name = renderbuffer + 1;
            }
            ctx->renderbuffer_binding = renderbuffer;
            return;
        }
        gl_set_error(GL_OUT_OF_MEMORY);
        return;
    }
    ctx->renderbuffer_binding = renderbuffer;
}

/* Is this internal format a depth format? */
static int format_is_depth(GLenum f) {
    return f == GL_DEPTH_COMPONENT   || f == GL_DEPTH_COMPONENT16 ||
           f == GL_DEPTH_COMPONENT24 || f == GL_DEPTH_COMPONENT32F;
}

static int format_is_color(GLenum f) {
    return f == GL_RGBA8 || f == GL_RGB8 || f == GL_RGBA || f == GL_RGB;
}

static int format_is_stencil(GLenum f) {
    return f == GL_STENCIL_INDEX8 || f == GL_STENCIL_INDEX;
}

void glRenderbufferStorage(GLenum target, GLenum internalformat,
                           GLsizei width, GLsizei height) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (target != GL_RENDERBUFFER) { gl_set_error(GL_INVALID_ENUM); return; }
    if (width < 0 || height < 0)   { gl_set_error(GL_INVALID_VALUE); return; }
    if (width > AGLX_MAX_DIM || height > AGLX_MAX_DIM) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }

    gl_renderbuffer_t *rb = find_rbo(ctx, ctx->renderbuffer_binding);
    if (!rb) { gl_set_error(GL_INVALID_OPERATION); return; }

    int depth_fmt   = format_is_depth(internalformat);
    int color_fmt   = format_is_color(internalformat);
    int stencil_fmt = format_is_stencil(internalformat);
    if (!depth_fmt && !color_fmt && !stencil_fmt) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }

    /* Re-specifying storage discards the old contents (§4.4.2). */
    free(rb->color);   rb->color   = (gl_pixel_t *)0;
    free(rb->depth);   rb->depth   = (float *)0;
    free(rb->stencil); rb->stencil = (uint8_t *)0;
    rb->format = internalformat;
    rb->width  = width;
    rb->height = height;

    if (width == 0 || height == 0) { gl_fbo_apply(ctx); return; }

    size_t n = (size_t)width * (size_t)height;
    if (depth_fmt) {
        /* Every depth format is stored as float regardless of the requested
         * bit depth: the rasterizer's depth buffer is float, and converting
         * per fragment to honour a 16-bit request would cost more than the
         * memory it saves. */
        rb->depth = (float *)malloc(n * sizeof(float));
        if (!rb->depth) {
            rb->width = rb->height = 0;
            gl_set_error(GL_OUT_OF_MEMORY);
            return;
        }
        for (size_t i = 0; i < n; i++) rb->depth[i] = 1.0f;
    } else if (stencil_fmt) {
        rb->stencil = (uint8_t *)malloc(n);
        if (!rb->stencil) {
            rb->width = rb->height = 0;
            gl_set_error(GL_OUT_OF_MEMORY);
            return;
        }
        memset(rb->stencil, 0, n);
    } else {
        rb->color = (gl_pixel_t *)malloc(n * sizeof(gl_pixel_t));
        if (!rb->color) {
            rb->width = rb->height = 0;
            gl_set_error(GL_OUT_OF_MEMORY);
            return;
        }
        memset(rb->color, 0, n * sizeof(gl_pixel_t));
    }

    /* Giving a previously empty renderbuffer storage can complete the bound
     * framebuffer, so the target has to be recomputed. */
    gl_fbo_apply(ctx);
}

void glGetRenderbufferParameteriv(GLenum target, GLenum pname, GLint *params) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (target != GL_RENDERBUFFER) { gl_set_error(GL_INVALID_ENUM); return; }
    if (!params) { gl_set_error(GL_INVALID_VALUE); return; }

    gl_renderbuffer_t *rb = find_rbo(ctx, ctx->renderbuffer_binding);
    if (!rb) { gl_set_error(GL_INVALID_OPERATION); return; }

    switch (pname) {
    case GL_RENDERBUFFER_WIDTH:           params[0] = (GLint)rb->width;  break;
    case GL_RENDERBUFFER_HEIGHT:          params[0] = (GLint)rb->height; break;
    case GL_RENDERBUFFER_INTERNAL_FORMAT: params[0] = (GLint)rb->format; break;
    default: gl_set_error(GL_INVALID_ENUM); break;
    }
}

/* ============================================================================
 * glReadPixels (§4.3.2)
 *
 * Reads from whatever the current draw target is, which means it works
 * against an FBO as naturally as against the window — that is the reason this
 * belongs in the same phase.
 *
 * The (x,y) origin is BOTTOM-left in window coordinates, and rows are
 * returned bottom-first, matching the specification and matching how
 * glTexImage2D expects to receive them.  So a glReadPixels straight into a
 * glTexImage2D round-trips without a flip, which is what applications
 * actually do with it.
 * ==========================================================================*/

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, GLvoid *pixels) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (width < 0 || height < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!pixels) { gl_set_error(GL_INVALID_VALUE); return; }
    if (type != GL_UNSIGNED_BYTE) { gl_set_error(GL_INVALID_ENUM); return; }
    if (width == 0 || height == 0) return;

    int comps;
    switch (format) {
    case GL_RGB:  case GL_BGR:  comps = 3; break;
    case GL_RGBA: case GL_BGRA: comps = 4; break;
    case GL_ALPHA:              comps = 1; break;
    case GL_DEPTH_COMPONENT:    comps = 1; break;
    default: gl_set_error(GL_INVALID_ENUM); return;
    }

    unsigned char *dst = (unsigned char *)pixels;

    if (format == GL_DEPTH_COMPONENT) {
        /* Depth as unsigned bytes: [0,1] scaled to [0,255].  Coarse, but it
         * is what GL_UNSIGNED_BYTE asks for, and it is enough to verify that
         * a depth attachment received what was expected. */
        for (GLsizei row = 0; row < height; row++) {
            int sy = y + row;
            for (GLsizei col = 0; col < width; col++) {
                int sx = x + col;
                unsigned char v = 0;
                if (ctx->depth && sx >= 0 && sy >= 0 &&
                    sx < ctx->width && sy < ctx->height) {
                    const float *drow = gl_depth_row(ctx, sy);
                    float d = drow[sx];
                    if (d < 0.0f) d = 0.0f;
                    if (d > 1.0f) d = 1.0f;
                    v = (unsigned char)(d * 255.0f + 0.5f);
                }
                dst[((size_t)row * width + col)] = v;
            }
        }
        return;
    }

    for (GLsizei row = 0; row < height; row++) {
        int sy = y + row;
        for (GLsizei col = 0; col < width; col++) {
            int sx = x + col;
            unsigned char *p = dst + ((size_t)row * width + col) * comps;

            /* Pixels outside the framebuffer have undefined values in GL
             * (§4.3.2).  Zero is defined, cheap and never a surprise. */
            uint32_t v = 0;
            if (ctx->color && sx >= 0 && sy >= 0 &&
                sx < ctx->width && sy < ctx->height) {
                v = gl_fb_row(ctx, sy)[sx];
            }
            unsigned char r = (unsigned char)((v >> 16) & 0xFF);
            unsigned char g = (unsigned char)((v >>  8) & 0xFF);
            unsigned char b = (unsigned char)( v        & 0xFF);
            /* The framebuffer has no alpha channel; reads report opaque,
             * which is what the rendered image actually is. */
            unsigned char a = 0xFF;

            switch (format) {
            case GL_RGB:  p[0] = r; p[1] = g; p[2] = b; break;
            case GL_BGR:  p[0] = b; p[1] = g; p[2] = r; break;
            case GL_RGBA: p[0] = r; p[1] = g; p[2] = b; p[3] = a; break;
            case GL_BGRA: p[0] = b; p[1] = g; p[2] = r; p[3] = a; break;
            case GL_ALPHA: p[0] = a; break;
            default: break;
            }
        }
    }
}

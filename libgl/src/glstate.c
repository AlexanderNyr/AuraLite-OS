/* libgl/src/glstate.c — GL state machine: errors, clearing, viewport.
 *
 * Phase G1 of GL_PLAN.md.  Implements the parts of the state machine needed
 * to produce a first frame; matrix state arrives in G2 and the per-fragment
 * tests in G3.
 */

#include <string.h>

#include "GL/gl.h"
#include "GL/auraglx.h"
#include "glcontext.h"
#include "GL/glbackend.h"
#include "glvertex.h"

/* ============================================================================
 * Errors (§2.5)
 * ==========================================================================*/

GLenum glGetError(void) {
    struct aglx_context *ctx = gl_current_ctx;

    /* Calling glGetError() without a context is itself a misuse, but there is
     * no context to record it in.  Reporting GL_INVALID_OPERATION is the most
     * informative answer available. */
    if (!ctx) return GL_INVALID_OPERATION;

    GLenum e = ctx->error;
    ctx->error = GL_NO_ERROR;   /* reading clears (§2.5) */
    return e;
}

/* ============================================================================
 * Strings (§6.1.11)
 * ==========================================================================*/

const GLubyte *glGetString(GLenum name) {
    switch (name) {
    case GL_VENDOR:
        return (const GLubyte *)"AuraLite OS";
    case GL_RENDERER: {
        /* Reports the ACTIVE backend, so an application can tell whether it
         * is on the software or hardware path (phase G9). */
        const gl_backend_info_t *bi = gl_backend_info();
        return (const GLubyte *)(bi && bi->name ? bi->name
                                                : "AuraLite Software Rasterizer");
    }
    case GL_VERSION:
        return (const GLubyte *)"1.1 AuraLite";
    case GL_EXTENSIONS:
        /* No extensions yet.  The empty string is the correct answer here,
         * not NULL: applications tokenise this and NULL would crash them. */
        return (const GLubyte *)"";
    default:
        gl_set_error(GL_INVALID_ENUM);
        return (const GLubyte *)0;
    }
}

/* ============================================================================
 * Clear state (§4.2.3)
 * ==========================================================================*/

void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    /* GLclampf arguments are clamped when specified, not when used. */
    ctx->clear_color.r = gl_clampf(r);
    ctx->clear_color.g = gl_clampf(g);
    ctx->clear_color.b = gl_clampf(b);
    ctx->clear_color.a = gl_clampf(a);
}

void glClearDepth(GLclampd depth) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    ctx->clear_depth = gl_clampf((GLfloat)depth);
}

void glClear(GLbitfield mask) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    /* Any bit outside the defined set is GL_INVALID_VALUE, and when that
     * happens nothing is cleared at all (§4.2.3). */
    GLbitfield valid = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                       GL_STENCIL_BUFFER_BIT;
    if (mask & ~valid) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }

    /* Clearing an incomplete framebuffer object is
     * GL_INVALID_FRAMEBUFFER_OPERATION (§4.4.4).  Without this the clear
     * would dereference the NULL target that gl_fbo_apply() installs. */
    if (!gl_fbo_target_ok(ctx)) {
        gl_set_error(GL_INVALID_FRAMEBUFFER_OPERATION);
        return;
    }

    /* The backend may clear in hardware; a non-zero return means it did not. */
    if (gl_backend_try_clear(ctx, mask) == 0) return;

    size_t pixels = (size_t)ctx->width * (size_t)ctx->height;

    if (mask & GL_COLOR_BUFFER_BIT) {
        gl_pixel_t c = gl_pack_color(ctx->clear_color);
        gl_pixel_t *p = ctx->color;

        if (c == 0) {
            /* Clearing to black is by far the most common case and memset is
             * substantially faster than a manual loop. */
            memset(p, 0, pixels * sizeof(gl_pixel_t));
        } else {
            /* Unrolled by 8: the clear touches every pixel every frame, so it
             * is one of the few places where this is worth the extra code. */
            size_t i = 0;
            size_t n8 = pixels & ~(size_t)7;
            for (; i < n8; i += 8) {
                p[i+0] = c; p[i+1] = c; p[i+2] = c; p[i+3] = c;
                p[i+4] = c; p[i+5] = c; p[i+6] = c; p[i+7] = c;
            }
            for (; i < pixels; i++) p[i] = c;
        }
    }

    if (mask & GL_DEPTH_BUFFER_BIT) {
        /* Silently ignored when the context has no depth buffer, exactly as a
         * GL implementation with a 0-bit depth buffer behaves. */
        if (ctx->depth) {
            float d = ctx->clear_depth;
            float *p = ctx->depth;
            size_t i = 0;
            size_t n8 = pixels & ~(size_t)7;
            for (; i < n8; i += 8) {
                p[i+0] = d; p[i+1] = d; p[i+2] = d; p[i+3] = d;
                p[i+4] = d; p[i+5] = d; p[i+6] = d; p[i+7] = d;
            }
            for (; i < pixels; i++) p[i] = d;
        }
    }

    /* GL_STENCIL_BUFFER_BIT is accepted but has no effect: AuraLite has no
     * stencil buffer yet.  Per the specification, clearing a buffer that does
     * not exist is a no-op rather than an error. */
}

/* ============================================================================
 * Viewport (§2.11)
 * ==========================================================================*/

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (width < 0 || height < 0) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }

    ctx->viewport_x = x;
    ctx->viewport_y = y;
    ctx->viewport_w = width;
    ctx->viewport_h = height;
}

/* ============================================================================
 * Shading model (§2.14.7)
 * ==========================================================================*/

void glShadeModel(GLenum mode) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (mode != GL_FLAT && mode != GL_SMOOTH) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    ctx->shade_model = mode;
}

/* ============================================================================
 * Enable / disable (§2.5, §6.1.1)
 * ==========================================================================*/

/* Resolve a capability to its GLboolean slot in the context, or NULL when the
 * enum is not a capability this implementation knows.
 *
 * Capabilities that exist in the GL 1.1 enum space but have no implementation
 * yet (lighting, texturing, blending, fog) deliberately return NULL here so
 * glEnable() reports GL_INVALID_ENUM.  Silently accepting them would let an
 * application believe lighting was on and then wonder why nothing changed;
 * they become real slots in G5/G6. */
static GLboolean *cap_slot(struct aglx_context *ctx, GLenum cap) {
    /* GL_LIGHT0..GL_LIGHT7 are a contiguous range, handled before the switch
     * so the eight cases do not have to be spelled out. */
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + GL_MAX_LIGHTS_IMPL) {
        return &ctx->lights[cap - GL_LIGHT0].enabled;
    }
    switch (cap) {
    case GL_DEPTH_TEST:     return &ctx->depth_test;
    case GL_CULL_FACE:      return &ctx->cull_face;
    case GL_SCISSOR_TEST:   return &ctx->scissor_test;
    case GL_LIGHTING:       return &ctx->lighting;
    case GL_NORMALIZE:      return &ctx->normalize;
    case GL_COLOR_MATERIAL: return &ctx->color_material;
    /* The texture enables are PER UNIT (GL 1.3 §3.8.15): glEnable(GL_TEXTURE_2D)
     * affects whichever unit glActiveTexture last selected, not a global. */
    case GL_TEXTURE_2D:
        return &ctx->texunits[ctx->active_texture].enabled_2d;
    case GL_TEXTURE_3D:
        return &ctx->texunits[ctx->active_texture].enabled_3d;
    case GL_TEXTURE_CUBE_MAP:
        return &ctx->texunits[ctx->active_texture].enabled_cube;
    case GL_BLEND:          return &ctx->blend;
    case GL_ALPHA_TEST:     return &ctx->alpha_test;
    case GL_FOG:            return &ctx->fog;
    default:                return (GLboolean *)0;
    }
}

void glEnable(GLenum cap) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    {
        GLint iv[1]; iv[0] = (GLint)cap;
        if (gl_list_record(ctx, gl_lop_enable(), (const GLfloat *)0, 0, iv, 1))
            return;
    }
    GLboolean *slot = cap_slot(ctx, cap);
    if (!slot) { gl_set_error(GL_INVALID_ENUM); return; }
    *slot = GL_TRUE;
}

void glDisable(GLenum cap) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    {
        GLint iv[1]; iv[0] = (GLint)cap;
        if (gl_list_record(ctx, gl_lop_disable(), (const GLfloat *)0, 0, iv, 1))
            return;
    }
    GLboolean *slot = cap_slot(ctx, cap);
    if (!slot) { gl_set_error(GL_INVALID_ENUM); return; }
    *slot = GL_FALSE;
}

GLboolean glIsEnabled(GLenum cap) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return GL_FALSE;
    GLboolean *slot = cap_slot(ctx, cap);
    if (!slot) { gl_set_error(GL_INVALID_ENUM); return GL_FALSE; }
    return *slot;
}

/* ============================================================================
 * Depth buffer state (§4.1.5)
 * ==========================================================================*/

void glDepthFunc(GLenum func) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    switch (func) {
    case GL_NEVER: case GL_LESS: case GL_EQUAL: case GL_LEQUAL:
    case GL_GREATER: case GL_NOTEQUAL: case GL_GEQUAL: case GL_ALWAYS:
        ctx->depth_func = func;
        return;
    default:
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
}

void glDepthMask(GLboolean flag) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    /* Any non-zero value means GL_TRUE (§2.1.2). */
    ctx->depth_mask = flag ? GL_TRUE : GL_FALSE;
}

/* ============================================================================
 * Polygon state (§3.5)
 * ==========================================================================*/

void glCullFace(GLenum mode) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (mode != GL_FRONT && mode != GL_BACK && mode != GL_FRONT_AND_BACK) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    ctx->cull_mode = mode;
}

void glFrontFace(GLenum mode) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (mode != GL_CW && mode != GL_CCW) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    ctx->front_face = mode;
}

void glPolygonMode(GLenum face, GLenum mode) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    if (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    /* Separate front/back modes are not tracked: AuraLite applies one mode to
     * both faces.  Accepting GL_FRONT or GL_BACK and applying it to both is
     * the pragmatic behaviour here — the alternative (rejecting them) would
     * break the very common glPolygonMode(GL_FRONT_AND_BACK, ...) idiom's
     * single-face cousins for no benefit. */
    ctx->polygon_mode = mode;
}

/* ============================================================================
 * Scissor test (§4.1.2)
 * ==========================================================================*/

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (width < 0 || height < 0) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    ctx->scissor_x = x;
    ctx->scissor_y = y;
    ctx->scissor_w = width;
    ctx->scissor_h = height;
}

/* ============================================================================
 * State queries (§6.1)
 * ==========================================================================*/

void glGetIntegerv(GLenum pname, GLint *params) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!params) { gl_set_error(GL_INVALID_VALUE); return; }

    switch (pname) {
    case GL_VIEWPORT:
        params[0] = ctx->viewport_x; params[1] = ctx->viewport_y;
        params[2] = (GLint)ctx->viewport_w; params[3] = (GLint)ctx->viewport_h;
        break;
    case GL_SCISSOR_BOX:
        params[0] = ctx->scissor_x; params[1] = ctx->scissor_y;
        params[2] = (GLint)ctx->scissor_w; params[3] = (GLint)ctx->scissor_h;
        break;
    case GL_MATRIX_MODE:   params[0] = (GLint)ctx->matrix_mode;  break;
    case GL_DEPTH_FUNC:    params[0] = (GLint)ctx->depth_func;   break;
    case GL_CULL_FACE_MODE:params[0] = (GLint)ctx->cull_mode;    break;
    case GL_FRONT_FACE:    params[0] = (GLint)ctx->front_face;   break;
    case GL_SHADE_MODEL:   params[0] = (GLint)ctx->shade_model;  break;
    case GL_MAX_LIGHTS:    params[0] = GL_MAX_LIGHTS_IMPL; break;
    case GL_TEXTURE_BINDING_2D:
        params[0] = (GLint)ctx->texunits[ctx->active_texture].binding_2d; break;
    case GL_TEXTURE_BINDING_3D:
        params[0] = (GLint)ctx->texunits[ctx->active_texture].binding_3d; break;
    case GL_TEXTURE_BINDING_CUBE_MAP:
        params[0] = (GLint)ctx->texunits[ctx->active_texture].binding_cube; break;
    case GL_ACTIVE_TEXTURE:
        params[0] = (GLint)(GL_TEXTURE0 + ctx->active_texture); break;
    case GL_CLIENT_ACTIVE_TEXTURE:
        params[0] = (GLint)(GL_TEXTURE0 + ctx->client_active_texture); break;
    case GL_MAX_TEXTURE_UNITS:
        params[0] = GL_MAX_TEXTURE_UNITS_IMPL; break;
    case GL_FRAMEBUFFER_BINDING:
        params[0] = (GLint)ctx->framebuffer_binding; break;
    case GL_RENDERBUFFER_BINDING:
        params[0] = (GLint)ctx->renderbuffer_binding; break;
    case GL_CURRENT_PROGRAM:
        params[0] = (GLint)ctx->program_binding; break;
    case GL_MAX_VERTEX_ATTRIBS:
        params[0] = GL_MAX_VERTEX_ATTRIBS_IMPL; break;
    case GL_MAX_VARYING_VECTORS:
        params[0] = GL_MAX_VARYING_VEC4S; break;
    case GL_MAX_VERTEX_UNIFORM_VECTORS:
    case GL_MAX_FRAGMENT_UNIFORM_VECTORS:
        params[0] = GL_MAX_UNIFORM_FLOATS / 4; break;
    case GL_MAX_COLOR_ATTACHMENTS:
        params[0] = GL_MAX_COLOR_ATTACHMENTS_IMPL; break;
    case GL_MAX_RENDERBUFFER_SIZE:
        params[0] = AGLX_MAX_DIM; break;
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE:
        params[0] = 8192; break;
    case GL_BLEND_SRC:     params[0] = (GLint)ctx->blend_src;  break;
    case GL_BLEND_DST:     params[0] = (GLint)ctx->blend_dst;  break;
    case GL_ALPHA_TEST_FUNC: params[0] = (GLint)ctx->alpha_func; break;
    case GL_FOG_MODE:      params[0] = (GLint)ctx->fog_mode;   break;
    case GL_MAX_MODELVIEW_STACK_DEPTH:
        params[0] = GL_MODELVIEW_STACK_DEPTH;  break;
    case GL_MAX_PROJECTION_STACK_DEPTH:
        params[0] = GL_PROJECTION_STACK_DEPTH; break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
}

void glGetFloatv(GLenum pname, GLfloat *params) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!params) { gl_set_error(GL_INVALID_VALUE); return; }

    switch (pname) {
    case GL_COLOR_CLEAR_VALUE:
        params[0] = ctx->clear_color.r; params[1] = ctx->clear_color.g;
        params[2] = ctx->clear_color.b; params[3] = ctx->clear_color.a;
        break;
    case GL_DEPTH_CLEAR_VALUE:
        params[0] = ctx->clear_depth;
        break;
    case GL_MODELVIEW_MATRIX:
        for (int i = 0; i < 16; i++)
            params[i] = ctx->modelview[ctx->modelview_top].m[i];
        break;
    case GL_PROJECTION_MATRIX:
        for (int i = 0; i < 16; i++)
            params[i] = ctx->projection[ctx->projection_top].m[i];
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
}

void glGetBooleanv(GLenum pname, GLboolean *params) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!params) { gl_set_error(GL_INVALID_VALUE); return; }

    switch (pname) {
    case GL_DEPTH_TEST:      params[0] = ctx->depth_test;   break;
    case GL_DEPTH_WRITEMASK: params[0] = ctx->depth_mask;   break;
    case GL_CULL_FACE:       params[0] = ctx->cull_face;    break;
    case GL_SCISSOR_TEST:    params[0] = ctx->scissor_test; break;
    case GL_LIGHTING:        params[0] = ctx->lighting;     break;
    case GL_NORMALIZE:       params[0] = ctx->normalize;    break;
    case GL_COLOR_MATERIAL:  params[0] = ctx->color_material; break;
    case GL_TEXTURE_2D:
        params[0] = ctx->texunits[ctx->active_texture].enabled_2d; break;
    case GL_TEXTURE_3D:
        params[0] = ctx->texunits[ctx->active_texture].enabled_3d; break;
    case GL_TEXTURE_CUBE_MAP:
        params[0] = ctx->texunits[ctx->active_texture].enabled_cube; break;
    case GL_BLEND:           params[0] = ctx->blend;          break;
    case GL_ALPHA_TEST:      params[0] = ctx->alpha_test;     break;
    case GL_FOG:             params[0] = ctx->fog;            break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
}

/* ============================================================================
 * Attribute stack (§6.1.2)
 *
 * glPushAttrib saves groups of server state selected by a bitmask; glPopAttrib
 * restores exactly the groups that the matching push saved.  The mask is
 * therefore stored with the entry rather than re-supplied at pop time.
 *
 * AuraLite tracks the groups it actually implements.  Bits for state that does
 * not exist yet (lighting, texturing, fog) are accepted and ignored rather
 * than rejected: a push/pop pair around code that sets them must still work,
 * and rejecting the mask would break the extremely common
 * glPushAttrib(GL_ALL_ATTRIB_BITS) idiom.
 * ==========================================================================*/

void glPushAttrib(GLbitfield mask) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (ctx->attrib_top >= GL_ATTRIB_STACK_DEPTH) {
        /* Overflow leaves the stack and the current state untouched. */
        gl_set_error(GL_STACK_OVERFLOW);
        return;
    }

    struct gl_attrib_entry *e = &ctx->attrib_stack[ctx->attrib_top];
    e->mask = mask;

    /* Everything is copied unconditionally; the mask decides what is restored.
     * Copying a handful of scalars is cheaper than branching per group. */
    e->clear_color  = ctx->clear_color;
    e->clear_depth  = ctx->clear_depth;
    e->viewport_x   = ctx->viewport_x;
    e->viewport_y   = ctx->viewport_y;
    e->viewport_w   = ctx->viewport_w;
    e->viewport_h   = ctx->viewport_h;
    e->depth_test   = ctx->depth_test;
    e->depth_mask   = ctx->depth_mask;
    e->depth_func   = ctx->depth_func;
    e->cull_face    = ctx->cull_face;
    e->cull_mode    = ctx->cull_mode;
    e->front_face   = ctx->front_face;
    e->shade_model  = ctx->shade_model;
    e->polygon_mode = ctx->polygon_mode;
    e->scissor_test = ctx->scissor_test;
    e->scissor_x    = ctx->scissor_x;
    e->scissor_y    = ctx->scissor_y;
    e->scissor_w    = ctx->scissor_w;
    e->scissor_h    = ctx->scissor_h;

    ctx->attrib_top++;
}

void glPopAttrib(void) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (ctx->attrib_top == 0) {
        gl_set_error(GL_STACK_UNDERFLOW);
        return;
    }

    ctx->attrib_top--;
    const struct gl_attrib_entry *e = &ctx->attrib_stack[ctx->attrib_top];
    GLbitfield m = e->mask;

    if (m & GL_ENABLE_BIT) {
        ctx->depth_test   = e->depth_test;
        ctx->cull_face    = e->cull_face;
        ctx->scissor_test = e->scissor_test;
    }
    if (m & GL_DEPTH_BUFFER_BIT) {
        ctx->depth_test  = e->depth_test;
        ctx->depth_mask  = e->depth_mask;
        ctx->depth_func  = e->depth_func;
        ctx->clear_depth = e->clear_depth;
    }
    if (m & GL_VIEWPORT_BIT) {
        ctx->viewport_x = e->viewport_x;
        ctx->viewport_y = e->viewport_y;
        ctx->viewport_w = e->viewport_w;
        ctx->viewport_h = e->viewport_h;
    }
    if (m & GL_SCISSOR_BIT) {
        ctx->scissor_test = e->scissor_test;
        ctx->scissor_x    = e->scissor_x;
        ctx->scissor_y    = e->scissor_y;
        ctx->scissor_w    = e->scissor_w;
        ctx->scissor_h    = e->scissor_h;
    }
    if (m & GL_POLYGON_BIT) {
        ctx->cull_face    = e->cull_face;
        ctx->cull_mode    = e->cull_mode;
        ctx->front_face   = e->front_face;
        ctx->polygon_mode = e->polygon_mode;
    }
    if (m & GL_LIGHTING_BIT) {
        ctx->shade_model = e->shade_model;
    }
    if (m & GL_COLOR_BUFFER_BIT) {
        ctx->clear_color = e->clear_color;
    }
}

/* ============================================================================
 * Synchronisation (§5.4)
 * ==========================================================================*/

void glFlush(void) {
    /* A software rasterizer has no command queue: drawing has already
     * happened by the time the entry point returns.  Validating the context
     * still gives applications the expected GL_INVALID_OPERATION when they
     * call GL with nothing current. */
    (void)gl_ctx_or_error();
}

void glFinish(void) {
    /* Same reasoning as glFlush(): nothing is ever outstanding. */
    (void)gl_ctx_or_error();
}

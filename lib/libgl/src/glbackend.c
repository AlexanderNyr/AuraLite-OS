/* libgl/src/glbackend.c — backend registry and the software backend.
 *
 * Phase G9 of GL_PLAN.md.  See GL/glbackend.h for the rationale.
 *
 * The registry deliberately holds only pointers to caller-owned tables, the
 * same contract kernel/net/netdev.h uses: a backend is a static const struct
 * in whichever translation unit implements it, so registration cannot fail on
 * allocation and there is nothing to free.
 */

#include <string.h>

#include "GL/gl.h"
#include "GL/glbackend.h"
#include "glcontext.h"
#include "glvertex.h"

/* H5: was 4.  virgl + software are registered at context creation, and a
 * test suite adds fake/minimal/H5 backends on top; at 4 the registry filled
 * silently and a later register() was dropped without a word -- which is how
 * a green suite can go red purely because a test moved.  8 is still tiny
 * (an array of pointers) and leaves room for the test doubles. */
#define GL_MAX_BACKENDS 8

static const gl_backend_t *backends[GL_MAX_BACKENDS];
static int                 backend_count;
static const gl_backend_t *active;
static gl_backend_info_t   active_info;
static const char         *forced_name;

/* ============================================================================
 * The software backend
 *
 * This is not a separate renderer: every operation returns "not handled" so
 * libgl runs its normal CPU path.  Modelling the default as a backend rather
 * than as a special case keeps the dispatch in one shape — there is no
 * "if (backend) ... else ..." scattered through the code, because there is
 * always a backend.
 * ==========================================================================*/

static int software_init(void) {
    return 0;                    /* always available */
}

static int software_clear(struct aglx_context *ctx, GLbitfield mask) {
    (void)ctx; (void)mask;
    return -1;                   /* fall through to the software path */
}

static int software_present(struct aglx_context *ctx) {
    (void)ctx;
    return -1;
}

static void software_destroy(struct aglx_context *ctx) {
    (void)ctx;
}

static const gl_backend_t software_backend = {
    "AuraLite Software Rasterizer",
    GL_BACKEND_SOFTWARE | GL_BACKEND_DEPTH | GL_BACKEND_TEXTURE,
    software_init,
    software_clear,
    software_present,
    0,                  /* draw: the software rasterizer IS the fallback */
    0,                  /* tex_image: textures are already in software    */
    software_destroy,
};

/* ============================================================================
 * Registry
 * ==========================================================================*/

static void publish_info(const gl_backend_t *b) {
    active_info.name     = b->name;
    active_info.flags    = b->flags;
    active_info.hardware = (b->flags & GL_BACKEND_HARDWARE) ? 1 : 0;
}

/* Choose the backend to use.  Called lazily on first use so that a hardware
 * backend registering during application start-up still wins, without libgl
 * needing an explicit initialisation call. */
static void ensure_active(void) {
    if (active) return;

    /* An explicit choice overrides everything, including registration order.
     * Used by the tests and by a debug switch in the demos. */
    if (forced_name) {
        for (int i = 0; i < backend_count; i++) {
            if (strcmp(backends[i]->name, forced_name) == 0) {
                if (!backends[i]->init || backends[i]->init() == 0) {
                    active = backends[i];
                    publish_info(active);
                    return;
                }
                break;           /* named backend declined: fall through */
            }
        }
    }

    /* Otherwise the first registered backend that accepts wins.  A backend may
     * decline in init() — a VirGL backend on a machine with no virtio-gpu
     * should do exactly that rather than failing every draw call later. */
    for (int i = 0; i < backend_count; i++) {
        if (backends[i] == &software_backend) continue;   /* keep as fallback */
        if (!backends[i]->init || backends[i]->init() == 0) {
            active = backends[i];
            publish_info(active);
            return;
        }
    }

    active = &software_backend;
    publish_info(active);
}

void gl_backend_register(const gl_backend_t *backend) {
    if (!backend || !backend->name) return;
    if (backend_count >= GL_MAX_BACKENDS) return;

    for (int i = 0; i < backend_count; i++) {
        if (backends[i] == backend) return;      /* already registered */
    }
    backends[backend_count++] = backend;

    /* Registering after a backend is already active does not displace it: a
     * context may already hold resources belonging to the current one. */
}

const gl_backend_t *gl_backend_active(void) {
    ensure_active();
    return active;
}

const gl_backend_info_t *gl_backend_info(void) {
    ensure_active();
    return &active_info;
}

int gl_backend_force(const char *name) {
    if (!name) {
        forced_name = (const char *)0;
        active = (const gl_backend_t *)0;         /* re-select on next use */
        return 0;
    }
    for (int i = 0; i < backend_count; i++) {
        if (strcmp(backends[i]->name, name) == 0) {
            forced_name = backends[i]->name;      /* point at stable storage */
            active = (const gl_backend_t *)0;
            return 0;
        }
    }
    return -1;
}

/* Register the software backend.  Called from context creation so the registry
 * is never empty, and late enough that a hardware backend registered by the
 * application still gets first refusal. */
void gl_backend_init_defaults(void) {
    gl_backend_register(&software_backend);
}

/* ============================================================================
 * Dispatch helpers used by libgl
 *
 * Each tries the backend and reports whether it handled the operation, so the
 * caller can run its software path when the answer is no.  Keeping the
 * fallback decision here rather than at each call site means a backend can
 * implement operations one at a time.
 * ==========================================================================*/

int gl_backend_try_clear(struct aglx_context *ctx, GLbitfield mask) {
    const gl_backend_t *b = gl_backend_active();
    if (!b || !b->clear) return -1;
    return b->clear(ctx, mask);
}

int gl_backend_try_present(struct aglx_context *ctx) {
    const gl_backend_t *b = gl_backend_active();
    if (!b || !b->present) return -1;
    return b->present(ctx);
}

/* GL2 L6: hand one assembled batch to the backend's draw hook.  Returning
 * non-zero (no hook, or the hook declined) is the whole-draw fallback. */
int gl_backend_try_draw(struct aglx_context *ctx,
                        const gl_draw_batch_t *batch) {
    const gl_backend_t *b = gl_backend_active();
    if (!b || !b->draw || !batch || batch->count < 3) return -1;
    return b->draw(ctx, batch);
}

/* Whether the canned hardware path may take this draw AT ALL.
 *
 * The canned pipeline is a fixed-function triangle feeder: one vertex format
 * (clip position + colour), no textures, no tests, no blending, no
 * reprojection.  Anything this list does not name would render differently
 * on the GPU than the software rasterizer would have — and "different but
 * plausible" is the one outcome this seam must never produce.  Lighting and
 * normals are deliberately absent from the list: they bake into the vertex
 * colour before this point, so they change nothing here.
 *
 * Note there is no matrix condition: the batch carries clip coordinates the
 * fixed-function matrices already produced, so the GPU divides and clips
 * exactly what the CPU path would have. */
/* The batch format libgl should pack for the active backend.  H5's rule:
 * a backend gets the widened format only if it declared GL_BACKEND_DRAW_FULL,
 * so every other backend keeps receiving format 0 byte for byte. */
unsigned gl_backend_draw_format(struct aglx_context *ctx) {
    (void)ctx;
    const gl_backend_t *b = gl_backend_active();
    if (!b || !b->draw) return GL_BATCH_FMT_POS_COLOR;
    return (b->flags & GL_BACKEND_DRAW_FULL) ? GL_BATCH_FMT_FULL
                                             : GL_BATCH_FMT_POS_COLOR;
}

/* Can the GPU reproduce this blend state exactly?
 *
 * Only the GL factors with a 1:1 gallium equivalent, and only the pairs this
 * path uploads.  This libgl has no glBlendEquation (§4.1.7's equation state
 * was never implemented), so GL_FUNC_ADD is what BOTH paths compute -- the
 * agreement is by construction, not by promise.  Anything outside the set
 * declines; a silently substituted factor is a frame that looks plausible
 * and is wrong, which is the one outcome the fallback design exists to
 * prevent. */
static int blend_is_gpu_exact(const struct aglx_context *ctx) {
    switch (ctx->blend_src) {
    case GL_ZERO: case GL_ONE: case GL_SRC_ALPHA: case GL_ONE_MINUS_SRC_ALPHA:
        break;
    default: return 0;
    }
    switch (ctx->blend_dst) {
    case GL_ZERO: case GL_ONE: case GL_SRC_ALPHA: case GL_ONE_MINUS_SRC_ALPHA:
        break;
    default: return 0;
    }
    return 1;
}

/* Whether the hardware path may take this draw AT ALL.
 *
 * H5 shrank this screen from thirteen refusals to nine.  The plan's rule for
 * a removal was that it "carries the test that would have failed before it",
 * so here is the ledger, one entry per removal, naming what makes it safe:
 *
 *   depth_test   removed.  Only when the backend declares GL_BACKEND_DEPTH
 *                (the VirGL backend creates its DSA with the test on and
 *                attaches a zsurf), and only for the exact comparison the
 *                GPU is set to: GL_LESS with writes enabled.  Any other
 *                depth_func declines -- the DSA is uploaded with LESS.
 *   blend        removed.  Only factor pairs blend_is_gpu_exact accepts.
 *   cull_face    removed.  The rasterizer object carries the face; GL_CCW
 *                winding only, because that is the one the VELEM order
 *                produces and a flip would cull the wrong side.
 *   texture      removed.  One unit, 2D only, and only when the backend
 *                declares GL_BACKEND_TEXTURE.  A second unit, a 3D or cube
 *                map, or a non-MODULATE env mode still declines.
 *
 * The other nine are untouched: non-triangles, partial triangles, immediate-
 * mode positions, an active shader, stencil, alpha test, scissor, polygon
 * mode and fog.  FOG in particular cannot move -- it is a fragment-stage
 * operation the canned shader does not perform, so dropping the refusal
 * would fog nothing and change every pixel silently.
 *
 * Every removal is gated on the BACKEND, never on a global: a backend that
 * does not promise depth/blend/texture keeps the strict screen, so this
 * widening cannot loosen it for a backend that cannot honour it. */
/* The four states H5 earned back, screened in their own function.
 *
 * Splitting them out is not cosmetic: it keeps the OUTER screen at the nine
 * unconditional refusals the plan counts, and puts the four conditional ones
 * where their extra conditions (GL_LESS rather than any depth func, unit 0
 * rather than any unit) can be read together.  The count the plan promises is
 * a count of STATES, and a state that is allowed under A and refused under B
 * still reads as one line here.
 *
 * Every condition below is a real asymmetry between what the GPU will do and
 * what the software path does; none is defensive padding. */
static int h5_state_is_reproducible(const struct aglx_context *ctx,
                                    const gl_backend_t *b) {
    if (ctx->depth_test) {
        /* The DSA is uploaded with PIPE_FUNC_LESS and the mask on.  Any other
         * function or a disabled write would compare differently. */
        if ((b->flags & GL_BACKEND_DEPTH) == 0) return 0;
        if (ctx->depth_func != GL_LESS) return 0;
        if (!ctx->depth_mask) return 0;
    }
    /* Only the 1:1 factor set; everything else is a blend the canned
     * pipeline cannot express. */
    if (ctx->blend && !blend_is_gpu_exact(ctx)) return 0;
    /* The VELEM order makes CCW the front face; a flip would cull the other
     * side. */
    if (ctx->cull_face && ctx->front_face != GL_CCW) return 0;

    /* One 2D unit, unit 0, default combine, and only if the backend says it
     * samples textures at all. */
    int enabled_units = 0;
    for (int u = 0; u < GL_MAX_TEXTURE_UNITS_IMPL; u++) {
        if (ctx->texunits[u].enabled_3d || ctx->texunits[u].enabled_cube)
            return 0;
        if (ctx->texunits[u].enabled_2d) {
            if (u != 0) return 0;
            enabled_units++;
        }
    }
    if (enabled_units) {
        if ((b->flags & GL_BACKEND_TEXTURE) == 0) return 0;
        if (ctx->texunits[0].env_mode != GL_MODULATE) return 0;
    }
    return 1;
}

/* Whether the hardware path may take this draw AT ALL.
 *
 * Thirteen refusals before H5, NINE after.  The four that left -- depth,
 * blend, cull and texture -- did not disappear; they moved into
 * h5_state_is_reproducible() above, where each is conditional on the backend
 * declaring the capability and on the exact sub-state the GPU is set to.
 * That is the whole shape of the phase: the screen did not get more
 * permissive, it got more precise, and every precision claim carries a test
 * in test_glbackend.c (the -declines- half of each pair).
 *
 * The nine below are unchanged from GL2 L6 and refuse for reasons no amount
 * of backend capability can fix: the canned pipeline draws triangles with
 * positions from an array and no shader, and FOG/ALPHA/STENCIL/SCISSOR are
 * fragment- or raster-stage operations it does not perform at all. */
int gl_backend_draw_eligible(struct aglx_context *ctx, GLenum mode,
                             GLsizei count) {
    if (!ctx) return 0;
    const gl_backend_t *b = gl_backend_active();
    if (!b || !b->draw) return 0;             /* software is the fallback   */

    if (mode != GL_TRIANGLES) return 0;       /* the canned primitive       */
    if (count < 3 || count % 3 != 0) return 0;/* whole triangles only       */
    if (!ctx->array_vertex.enabled) return 0; /* position from an array     */
    if (gl_shader_active(ctx)) return 0;      /* fixed function only        */
    if (ctx->stencil_test) return 0;          /* no stencil surface         */
    if (ctx->alpha_test) return 0;            /* fragment-stage discard     */
    if (ctx->scissor_test) return 0;          /* scissored pixels would leak */
    if (ctx->polygon_mode != GL_FILL) return 0;
    if (ctx->fog) return 0;                   /* a fragment-stage fixup     */

    return h5_state_is_reproducible(ctx, b);
}

/* GPU_ACCEL_PLAN H3: offer one changed mip level to the backend.
 *
 * Called by gltexture.c after the SOFTWARE image is already updated, so the
 * software sampler is authoritative whatever happens here: a refusal means
 * "do not ask me about this texture again", never "the texture is gone".
 * A backend that returns 0 has mirrored the level on the device and promises
 * the pixels it holds match libgl's.
 *
 * The `last_level` argument is the chain length AFTER this call, not before:
 * glTexImage2D level 0 drops the levels below it, and a backend sizing a
 * chain resource needs the post-call truth. */
int gl_backend_try_tex_image(struct aglx_context *ctx, GLuint name,
                             GLenum target, int face, int level,
                             GLsizei width, GLsizei height,
                             const uint32_t *data, int last_level) {
    const gl_backend_t *b = gl_backend_active();
    if (!b || !b->tex_image) return -1;
    return b->tex_image(ctx, name, target, face, level, width, height,
                        data, last_level);
}

void gl_backend_notify_destroy(struct aglx_context *ctx) {
    const gl_backend_t *b = gl_backend_active();
    if (b && b->destroy) b->destroy(ctx);
}

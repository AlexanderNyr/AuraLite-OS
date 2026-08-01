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

#define GL_MAX_BACKENDS 4

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

void gl_backend_notify_destroy(struct aglx_context *ctx) {
    const gl_backend_t *b = gl_backend_active();
    if (b && b->destroy) b->destroy(ctx);
}

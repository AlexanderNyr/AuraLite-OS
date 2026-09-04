/*
 * test_glbackend.c — host-side unit tests for the backend seam (phase G9).
 *
 * The point of this suite is to prove the seam actually works, not merely that
 * it compiles.  It registers a FAKE hardware backend and checks that:
 *
 *   - it takes precedence over software when it accepts,
 *   - libgl really routes clear/present through it,
 *   - a backend that implements only some operations gets the software path
 *     for the rest (partial backends must be usable),
 *   - a backend that declines at init() is skipped cleanly,
 *   - glGetString(GL_RENDERER) reports whichever one is active.
 *
 * That last set is what makes the hardware path in G9+ a drop-in rather than a
 * rewrite: a real VirGL backend can be brought up one entry point at a time.
 */

#include <stdio.h>
#include <string.h>

#include "GL/gl.h"
#include "GL/glbackend.h"
#include "GL/auraglx.h"
#include "glcontext.h"
#include "auragui.h"

void gl_imm_reset(void);

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    ag_stub_reset();                                    \
    gl_imm_reset();                                     \
    aglxMakeCurrent(NULL);                              \
    gl_backend_force(NULL);                             \
    fake_reset();                                       \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

#define W 32
#define H 32

/* ---------------------------------------------------------- fake backend -- */

static struct {
    int init_calls, clear_calls, present_calls, draw_calls, destroy_calls;
    int accept_init;      /* 0 => decline at init() */
    int handle_clear;     /* 0 => report "not handled" */
    int handle_present;
} fake;

static void fake_reset(void) {
    memset(&fake, 0, sizeof fake);
    fake.accept_init = 1;
}

static int fake_init(void) {
    fake.init_calls++;
    return fake.accept_init ? 0 : -1;
}

static int fake_clear(struct aglx_context *ctx, GLbitfield mask) {
    (void)mask;
    fake.clear_calls++;
    if (!fake.handle_clear) return -1;
    /* Paint something recognisable so the test can tell this ran instead of
     * the software clear. */
    if (ctx && ctx->color) {
        size_t n = (size_t)ctx->width * (size_t)ctx->height;
        for (size_t i = 0; i < n; i++) ctx->color[i] = 0x00ABCDEF;
    }
    return 0;
}

static int fake_present(struct aglx_context *ctx) {
    (void)ctx;
    fake.present_calls++;
    return fake.handle_present ? 0 : -1;
}

static void fake_destroy(struct aglx_context *ctx) {
    (void)ctx;
    fake.destroy_calls++;
}

static int fake_draw(struct aglx_context *ctx,
                     const gl_draw_batch_t *batch) {
    (void)ctx; (void)batch;
    fake.draw_calls++;                       /* counts, and declines: the
                                              * whole-draw fallback is what
                                              * the seam promises */
    return -1;
}

static const gl_backend_t fake_backend = {
    "Fake Test Backend",
    GL_BACKEND_HARDWARE,
    fake_init, fake_clear, fake_present, fake_draw, fake_destroy,
};

/* A backend with every optional entry point NULL: the minimum legal one. */
static int minimal_init(void) { return 0; }
static const gl_backend_t minimal_backend = {
    "Minimal Backend", GL_BACKEND_HARDWARE,
    minimal_init, (int (*)(struct aglx_context *, GLbitfield))0,
    (int (*)(struct aglx_context *))0,
    (int (*)(struct aglx_context *, const gl_draw_batch_t *))0,
    (void (*)(struct aglx_context *))0,
};

static aglx_context_t *setup(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    if (!c) return NULL;
    aglxMakeCurrent(c);
    return c;
}

/* ------------------------------------------------------------- registry --- */

/* Without any hardware backend, software must be active. */
static int t_software_is_default(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const gl_backend_info_t *bi = gl_backend_info();
    int ok = bi != NULL && bi->name != NULL
          && bi->hardware == 0
          && (bi->flags & GL_BACKEND_SOFTWARE) != 0;
    aglxDestroyContext(c);
    return ok;
}

/* The active backend is never NULL once a context exists. */
static int t_active_never_null(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    int ok = gl_backend_active() != NULL;
    aglxDestroyContext(c);
    return ok;
}

/* glGetString(GL_RENDERER) must report the active backend by name. */
static int t_renderer_string_matches(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    const gl_backend_info_t *bi = gl_backend_info();
    const char *r = (const char *)glGetString(GL_RENDERER);
    int ok = r && bi && bi->name && strcmp(r, bi->name) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* Registering the same table twice must not duplicate it. */
static int t_double_registration_ignored(void) {
    gl_backend_register(&fake_backend);
    gl_backend_register(&fake_backend);
    /* Nothing observable to count directly, but forcing it must still work,
     * which it would not if the slot table had been corrupted. */
    int ok = gl_backend_force("Fake Test Backend") == 0;
    gl_backend_force(NULL);
    return ok;
}

/* Registering NULL, or a table with no name, must be ignored. */
static int t_register_rejects_garbage(void) {
    gl_backend_register((const gl_backend_t *)0);
    static const gl_backend_t nameless = {
        (const char *)0, 0,
        (int (*)(void))0, (int (*)(struct aglx_context *, GLbitfield))0,
        (int (*)(struct aglx_context *))0,
        (int (*)(struct aglx_context *, const gl_draw_batch_t *))0,
        (void (*)(struct aglx_context *))0,
    };
    gl_backend_register(&nameless);
    aglx_context_t *c = setup(); if (!c) return 0;
    int ok = gl_backend_active() != NULL;    /* still sane */
    aglxDestroyContext(c);
    return ok;
}

/* --------------------------------------------------------------- forcing -- */

static int t_force_selects_backend(void) {
    gl_backend_register(&fake_backend);
    int rc = gl_backend_force("Fake Test Backend");

    aglx_context_t *c = setup(); if (!c) { gl_backend_force(NULL); return 0; }
    const gl_backend_info_t *bi = gl_backend_info();
    int ok = rc == 0 && bi && strcmp(bi->name, "Fake Test Backend") == 0
          && bi->hardware == 1
          && fake.init_calls > 0;
    aglxDestroyContext(c);
    gl_backend_force(NULL);
    return ok;
}

static int t_force_unknown_name_fails(void) {
    int ok = gl_backend_force("No Such Backend") != 0;
    gl_backend_force(NULL);
    return ok;
}

/* Forcing NULL returns to automatic selection. */
static int t_force_null_restores_default(void) {
    gl_backend_register(&fake_backend);
    gl_backend_force("Fake Test Backend");
    gl_backend_force(NULL);

    aglx_context_t *c = setup(); if (!c) return 0;
    const gl_backend_info_t *bi = gl_backend_info();
    /* The fake declines nothing, so automatic selection may legitimately pick
     * it; what must hold is that a valid backend is active and reported. */
    int ok = bi && bi->name && gl_backend_active() != NULL;
    aglxDestroyContext(c);
    return ok;
}

/* A backend that declines at init() must be skipped, not left active. */
static int t_declining_backend_is_skipped(void) {
    gl_backend_register(&fake_backend);
    fake.accept_init = 0;                    /* decline */
    gl_backend_force("Fake Test Backend");

    aglx_context_t *c = setup(); if (!c) { gl_backend_force(NULL); return 0; }
    const gl_backend_info_t *bi = gl_backend_info();
    /* init() was attempted, and the result is NOT the declining backend. */
    int ok = fake.init_calls > 0
          && bi && strcmp(bi->name, "Fake Test Backend") != 0;
    aglxDestroyContext(c);
    gl_backend_force(NULL);
    return ok;
}

/* The real VirGL backend declines today, which is the documented behaviour:
 * it must never end up active on a machine with no user-space GPU path.
 *
 * Note this test asserts only that VirGL itself is not selected, not that the
 * software backend is.  The registry is process-global and earlier tests leave
 * their fake backends registered, so automatic selection may legitimately land
 * on one of those; what matters here is that the DECLINING backend is skipped. */
static int t_virgl_declines_today(void) {
    gl_virgl_register();
    int rc = gl_backend_force("AuraLite VirGL (virtio-gpu)");

    aglx_context_t *c = setup(); if (!c) { gl_backend_force(NULL); return 0; }
    const gl_backend_info_t *bi = gl_backend_info();
    /* It is registered (so force found it) but must not be the active one. */
    int ok = rc == 0 && bi && bi->name
          && strcmp(bi->name, "AuraLite VirGL (virtio-gpu)") != 0;
    aglxDestroyContext(c);
    gl_backend_force(NULL);
    return ok;
}

/* -------------------------------------------------------------- dispatch -- */

/* glClear must consult the backend. */
static int t_clear_reaches_backend(void) {
    gl_backend_register(&fake_backend);
    fake.handle_clear = 0;                   /* report "not handled" */
    gl_backend_force("Fake Test Backend");

    aglx_context_t *c = setup(); if (!c) { gl_backend_force(NULL); return 0; }
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    /* The backend was asked, declined, and software did the work. */
    int ok = fake.clear_calls > 0
          && aglxGetColorBuffer(c)[0] == 0x0000FF;
    aglxDestroyContext(c);
    gl_backend_force(NULL);
    return ok;
}

/* When the backend handles the clear, software must NOT also run. */
static int t_backend_clear_wins(void) {
    gl_backend_register(&fake_backend);
    fake.handle_clear = 1;
    gl_backend_force("Fake Test Backend");

    aglx_context_t *c = setup(); if (!c) { gl_backend_force(NULL); return 0; }
    glClearColor(0, 0, 1, 1);                /* software would write 0x0000FF */
    glClear(GL_COLOR_BUFFER_BIT);

    /* The fake's marker survived, so the software clear did not overwrite it. */
    int ok = aglxGetColorBuffer(c)[0] == 0x00ABCDEF;
    aglxDestroyContext(c);
    gl_backend_force(NULL);
    return ok;
}

/* aglxSwapBuffers must consult the backend before blitting. */
static int t_present_reaches_backend(void) {
    gl_backend_register(&fake_backend);
    fake.handle_present = 0;
    gl_backend_force("Fake Test Backend");

    aglx_context_t *c = setup(); if (!c) { gl_backend_force(NULL); return 0; }
    int rc = aglxSwapBuffers(c);

    /* Declined, so the normal ag_blit path ran. */
    int ok = rc == 0 && fake.present_calls > 0 && ag_stub.calls == 1;
    aglxDestroyContext(c);
    gl_backend_force(NULL);
    return ok;
}

/* When the backend presents, the compositor blit must be skipped entirely. */
static int t_backend_present_skips_blit(void) {
    gl_backend_register(&fake_backend);
    fake.handle_present = 1;
    gl_backend_force("Fake Test Backend");

    aglx_context_t *c = setup(); if (!c) { gl_backend_force(NULL); return 0; }
    int rc = aglxSwapBuffers(c);

    int ok = rc == 0 && fake.present_calls == 1 && ag_stub.calls == 0;
    aglxDestroyContext(c);
    gl_backend_force(NULL);
    return ok;
}

/* Destroying a context must notify the backend so it can free its resources. */
static int t_destroy_notifies_backend(void) {
    gl_backend_register(&fake_backend);
    gl_backend_force("Fake Test Backend");

    aglx_context_t *c = setup(); if (!c) { gl_backend_force(NULL); return 0; }
    aglxDestroyContext(c);

    int ok = fake.destroy_calls == 1;
    gl_backend_force(NULL);
    return ok;
}

/* THE key property for incremental bring-up: a backend with NULL entry points
 * must work, with software covering everything it does not implement. */
static int t_partial_backend_falls_back(void) {
    gl_backend_register(&minimal_backend);
    gl_backend_force("Minimal Backend");

    aglx_context_t *c = setup(); if (!c) { gl_backend_force(NULL); return 0; }

    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int cleared = aglxGetColorBuffer(c)[0] == 0xFF0000;

    int rc = aglxSwapBuffers(c);
    int presented = (rc == 0 && ag_stub.calls == 1);

    /* Rendering still works end to end despite the backend implementing
     * nothing at all. */
    int ok = cleared && presented;
    aglxDestroyContext(c);          /* NULL destroy must not be called */
    gl_backend_force(NULL);
    return ok;
}

/* Drawing must still be correct while a non-handling backend is active: the
 * seam must not disturb the rest of the pipeline. */
static int t_rendering_unaffected_by_seam(void) {
    gl_backend_register(&fake_backend);
    fake.handle_clear = 0;
    fake.handle_present = 0;
    gl_backend_force("Fake Test Backend");

    aglx_context_t *c = setup(); if (!c) { gl_backend_force(NULL); return 0; }
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glVertex3f(4, 4, 0); glVertex3f(28, 4, 0); glVertex3f(4, 28, 0);
    glEnd();

    const uint32_t *b = aglxGetColorBuffer(c);
    int lit = 0;
    for (int i = 0; i < W * H; i++) if (b[i]) lit++;
    int ok = lit > 100 && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    gl_backend_force(NULL);
    return ok;
}

/* ------------------------------------------------------------------------ */

int main(void) {
    printf("=== glbackend (G9) unit tests ===\n");

    printf("--- registry ---\n");
    RUN(t_software_is_default); RUN(t_active_never_null);
    RUN(t_renderer_string_matches); RUN(t_double_registration_ignored);
    RUN(t_register_rejects_garbage);

    printf("--- selection ---\n");
    RUN(t_force_selects_backend); RUN(t_force_unknown_name_fails);
    RUN(t_force_null_restores_default);
    RUN(t_declining_backend_is_skipped); RUN(t_virgl_declines_today);

    printf("--- dispatch ---\n");
    RUN(t_clear_reaches_backend); RUN(t_backend_clear_wins);
    RUN(t_present_reaches_backend); RUN(t_backend_present_skips_blit);
    RUN(t_destroy_notifies_backend); RUN(t_partial_backend_falls_back);
    RUN(t_rendering_unaffected_by_seam);

    printf("\ntest_glbackend: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

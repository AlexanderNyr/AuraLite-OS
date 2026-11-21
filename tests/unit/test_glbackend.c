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
#include <stdlib.h>

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

static struct { int draw_calls; unsigned last_format, last_stride_floats; } full;

static void fake_reset(void) {
    memset(&fake, 0, sizeof fake);
    memset(&full, 0, sizeof full);
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
    fake_init, fake_clear, fake_present, fake_draw,
    0,                  /* tex_image: H3 optional slot, unused here */
    fake_destroy,
};

/* A backend with every optional entry point NULL: the minimum legal one. */
static int minimal_init(void) { return 0; }
static const gl_backend_t minimal_backend = {
    "Minimal Backend", GL_BACKEND_HARDWARE,
    minimal_init, (int (*)(struct aglx_context *, GLbitfield))0,
    (int (*)(struct aglx_context *))0,
    (int (*)(struct aglx_context *, const gl_draw_batch_t *))0,
    (int (*)(struct aglx_context *, GLuint, GLenum, int, int,
             GLsizei, GLsizei, const uint32_t *, int))0,
    (void (*)(struct aglx_context *))0,
};

/* H5: a backend that DECLARES the widened draws.  The eligibility table is
 * about what libgl will attempt, so the interesting bit for each test is the
 * backend's flags plus one piece of GL state. */
static int full_draw(struct aglx_context *ctx, const gl_draw_batch_t *batch) {
    (void)ctx;
    full.last_format = batch ? batch->format : 0xFFFFFFFFu;
    full.last_stride_floats = batch ? GL_BATCH_STRIDE(batch->format) : 0;
    full.draw_calls++;
    return -1;                        /* decline: we test the screen, not the
                                       * pixels */
}
static const gl_backend_t full_backend = {
    "Full Test Backend",
    GL_BACKEND_HARDWARE | GL_BACKEND_DEPTH | GL_BACKEND_TEXTURE |
    GL_BACKEND_DRAW_FULL,
    fake_init, fake_clear, fake_present, full_draw,
    0, fake_destroy,
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
        (int (*)(struct aglx_context *, GLuint, GLenum, int, int,
                 GLsizei, GLsizei, const uint32_t *, int))0,
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

/* ============================== H5: the screen ========================== *
 *
 * The plan's gate: "host tests for the new eligibility table (every state the
 * batch cannot reproduce must still decline -- enumerated as a test, not as a
 * hope)".  These run against a backend that DECLARES every H5 capability, so
 * a pass here cannot be an artefact of a weak backend: each of the four
 * removals is followed by a state that still refuses, and each of the nine
 * that stayed is enumerated by name.
 * ========================================================================= */

/* A context with a vertex array bound, so "position from an array" is met and
 * the test is about the state under examination only. */
static int h5_registered;
/* The registry holds four slots.  gl_virgl_register() and the software
 * default take two at context creation; the fake backend takes a third.  That
 * leaves exactly one for H5, so `full_backend` (which declares everything the
 * widened path needs) lives in it and `plain_backend` is exercised through
 * the SAME table with the flags masked -- see h5_plain_force().  Registering
 * both would need a fifth slot, and a silent drop there is how a test turns
 * green for the wrong reason. */
static void h5_register_once(void) {
    if (h5_registered) return;
    gl_backend_register(&full_backend);
    h5_registered = 1;
}

static aglx_context_t *setup_drawable(void) {
    h5_register_once();
    gl_backend_force("Full Test Backend");
    aglx_context_t *c = setup();
    if (!c) return NULL;
    static GLfloat pos[9] = { 2,2,0, 29,2,0, 2,29,0 };
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos);
    return c;
}

/* The baseline draw is eligible; every following test flips one state. */
static int t_h5_baseline_eligible(void) {
    aglx_context_t *c = setup_drawable(); if (!c) return 0;
    int ok = gl_backend_draw_eligible(c, GL_TRIANGLES, 3) == 1;
    aglxDestroyContext(c); gl_backend_force(NULL);
    return ok;
}

/* ---- the four H5 removed: each passes ONLY because the backend says so ---- */

static int t_h5_depth_allowed_when_backend_has_it(void) {
    aglx_context_t *c = setup_drawable(); if (!c) return 0;
    glEnable(GL_DEPTH_TEST);           /* GL_LESS, writes on: the DSA's own */
    int ok = gl_backend_draw_eligible(c, GL_TRIANGLES, 3) == 1;
    aglxDestroyContext(c); gl_backend_force(NULL);
    return ok;
}

/* ... and the test that would have failed BEFORE H5: the same state against a
 * backend that does not declare depth must still decline.  This is the pair
 * the plan's "each removal carries the test" rule asks for. */
static int t_h5_depth_declines_without_backend_support(void) {
    /* With no hardware backend at all the screen must refuse -- the strict
     * outcome for a backend that cannot honour the state. */
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_DEPTH_TEST);
    int ok = gl_backend_draw_eligible(c, GL_TRIANGLES, 3) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* A depth FUNCTION the DSA does not encode still declines: the object is
 * uploaded with LESS, so anything else would test differently. */
static int t_h5_depth_func_lequal_declines(void) {
    aglx_context_t *c = setup_drawable(); if (!c) return 0;
    glEnable(GL_DEPTH_TEST);
    c->depth_func = GL_LEQUAL;
    int ok = gl_backend_draw_eligible(c, GL_TRIANGLES, 3) == 0;
    aglxDestroyContext(c); gl_backend_force(NULL);
    return ok;
}

static int t_h5_blend_allowed_for_exact_factors(void) {
    aglx_context_t *c = setup_drawable(); if (!c) return 0;
    glEnable(GL_BLEND);
    c->blend_src = GL_SRC_ALPHA; c->blend_dst = GL_ONE_MINUS_SRC_ALPHA;
    int ok = gl_backend_draw_eligible(c, GL_TRIANGLES, 3) == 1;
    aglxDestroyContext(c); gl_backend_force(NULL);
    return ok;
}

/* A factor outside the 1:1 set must decline -- before H5 it declined because
 * blending was refused outright; the refusal moved, it did not vanish. */
static int t_h5_blend_with_constant_colour_declines(void) {
    aglx_context_t *c = setup_drawable(); if (!c) return 0;
    glEnable(GL_BLEND);
    c->blend_src = GL_DST_COLOR;   /* a real factor with no 1:1 mapping */
    int ok = gl_backend_draw_eligible(c, GL_TRIANGLES, 3) == 0;
    aglxDestroyContext(c); gl_backend_force(NULL);
    return ok;
}

static int t_h5_cull_allowed_ccw(void) {
    aglx_context_t *c = setup_drawable(); if (!c) return 0;
    glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
    int ok = gl_backend_draw_eligible(c, GL_TRIANGLES, 3) == 1;
    aglxDestroyContext(c); gl_backend_force(NULL);
    return ok;
}

/* CW winding would cull the opposite face to the one the VELEM order
 * produces, so it declines rather than culling backwards. */
static int t_h5_cull_cw_winding_declines(void) {
    aglx_context_t *c = setup_drawable(); if (!c) return 0;
    glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
    c->front_face = GL_CW;
    int ok = gl_backend_draw_eligible(c, GL_TRIANGLES, 3) == 0;
    aglxDestroyContext(c); gl_backend_force(NULL);
    return ok;
}

static int t_h5_texture_unit0_allowed(void) {
    aglx_context_t *c = setup_drawable(); if (!c) return 0;
    glActiveTexture(GL_TEXTURE0); glEnable(GL_TEXTURE_2D);
    int ok = gl_backend_draw_eligible(c, GL_TRIANGLES, 3) == 1;
    aglxDestroyContext(c); gl_backend_force(NULL);
    return ok;
}

/* A second texture unit is outside the canned shader, and declined before
 * H5 as well -- the refusal stayed, only its reason narrowed. */
static int t_h5_texture_second_unit_declines(void) {
    aglx_context_t *c = setup_drawable(); if (!c) return 0;
    glActiveTexture(GL_TEXTURE0); glEnable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE1); glEnable(GL_TEXTURE_2D);
    int ok = gl_backend_draw_eligible(c, GL_TRIANGLES, 3) == 0;
    aglxDestroyContext(c); gl_backend_force(NULL);
    return ok;
}

/* ---- the nine that never changed, each enumerated ---- */

static int t_h5_still_declines(void) {
    int ok = 1;
    {   aglx_context_t *c = setup_drawable(); if (!c) return 0;
        if (gl_backend_draw_eligible(c, GL_LINES, 2)) ok = 0;
        aglxDestroyContext(c); }
    {   aglx_context_t *c = setup_drawable(); if (!c) return 0;
        if (gl_backend_draw_eligible(c, GL_TRIANGLES, 4)) ok = 0; /* not 3*n */
        aglxDestroyContext(c); }
    {   aglx_context_t *c = setup_drawable(); if (!c) return 0;
        glDisableClientState(GL_VERTEX_ARRAY);
        if (gl_backend_draw_eligible(c, GL_TRIANGLES, 3)) ok = 0;
        aglxDestroyContext(c); }
    {   aglx_context_t *c = setup_drawable(); if (!c) return 0;
        c->stencil_test = 1;
        if (gl_backend_draw_eligible(c, GL_TRIANGLES, 3)) ok = 0;
        aglxDestroyContext(c); }
    {   aglx_context_t *c = setup_drawable(); if (!c) return 0;
        c->alpha_test = 1;
        if (gl_backend_draw_eligible(c, GL_TRIANGLES, 3)) ok = 0;
        aglxDestroyContext(c); }
    {   aglx_context_t *c = setup_drawable(); if (!c) return 0;
        c->scissor_test = 1;
        if (gl_backend_draw_eligible(c, GL_TRIANGLES, 3)) ok = 0;
        aglxDestroyContext(c); }
    {   aglx_context_t *c = setup_drawable(); if (!c) return 0;
        c->polygon_mode = GL_LINE;
        if (gl_backend_draw_eligible(c, GL_TRIANGLES, 3)) ok = 0;
        aglxDestroyContext(c); }
    {   aglx_context_t *c = setup_drawable(); if (!c) return 0;
        c->fog = 1;
        if (gl_backend_draw_eligible(c, GL_TRIANGLES, 3)) ok = 0;
        aglxDestroyContext(c); }
    gl_backend_force(NULL);
    return ok;
}

/* ---- format selection ---- */

/* The whole versioning rule: a backend that does not declare the widened
 * format is asked for format 0, whatever else it can do. */
static int t_h5_format_is_opt_in(void) {
    /* Software only: narrow format, because nothing declared the wide one. */
    aglx_context_t *c = setup(); if (!c) return 0;
    unsigned f = gl_backend_draw_format(c);
    aglxDestroyContext(c);
    return f == GL_BATCH_FMT_POS_COLOR;
}

static int t_h5_format_full_when_declared(void) {
    h5_register_once();
    gl_backend_force("Full Test Backend");
    aglx_context_t *c = setup(); if (!c) { gl_backend_force(NULL); return 0; }
    unsigned f = gl_backend_draw_format(c);
    if (getenv("H5DBG"))
        printf("  [dbg] active=%s flags=%#x fmt=%u reg=%d\n",
               gl_backend_info()->name, gl_backend_info()->flags, f, h5_registered);
    aglxDestroyContext(c); gl_backend_force(NULL);
    return f == GL_BATCH_FMT_FULL;
}

/* No hardware backend active: software, and the narrow format.  Forced
 * explicitly rather than assumed from test order. */
static int t_h5_format_software_is_narrow(void) {
    /* Forced by NAME: force(NULL) means "re-select", and with the H5 backend
     * registered, registration order would pick the hardware one -- which is
     * the very thing this test is not about. */
    gl_backend_force("AuraLite Software Rasterizer");
    aglx_context_t *c = setup(); if (!c) { gl_backend_force(NULL); return 0; }
    const gl_backend_info_t *bi = gl_backend_info();
    unsigned f = gl_backend_draw_format(c);
    int ok = bi && bi->hardware == 0 && f == GL_BATCH_FMT_POS_COLOR;
    aglxDestroyContext(c);
    gl_backend_force(NULL);
    return ok;
}

/* The stride table itself -- the single definition everything else uses. */
static int t_h5_stride_table(void) {
    return GL_BATCH_STRIDE(GL_BATCH_FMT_POS_COLOR) == 8u
        && GL_BATCH_STRIDE(GL_BATCH_FMT_FULL) == 12u
        && GL_BATCH_STRIDE(7u) == 8u;         /* unknown => the safe width */
}

/* The draw reaches the backend with the format the batch carried -- the other
 * half of "libgl packs what the backend promised". */
static int t_h5_batch_reaches_backend_with_format(void) {
    aglx_context_t *c = setup_drawable(); if (!c) return 0;
    static GLfloat pos[9] = { 2,2,0, 29,2,0, 2,29,0 };
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glColor3f(1, 1, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    int ok = full.draw_calls == 1
          && full.last_format == GL_BATCH_FMT_FULL
          && full.last_stride_floats == 12u;
    aglxDestroyContext(c); gl_backend_force(NULL);
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

    printf("--- H5: eligibility table (13 -> 9) ---\n");
    RUN(t_h5_baseline_eligible);
    RUN(t_h5_depth_allowed_when_backend_has_it);
    RUN(t_h5_depth_declines_without_backend_support);
    RUN(t_h5_depth_func_lequal_declines);
    RUN(t_h5_blend_allowed_for_exact_factors);
    RUN(t_h5_blend_with_constant_colour_declines);
    RUN(t_h5_cull_allowed_ccw);
    RUN(t_h5_cull_cw_winding_declines);
    RUN(t_h5_texture_unit0_allowed);
    RUN(t_h5_texture_second_unit_declines);
    RUN(t_h5_still_declines);

    printf("--- H5: batch format ---\n");
    RUN(t_h5_format_is_opt_in); RUN(t_h5_format_full_when_declared);
    RUN(t_h5_format_software_is_narrow); RUN(t_h5_stride_table);
    RUN(t_h5_batch_reaches_backend_with_format);

    printf("\ntest_glbackend: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

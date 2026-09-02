/*
 * test_glstencil.c — host-side unit tests for the 8-bit stencil plane (GL2 L1).
 *
 * Coverage: the eight compare functions, the eight ops including wrap vs
 * saturate, two-pass "draw where stencil == 1", FBO attach/complete, clear,
 * GL_STENCIL_BITS, and colour-buffer canaries so a stencil write cannot bleed.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "GL/gl.h"
#include "GL/auraglx.h"
#include "glcontext.h"
#include "auragui.h"

void gl_imm_reset(void);

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    ag_stub_reset();                                    \
    gl_imm_reset();                                     \
    aglxMakeCurrent(NULL);                              \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

#define W 8
#define H 8

static uint32_t px(aglx_context_t *c, int x, int y) {
    const uint32_t *b = aglxGetColorBuffer(c);
    return b[(size_t)(H - 1 - y) * W + x];
}

static uint8_t spx(aglx_context_t *c, int x, int y) {
    const uint8_t *s = aglxGetStencilBuffer(c);
    return s[(size_t)(H - 1 - y) * W + x];
}

static aglx_context_t *setup(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH | AGLX_STENCIL);
    if (!c) return NULL;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    return c;
}

static void fullscreen_quad(float z) {
    glBegin(GL_QUADS);
    glVertex3f(0, 0, z);
    glVertex3f((float)W, 0, z);
    glVertex3f((float)W, (float)H, z);
    glVertex3f(0, (float)H, z);
    glEnd();
}

static void small_quad(float x0, float y0, float x1, float y1) {
    glBegin(GL_QUADS);
    glVertex3f(x0, y0, 0);
    glVertex3f(x1, y0, 0);
    glVertex3f(x1, y1, 0);
    glVertex3f(x0, y1, 0);
    glEnd();
}

static int color_unchanged(aglx_context_t *c, const uint32_t *snap) {
    const uint32_t *b = aglxGetColorBuffer(c);
    return memcmp(b, snap, (size_t)W * H * sizeof(uint32_t)) == 0;
}

/* ---- queries / defaults ----------------------------------------------- */

static int t_bits_8(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLint bits = -1;
    glGetIntegerv(GL_STENCIL_BITS, &bits);
    int ok = bits == 8 && aglxGetStencilBuffer(c) != NULL
          && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_bits_0_without_plane(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH); /* no stencil */
    if (!c) return 0;
    aglxMakeCurrent(c);
    GLint bits = -1;
    glGetIntegerv(GL_STENCIL_BITS, &bits);
    int ok = bits == 0 && aglxGetStencilBuffer(c) == NULL
          && glGetError() == GL_NO_ERROR;
    /* Enabling the test with no plane must not crash and must still draw. */
    glEnable(GL_STENCIL_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glColor3f(1, 0, 0);
    fullscreen_quad(0);
    ok = ok && px(c, 4, 4) == 0xFF0000u && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_default_off_unchanged(void) {
    /* D3: with the test off, a stencil plane must not change colour. */
    aglx_context_t *with = setup(); if (!with) return 0;
    aglx_context_t *without = aglxCreateContext(2, W, H, AGLX_DEPTH);
    if (!without) { aglxDestroyContext(with); return 0; }

    aglxMakeCurrent(with);
    glColor3f(0, 1, 0);
    fullscreen_quad(0);
    uint32_t a = px(with, 3, 3);

    aglxMakeCurrent(without);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glColor3f(0, 1, 0);
    fullscreen_quad(0);
    uint32_t b = px(without, 3, 3);

    int ok = a == b && a == 0x00FF00u
          && glIsEnabled(GL_STENCIL_TEST) == GL_FALSE;
    aglxDestroyContext(with);
    aglxDestroyContext(without);
    return ok;
}

static int t_queries(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLint v = -1;
    int ok = 1;
    glGetIntegerv(GL_STENCIL_FUNC, &v); ok = ok && v == GL_ALWAYS;
    glGetIntegerv(GL_STENCIL_REF, &v);  ok = ok && v == 0;
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &v); ok = ok && v == (GLint)0xFF;
    glGetIntegerv(GL_STENCIL_FAIL, &v); ok = ok && v == GL_KEEP;
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &v); ok = ok && v == GL_KEEP;
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &v); ok = ok && v == GL_KEEP;
    glGetIntegerv(GL_STENCIL_WRITEMASK, &v); ok = ok && v == (GLint)0xFF;
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &v); ok = ok && v == 0;
    GLboolean b = GL_TRUE;
    glGetBooleanv(GL_STENCIL_TEST, &b); ok = ok && b == GL_FALSE;
    ok = ok && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_invalid_enum(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glStencilFunc(0x1234, 0, 0xFF);
    int ok = glGetError() == GL_INVALID_ENUM && c->stencil_func == GL_ALWAYS;
    glStencilOp(GL_KEEP, 0x9999, GL_KEEP);
    ok = ok && glGetError() == GL_INVALID_ENUM && c->stencil_zfail == GL_KEEP;
    aglxDestroyContext(c);
    return ok;
}

/* ---- clear + canaries ------------------------------------------------- */

static int t_clear_writes(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    uint32_t snap[W * H];
    memcpy(snap, aglxGetColorBuffer(c), sizeof snap);

    glClearStencil(0xA5);
    glClear(GL_STENCIL_BUFFER_BIT);
    int ok = 1;
    for (int y = 0; y < H && ok; y++)
        for (int x = 0; x < W; x++)
            if (spx(c, x, y) != 0xA5) ok = 0;
    ok = ok && color_unchanged(c, snap);
    aglxDestroyContext(c);
    return ok;
}

static int t_clear_honours_writemask(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearStencil(0xFF);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilMask(0x0F);
    glClearStencil(0x00);
    glClear(GL_STENCIL_BUFFER_BIT);
    int ok = spx(c, 2, 2) == 0xF0;   /* low nibble cleared, high kept */
    glStencilMask(0xFF);
    aglxDestroyContext(c);
    return ok;
}

static int t_sfail_does_not_write_color(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glColor3f(0, 0, 1);
    fullscreen_quad(0);
    uint32_t snap[W * H];
    memcpy(snap, aglxGetColorBuffer(c), sizeof snap);

    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_NEVER, 0, 0xFF);
    glStencilOp(GL_INCR, GL_INCR, GL_INCR);
    glColor3f(1, 0, 0);
    fullscreen_quad(0);

    int ok = color_unchanged(c, snap) && spx(c, 4, 4) == 1;
    aglxDestroyContext(c);
    return ok;
}

/* ---- func × op -------------------------------------------------------- */

static int func_case(GLenum func, GLint ref, uint8_t stored, int should_pass) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearStencil((GLint)stored);
    glClear(GL_STENCIL_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(func, ref, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glColor3f(0, 1, 0);
    fullscreen_quad(0);
    uint32_t p = px(c, 4, 4);
    int ok = should_pass ? (p == 0x00FF00u) : (p == 0);
    aglxDestroyContext(c);
    return ok;
}

static int t_func_never(void)    { return func_case(GL_NEVER,    1, 1, 0); }
static int t_func_always(void)   { return func_case(GL_ALWAYS,   0, 9, 1); }
static int t_func_equal(void)    { return func_case(GL_EQUAL,    4, 4, 1); }
static int t_func_equal_miss(void){ return func_case(GL_EQUAL,   4, 5, 0); }
static int t_func_notequal(void) { return func_case(GL_NOTEQUAL, 4, 5, 1); }
static int t_func_less(void)     { return func_case(GL_LESS,     3, 5, 1); }
static int t_func_lequal(void)   { return func_case(GL_LEQUAL,   5, 5, 1); }
static int t_func_greater(void)  { return func_case(GL_GREATER,  7, 2, 1); }
static int t_func_gequal(void)   { return func_case(GL_GEQUAL,   2, 2, 1); }

static int op_case(GLenum op, uint8_t start, uint8_t want) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearStencil((GLint)start);
    glClear(GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 0x7A, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, op);
    glColor3f(1, 1, 1);
    fullscreen_quad(0);
    int ok = spx(c, 4, 4) == want;
    aglxDestroyContext(c);
    return ok;
}

static int t_op_keep(void)      { return op_case(GL_KEEP,      9, 9); }
static int t_op_zero(void)      { return op_case(GL_ZERO,      9, 0); }
static int t_op_replace(void)   { return op_case(GL_REPLACE,   9, 0x7A); }
static int t_op_incr(void)      { return op_case(GL_INCR,      9, 10); }
static int t_op_decr(void)      { return op_case(GL_DECR,      9, 8); }
static int t_op_invert(void)    { return op_case(GL_INVERT,    0x0F, 0xF0); }
static int t_incr_saturates(void){ return op_case(GL_INCR,     255, 255); }
static int t_decr_saturates(void){ return op_case(GL_DECR,     0, 0); }
static int t_incr_wraps(void)   { return op_case(GL_INCR_WRAP, 255, 0); }
static int t_decr_wraps(void)   { return op_case(GL_DECR_WRAP, 0, 255); }

static int t_writemask_partial(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearStencil(0x00);
    glClear(GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 0xFF, 0xFF);
    glStencilMask(0x0F);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    fullscreen_quad(0);
    int ok = spx(c, 1, 1) == 0x0F;
    glStencilMask(0xFF);
    aglxDestroyContext(c);
    return ok;
}

/* ---- two-pass clip ---------------------------------------------------- */

static int t_two_pass_clip(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColor3f(0, 0, 0);
    small_quad(0, 0, 4, 4);            /* mark the lower-left 4×4 */

    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glColor3f(1, 0, 0);
    fullscreen_quad(0);

    int ok = px(c, 1, 1) == 0xFF0000u   /* inside the stencil */
          && px(c, 6, 6) == 0           /* outside stays clear */
          && spx(c, 1, 1) == 1
          && spx(c, 6, 6) == 0;
    aglxDestroyContext(c);
    return ok;
}

static int t_depth_fail_zfail(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glColor3f(0, 0, 1);
    fullscreen_quad(0.5f);             /* near: window depth 0.25 */

    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 0, 0xFF);
    glStencilOp(GL_KEEP, GL_INCR, GL_KEEP);  /* zfail increments */
    glColor3f(1, 0, 0);
    fullscreen_quad(-0.5f);            /* far: rejected by depth */

    int ok = px(c, 4, 4) == 0x0000FFu  /* colour unchanged */
          && spx(c, 4, 4) == 1;        /* zfail fired */
    aglxDestroyContext(c);
    return ok;
}

/* ---- FBO -------------------------------------------------------------- */

static GLuint make_color_tex(int n) {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, n, n, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return id;
}

static int t_fbo_attach_complete(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb = 0, tex = make_color_tex(W), rb = 0;
    glGenFramebuffers(1, &fb);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    while (glGetError() != GL_NO_ERROR) { }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, rb);
    int ok = glGetError() == GL_NO_ERROR
          && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE
          && c->stencil != NULL && c->stencil != c->win_stencil;

    GLint bits = 0;
    glGetIntegerv(GL_STENCIL_BITS, &bits);
    ok = ok && bits == 8;

    glViewport(0, 0, W, H);
    glClearStencil(0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColor3f(0, 0, 0);
    small_quad(0, 0, 4, 4);
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glColor3f(0, 1, 0);
    fullscreen_quad(0);

    unsigned char rgb[3];
    glReadPixels(1, 1, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    ok = ok && rgb[1] == 255 && rgb[0] == 0;
    glReadPixels(6, 6, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    ok = ok && rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

static int t_fbo_wrong_type_incomplete(void) {
    /* Attaching a colour RBO as stencil is accepted, then incomplete. */
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb = 0, tex = make_color_tex(W), rb = 0;
    glGenFramebuffers(1, &fb);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    while (glGetError() != GL_NO_ERROR) { }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, rb);
    int ok = glGetError() == GL_NO_ERROR
          && glCheckFramebufferStatus(GL_FRAMEBUFFER)
             == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

static int t_fbo_dimension_mismatch(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint fb = 0, tex = make_color_tex(W), rb = 0;
    glGenFramebuffers(1, &fb);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, 4, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, rb);
    int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER)
             == GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS;
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, W, H);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

static int t_aglx_default_has_stencil(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEFAULT);
    if (!c) return 0;
    aglxMakeCurrent(c);
    GLint bits = 0;
    glGetIntegerv(GL_STENCIL_BITS, &bits);
    int ok = bits == 8 && aglxGetStencilBuffer(c) != NULL;
    aglxDestroyContext(c);
    return ok;
}

int main(void) {
    printf("=== test_glstencil: 8-bit stencil plane (GL2 L1) ===\n");

    printf("--- queries ---\n");
    RUN(t_bits_8); RUN(t_bits_0_without_plane);
    RUN(t_default_off_unchanged); RUN(t_queries); RUN(t_invalid_enum);
    RUN(t_aglx_default_has_stencil);

    printf("--- clear and canaries ---\n");
    RUN(t_clear_writes); RUN(t_clear_honours_writemask);
    RUN(t_sfail_does_not_write_color);

    printf("--- func ---\n");
    RUN(t_func_never); RUN(t_func_always);
    RUN(t_func_equal); RUN(t_func_equal_miss); RUN(t_func_notequal);
    RUN(t_func_less); RUN(t_func_lequal);
    RUN(t_func_greater); RUN(t_func_gequal);

    printf("--- op (wrap vs saturate) ---\n");
    RUN(t_op_keep); RUN(t_op_zero); RUN(t_op_replace);
    RUN(t_op_incr); RUN(t_op_decr); RUN(t_op_invert);
    RUN(t_incr_saturates); RUN(t_decr_saturates);
    RUN(t_incr_wraps); RUN(t_decr_wraps); RUN(t_writemask_partial);

    printf("--- two-pass ---\n");
    RUN(t_two_pass_clip); RUN(t_depth_fail_zfail);

    printf("--- FBO ---\n");
    RUN(t_fbo_attach_complete); RUN(t_fbo_wrong_type_incomplete);
    RUN(t_fbo_dimension_mismatch);

    printf("\ntest_glstencil: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

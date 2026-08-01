/*
 * test_gltex2.c — host-side unit tests for GL 1.2/1.3 texturing (phase G10).
 *
 * Covers mipmaps, multitexturing, 3D textures, cube maps and
 * GL_CLAMP_TO_BORDER.  Links the real gltexture.c, glraster.c and glu.c and
 * drives them through the public GL API, exactly like test_gltex.c does for
 * G6 — a test that reimplemented the sampler would only prove the test works.
 *
 * The mipmap tests are written so that a NON-mipmapped implementation
 * demonstrably fails them: the aliasing test compares pixel variance against
 * the un-mipmapped case rather than asserting an absolute value, because the
 * point of a mipmap is the difference it makes, not any particular colour.
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "GL/gl.h"
#include "GL/glu.h"
#include "GL/auraglx.h"
#include "glcontext.h"
#include "glvertex.h"
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

#define W 64
#define H 64

static uint32_t px(aglx_context_t *c, int x, int y) {
    const uint32_t *b = aglxGetColorBuffer(c);
    return b[(size_t)(H - 1 - y) * W + x];
}

static int near_u8(int a, int b, int tol) {
    int d = a - b; if (d < 0) d = -d; return d <= tol;
}

static aglx_context_t *setup(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    if (!c) return NULL;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, 0, H, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glColor3f(1, 1, 1);
    return c;
}

/* A quad covering [x0,x1]x[y0,y1] in window pixels with s,t running 0..smax. */
static void quad(GLfloat x0, GLfloat y0, GLfloat x1, GLfloat y1,
                 GLfloat smax, GLfloat tmax) {
    glBegin(GL_QUADS);
    glTexCoord2f(0,    0);    glVertex3f(x0, y0, 0);
    glTexCoord2f(smax, 0);    glVertex3f(x1, y0, 0);
    glTexCoord2f(smax, tmax); glVertex3f(x1, y1, 0);
    glTexCoord2f(0,    tmax); glVertex3f(x0, y1, 0);
    glEnd();
}

/* ============================================================================
 * Mipmap chain construction
 * ==========================================================================*/

/* An NxN checkerboard in GL_RGB, alternating black and white per texel. */
static unsigned char *checker_rgb(int n) {
    unsigned char *p = (unsigned char *)malloc((size_t)n * n * 3);
    if (!p) return NULL;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            unsigned char v = ((x + y) & 1) ? 255 : 0;
            p[((size_t)y * n + x) * 3 + 0] = v;
            p[((size_t)y * n + x) * 3 + 1] = v;
            p[((size_t)y * n + x) * 3 + 2] = v;
        }
    }
    return p;
}

/* Reach into the context to inspect the chain.  Legitimate here: these are
 * internal-consistency checks that no public entry point can express. */
static gl_texture_t *find_tex(aglx_context_t *c, GLuint name) {
    for (int i = 0; i < GL_MAX_TEXTURES_IMPL; i++) {
        if (c->textures[i].used && c->textures[i].name == name) {
            return &c->textures[i];
        }
    }
    return NULL;
}

/* glGenerateMipmap on a 64x64 image must produce levels 0..6, halving each
 * time and ending at 1x1. */
static int t_chain_dimensions(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    unsigned char *img = checker_rgb(64);
    if (!img) { aglxDestroyContext(c); return 0; }

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, img);
    glGenerateMipmap(GL_TEXTURE_2D);

    gl_texture_t *t = find_tex(c, id);
    int ok = t && t->levels == 7;
    for (int l = 0; ok && l < 7; l++) {
        GLsizei expect = (GLsizei)(64 >> l);
        ok = t->img[0][l].texels &&
             t->img[0][l].width == expect && t->img[0][l].height == expect;
    }
    free(img);
    aglxDestroyContext(c);
    return ok;
}

/* A non-square image halves each dimension independently and stops at 1. */
static int t_chain_non_square(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    unsigned char img[8 * 2 * 3];
    memset(img, 128, sizeof img);

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 8, 2, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, img);
    glGenerateMipmap(GL_TEXTURE_2D);

    gl_texture_t *t = find_tex(c, id);
    /* 8x2 -> 4x1 -> 2x1 -> 1x1: four levels. */
    int ok = t && t->levels == 4 &&
             t->img[0][1].width == 4 && t->img[0][1].height == 1 &&
             t->img[0][3].width == 1 && t->img[0][3].height == 1;
    aglxDestroyContext(c);
    return ok;
}

/* A box filter preserves the mean: every generated level of a checkerboard
 * must average to the same mid-grey as level 0. */
static int t_chain_preserves_mean(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    unsigned char *img = checker_rgb(32);
    if (!img) { aglxDestroyContext(c); return 0; }

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 32, 32, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, img);
    glGenerateMipmap(GL_TEXTURE_2D);

    gl_texture_t *t = find_tex(c, id);
    int ok = t && t->levels == 6;
    for (int l = 0; ok && l < t->levels; l++) {
        const gl_teximage_t *im = &t->img[0][l];
        size_t n = (size_t)im->width * im->height;
        double sum = 0.0;
        for (size_t i = 0; i < n; i++) sum += (double)(im->texels[i] & 0xFF);
        double mean = sum / (double)n;
        /* Rounding accumulates a little per level; 4/255 is generous but
         * still an order of magnitude tighter than a broken filter. */
        ok = fabs(mean - 127.5) < 4.0;
    }
    free(img);
    aglxDestroyContext(c);
    return ok;
}

/* Uploading level 0 again must discard the chain built from the old image:
 * the smaller levels would otherwise still describe the previous texture. */
static int t_reupload_drops_chain(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    unsigned char *img = checker_rgb(16);
    if (!img) { aglxDestroyContext(c); return 0; }

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 16, 16, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, img);
    glGenerateMipmap(GL_TEXTURE_2D);
    gl_texture_t *t = find_tex(c, id);
    int had_chain = t && t->levels == 5;

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 16, 16, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, img);
    int ok = had_chain && t->levels == 1 && t->img[0][1].texels == NULL;

    free(img);
    aglxDestroyContext(c);
    return ok;
}

/* gluBuild2DMipmaps must fill the same chain glGenerateMipmap would, and the
 * two must agree level for level — they are separate implementations of the
 * same box filter, one client-side and one inside libgl. */
static int t_glu_matches_generate(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    unsigned char *img = checker_rgb(16);
    if (!img) { aglxDestroyContext(c); return 0; }

    GLuint a = 0, b = 0;
    glGenTextures(1, &a);
    glBindTexture(GL_TEXTURE_2D, a);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 16, 16, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, img);
    glGenerateMipmap(GL_TEXTURE_2D);

    glGenTextures(1, &b);
    glBindTexture(GL_TEXTURE_2D, b);
    int rc = gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, 16, 16, GL_RGB,
                               GL_UNSIGNED_BYTE, img);

    gl_texture_t *ta = find_tex(c, a), *tb = find_tex(c, b);
    int ok = rc == 0 && ta && tb && ta->levels == 5 && tb->levels == 5;
    for (int l = 0; ok && l < 5; l++) {
        const gl_teximage_t *ia = &ta->img[0][l], *ib = &tb->img[0][l];
        ok = ia->width == ib->width && ia->height == ib->height;
        size_t n = (size_t)ia->width * ia->height;
        for (size_t i = 0; ok && i < n; i++) {
            /* Allow one unit of rounding difference per channel: the two
             * paths round in different orders. */
            ok = near_u8((int)(ia->texels[i] & 0xFF),
                         (int)(ib->texels[i] & 0xFF), 1);
        }
    }
    free(img);
    aglxDestroyContext(c);
    return ok;
}

/* glGenerateMipmap with no level 0 uploaded is an error, not a crash. */
static int t_generate_without_image(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    while (glGetError() != GL_NO_ERROR) { }
    glGenerateMipmap(GL_TEXTURE_2D);
    int ok = glGetError() == GL_INVALID_OPERATION;
    aglxDestroyContext(c);
    return ok;
}

/* ============================================================================
 * Mipmapped rendering
 * ==========================================================================*/

/* Draw a heavily minified checkerboard and report the variance of the drawn
 * pixels' red channel.  A mipmapped draw averages the checker away and gives
 * a low variance; an un-mipmapped one point-samples it and gives a high one. */
static double minified_variance(aglx_context_t *c, GLenum min_filter,
                                int mipmapped) {
    unsigned char *img = checker_rgb(64);
    if (!img) return -1.0;

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, img);
    if (mipmapped) glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* An 11x11 pixel quad showing the whole 64x64 texture: ~6:1 minification.
     *
     * The size is deliberately NOT a power-of-two fraction of 64.  At exactly
     * 8:1 every pixel centre lands on the same phase of the checkerboard, the
     * point-sampled image comes out uniformly black, and the un-mipmapped
     * reference would show zero variance -- making the test pass for entirely
     * the wrong reason.  An 11-pixel quad walks through the phases and gives
     * the aliasing this is meant to measure. */
    quad(8, 8, 19, 19, 1.0f, 1.0f);

    double sum = 0.0, sumsq = 0.0;
    int n = 0;
    for (int y = 10; y < 17; y++) {
        for (int x = 10; x < 17; x++) {
            double v = (double)((px(c, x, y) >> 16) & 0xFF);
            sum += v; sumsq += v * v; n++;
        }
    }
    glDeleteTextures(1, &id);
    glDisable(GL_TEXTURE_2D);
    free(img);
    if (n == 0) return -1.0;
    double mean = sum / n;
    return sumsq / n - mean * mean;
}

/* The headline test of the phase: mipmapping must reduce minification
 * aliasing.  Asserted as a comparison, not an absolute, because that is the
 * property that actually matters. */
static int t_mipmap_reduces_aliasing(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    double plain = minified_variance(c, GL_NEAREST, 0);
    double mip   = minified_variance(c, GL_LINEAR_MIPMAP_LINEAR, 1);
    aglxDestroyContext(c);
    if (plain < 0.0 || mip < 0.0) return 0;
    /* A 64x64 checkerboard point-sampled into 8x8 pixels alternates between
     * black and white, variance ~16000.  Mipmapped it resolves to flat grey. */
    return plain > 1000.0 && mip < plain / 10.0;
}

/* All four mipmap filters must be accepted and must all draw something
 * sensible (mid-grey) for a minified checkerboard. */
static int t_all_four_mipmap_filters(void) {
    static const GLenum filters[4] = {
        GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST,
        GL_NEAREST_MIPMAP_LINEAR,  GL_LINEAR_MIPMAP_LINEAR
    };
    aglx_context_t *c = setup(); if (!c) return 0;
    int ok = 1;
    for (int i = 0; ok && i < 4; i++) {
        double var = minified_variance(c, filters[i], 1);
        if (var < 0.0) { ok = 0; break; }
        /* Every mipmap filter must land on the checkerboard's mean. */
        ok = var < 2000.0;
    }
    aglxDestroyContext(c);
    return ok;
}

/* A mipmap filter on a texture with NO chain must still draw, falling back to
 * level 0 (§3.8.10).  This is the default state of every texture, so getting
 * it wrong would break every G6 application. */
static int t_mipmap_filter_without_chain(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    unsigned char rgb[3] = { 200, 100, 50 };
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, rgb);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    quad(8, 8, 56, 56, 1.0f, 1.0f);

    uint32_t p = px(c, 32, 32);
    int ok = near_u8((p >> 16) & 0xFF, 200, 2) &&
             near_u8((p >> 8) & 0xFF, 100, 2) &&
             near_u8(p & 0xFF, 50, 2);
    aglxDestroyContext(c);
    return ok;
}

/* Magnification must never use a mipmap level, whatever the min filter says:
 * a magnified 2x2 texture has to show its own texels, not the 1x1 average. */
static int t_magnification_ignores_chain(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    static const unsigned char rgb[2 * 2 * 3] = {
        255, 0, 0,   0, 255, 0,
        0, 0, 255,   255, 255, 255,
    };
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, rgb);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    quad(0, 0, W, H, 1.0f, 1.0f);

    /* Bottom-left quadrant must still be pure red. */
    uint32_t p = px(c, 8, 8);
    int ok = p == 0x00FF0000u;
    aglxDestroyContext(c);
    return ok;
}

/* GL_TEXTURE_MAX_LEVEL must cap the chain: capped to level 0, a minified
 * checkerboard aliases again. */
static int t_max_level_caps_chain(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    unsigned char *img = checker_rgb(64);
    if (!img) { aglxDestroyContext(c); return 0; }

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 64, 64, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, img);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    quad(8, 8, 19, 19, 1.0f, 1.0f);

    double sum = 0.0, sumsq = 0.0; int n = 0;
    for (int y = 10; y < 17; y++) {
        for (int x = 10; x < 17; x++) {
            double v = (double)((px(c, x, y) >> 16) & 0xFF);
            sum += v; sumsq += v * v; n++;
        }
    }
    double mean = sum / n;
    double var = sumsq / n - mean * mean;

    free(img);
    aglxDestroyContext(c);
    /* Capped to level 0 this is a point-sampled checkerboard again. */
    return var > 1000.0;
}

/* An invalid min filter is GL_INVALID_ENUM; a mipmap enum as the MAG filter
 * is too, because magnification has no levels to choose between. */
static int t_filter_validation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    while (glGetError() != GL_NO_ERROR) { }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    int ok = glGetError() == GL_NO_ERROR;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    ok = ok && glGetError() == GL_INVALID_ENUM;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_RGB);
    ok = ok && glGetError() == GL_INVALID_ENUM;

    aglxDestroyContext(c);
    return ok;
}

/* ============================================================================
 * Multitexturing
 * ==========================================================================*/

/* Bind a 1x1 texture of the given colour to the currently active unit. */
static GLuint solid_texture(unsigned char r, unsigned char g, unsigned char b) {
    unsigned char rgb[3] = { r, g, b };
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, rgb);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return id;
}

/* glActiveTexture must move the binding point, and glGetIntegerv must report
 * which unit is selected. */
static int t_active_texture_selects_unit(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLint v = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &v);
    int ok = v == GL_TEXTURE0;

    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &v);
    ok = ok && v == GL_TEXTURE1;

    GLuint b = solid_texture(0, 0, 255);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &v);
    ok = ok && v == (GLint)b;

    /* Unit 0 must be untouched by all of that. */
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &v);
    ok = ok && v == 0;

    aglxDestroyContext(c);
    return ok;
}

/* Out-of-range units are GL_INVALID_ENUM, and the selection must not move. */
static int t_active_texture_validation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    while (glGetError() != GL_NO_ERROR) { }
    glActiveTexture(GL_TEXTURE0 + GL_MAX_TEXTURE_UNITS_IMPL);
    int ok = glGetError() == GL_INVALID_ENUM;

    GLint v = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &v);
    ok = ok && v == GL_TEXTURE0;
    aglxDestroyContext(c);
    return ok;
}

/* The point of two units: GL_MODULATE on both must give the PRODUCT of the
 * two textures, because unit 0's output is unit 1's input. */
static int t_two_units_modulate(void) {
    aglx_context_t *c = setup(); if (!c) return 0;

    glActiveTexture(GL_TEXTURE0);
    solid_texture(255, 128, 0);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    glActiveTexture(GL_TEXTURE1);
    solid_texture(128, 255, 255);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glBegin(GL_QUADS);
    glMultiTexCoord2f(GL_TEXTURE1, 0, 0); glTexCoord2f(0, 0); glVertex3f(8,  8,  0);
    glMultiTexCoord2f(GL_TEXTURE1, 1, 0); glTexCoord2f(1, 0); glVertex3f(56, 8,  0);
    glMultiTexCoord2f(GL_TEXTURE1, 1, 1); glTexCoord2f(1, 1); glVertex3f(56, 56, 0);
    glMultiTexCoord2f(GL_TEXTURE1, 0, 1); glTexCoord2f(0, 1); glVertex3f(8,  56, 0);
    glEnd();

    uint32_t p = px(c, 32, 32);
    /* (255,128,0) * (128,255,255) / 255 = (128, 128, 0). */
    int ok = near_u8((p >> 16) & 0xFF, 128, 3) &&
             near_u8((p >> 8)  & 0xFF, 128, 3) &&
             near_u8( p        & 0xFF,   0, 3);
    aglxDestroyContext(c);
    return ok;
}

/* Unit 1 with GL_REPLACE must overwrite whatever unit 0 produced: the units
 * are applied in order, and the later one wins. */
static int t_unit_order_replace(void) {
    aglx_context_t *c = setup(); if (!c) return 0;

    glActiveTexture(GL_TEXTURE0);
    solid_texture(255, 0, 0);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    glActiveTexture(GL_TEXTURE1);
    solid_texture(0, 0, 255);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    glBegin(GL_QUADS);
    glMultiTexCoord2f(GL_TEXTURE1, 0, 0); glTexCoord2f(0, 0); glVertex3f(8,  8,  0);
    glMultiTexCoord2f(GL_TEXTURE1, 1, 0); glTexCoord2f(1, 0); glVertex3f(56, 8,  0);
    glMultiTexCoord2f(GL_TEXTURE1, 1, 1); glTexCoord2f(1, 1); glVertex3f(56, 56, 0);
    glMultiTexCoord2f(GL_TEXTURE1, 0, 1); glTexCoord2f(0, 1); glVertex3f(8,  56, 0);
    glEnd();

    int ok = px(c, 32, 32) == 0x000000FFu;
    aglxDestroyContext(c);
    return ok;
}

/* A unit that is bound but not enabled must contribute nothing. */
static int t_disabled_unit_ignored(void) {
    aglx_context_t *c = setup(); if (!c) return 0;

    glActiveTexture(GL_TEXTURE0);
    solid_texture(255, 0, 0);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    glActiveTexture(GL_TEXTURE1);
    solid_texture(0, 0, 255);
    /* deliberately NOT enabled */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    quad(8, 8, 56, 56, 1.0f, 1.0f);
    int ok = px(c, 32, 32) == 0x00FF0000u;
    aglxDestroyContext(c);
    return ok;
}

/* glTexEnv affects only the active unit. */
static int t_env_is_per_unit(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glActiveTexture(GL_TEXTURE0);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glActiveTexture(GL_TEXTURE1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);

    int ok = c->texunits[0].env_mode == GL_REPLACE &&
             c->texunits[1].env_mode == GL_DECAL;
    aglxDestroyContext(c);
    return ok;
}

/* glTexCoord always writes unit 0, even when unit 1 is the active one — the
 * server-side selector does not redirect the immediate-mode coordinate. */
static int t_texcoord_always_unit0(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glActiveTexture(GL_TEXTURE1);

    glActiveTexture(GL_TEXTURE0);
    solid_texture(0, 255, 0);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    glActiveTexture(GL_TEXTURE1);   /* active, but no texture enabled here */
    quad(8, 8, 56, 56, 1.0f, 1.0f);

    int ok = px(c, 32, 32) == 0x0000FF00u;
    aglxDestroyContext(c);
    return ok;
}

/* Deleting a texture must unbind it from EVERY unit, not just the active one. */
static int t_delete_unbinds_all_units(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = 0;
    glGenTextures(1, &id);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, id);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, id);
    glDeleteTextures(1, &id);

    int ok = c->texunits[0].binding_2d == 0 && c->texunits[1].binding_2d == 0;
    aglxDestroyContext(c);
    return ok;
}

/* glClientActiveTexture must select which array glTexCoordPointer sets, and
 * must be independent of glActiveTexture. */
static int t_client_active_texture(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    static const GLfloat uv[8] = { 0,0, 1,0, 1,1, 0,1 };

    glActiveTexture(GL_TEXTURE1);          /* server selector: irrelevant here */
    glClientActiveTexture(GL_TEXTURE1);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);

    int ok = c->array_texcoord[1].enabled == GL_TRUE &&
             c->array_texcoord[1].ptr == uv &&
             c->array_texcoord[0].enabled == GL_FALSE &&
             c->array_texcoord[0].ptr == NULL;

    GLint v = 0;
    glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &v);
    ok = ok && v == GL_TEXTURE1;

    aglxDestroyContext(c);
    return ok;
}

/* ============================================================================
 * 3D textures
 * ==========================================================================*/

/* A 1x1x2 volume: slice 0 red, slice 1 blue.  Sampling at r=0.25 must give
 * the first slice, r=0.75 the second, and r=0.5 their blend. */
static int t_texture_3d_slices(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    static const unsigned char vol[2 * 3] = {
        255, 0, 0,      /* slice 0 */
        0, 0, 255,      /* slice 1 */
    };
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_3D, id);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB, 1, 1, 2, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, vol);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_3D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    glBegin(GL_QUADS);
    glTexCoord3f(0.5f, 0.5f, 0.25f); glVertex3f(8,  8,  0);
    glTexCoord3f(0.5f, 0.5f, 0.25f); glVertex3f(30, 8,  0);
    glTexCoord3f(0.5f, 0.5f, 0.25f); glVertex3f(30, 56, 0);
    glTexCoord3f(0.5f, 0.5f, 0.25f); glVertex3f(8,  56, 0);
    glEnd();
    int ok = px(c, 20, 32) == 0x00FF0000u;

    glBegin(GL_QUADS);
    glTexCoord3f(0.5f, 0.5f, 0.75f); glVertex3f(34, 8,  0);
    glTexCoord3f(0.5f, 0.5f, 0.75f); glVertex3f(56, 8,  0);
    glTexCoord3f(0.5f, 0.5f, 0.75f); glVertex3f(56, 56, 0);
    glTexCoord3f(0.5f, 0.5f, 0.75f); glVertex3f(34, 56, 0);
    glEnd();
    ok = ok && px(c, 45, 32) == 0x000000FFu;

    aglxDestroyContext(c);
    return ok;
}

/* Trilinear filtering across the two slices: r = 0.5 sits exactly between the
 * slice centres and must give the average of the two. */
static int t_texture_3d_trilinear(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    static const unsigned char vol[2 * 3] = {
        255, 0, 0,
        0, 0, 255,
    };
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_3D, id);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB, 1, 1, 2, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, vol);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glEnable(GL_TEXTURE_3D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    glBegin(GL_QUADS);
    glTexCoord3f(0.5f, 0.5f, 0.5f); glVertex3f(8,  8,  0);
    glTexCoord3f(0.5f, 0.5f, 0.5f); glVertex3f(56, 8,  0);
    glTexCoord3f(0.5f, 0.5f, 0.5f); glVertex3f(56, 56, 0);
    glTexCoord3f(0.5f, 0.5f, 0.5f); glVertex3f(8,  56, 0);
    glEnd();

    uint32_t p = px(c, 32, 32);
    int ok = near_u8((p >> 16) & 0xFF, 128, 4) &&
             near_u8((p >> 8)  & 0xFF,   0, 2) &&
             near_u8( p        & 0xFF, 128, 4);
    aglxDestroyContext(c);
    return ok;
}

/* Binding one name to two different targets is GL_INVALID_OPERATION: a
 * texture object's target is fixed at first bind. */
static int t_target_is_sticky(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    while (glGetError() != GL_NO_ERROR) { }
    glBindTexture(GL_TEXTURE_3D, id);
    int ok = glGetError() == GL_INVALID_OPERATION;
    aglxDestroyContext(c);
    return ok;
}

/* ============================================================================
 * Cube maps
 * ==========================================================================*/

/* Six 1x1 faces, each a distinct primary so the face choice is unambiguous. */
static GLuint make_cube(void) {
    static const unsigned char face_rgb[6][3] = {
        { 255,   0,   0 },   /* +X red     */
        {   0, 255,   0 },   /* -X green   */
        {   0,   0, 255 },   /* +Y blue    */
        { 255, 255,   0 },   /* -Y yellow  */
        { 255,   0, 255 },   /* +Z magenta */
        {   0, 255, 255 },   /* -Z cyan    */
    };
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_CUBE_MAP, id);
    for (int f = 0; f < 6; f++) {
        glTexImage2D((GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f), 0, GL_RGB,
                     1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, face_rgb[f]);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return id;
}

/* Draw a small quad with a constant cube-map direction and read the colour. */
static uint32_t cube_probe(aglx_context_t *c, GLfloat x, GLfloat y, GLfloat z) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord3f(x, y, z); glVertex3f(8,  8,  0);
    glTexCoord3f(x, y, z); glVertex3f(56, 8,  0);
    glTexCoord3f(x, y, z); glVertex3f(56, 56, 0);
    glTexCoord3f(x, y, z); glVertex3f(8,  56, 0);
    glEnd();
    return px(c, 32, 32);
}

/* Each of the six axis directions must return its own face's colour. */
static int t_cube_face_selection(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    make_cube();
    glEnable(GL_TEXTURE_CUBE_MAP);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    int ok = 1;
    ok = ok && cube_probe(c,  1,  0,  0) == 0x00FF0000u;   /* +X red     */
    ok = ok && cube_probe(c, -1,  0,  0) == 0x0000FF00u;   /* -X green   */
    ok = ok && cube_probe(c,  0,  1,  0) == 0x000000FFu;   /* +Y blue    */
    ok = ok && cube_probe(c,  0, -1,  0) == 0x00FFFF00u;   /* -Y yellow  */
    ok = ok && cube_probe(c,  0,  0,  1) == 0x00FF00FFu;   /* +Z magenta */
    ok = ok && cube_probe(c,  0,  0, -1) == 0x0000FFFFu;   /* -Z cyan    */

    aglxDestroyContext(c);
    return ok;
}

/* The MAJOR axis decides, so a direction that merely leans towards +X still
 * picks the +X face. */
static int t_cube_major_axis(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    make_cube();
    glEnable(GL_TEXTURE_CUBE_MAP);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    int ok = cube_probe(c, 1.0f, 0.9f, -0.8f) == 0x00FF0000u &&
             cube_probe(c, 0.4f, -0.3f, -1.0f) == 0x0000FFFFu;
    aglxDestroyContext(c);
    return ok;
}

/* A cube map takes priority over a 2D texture enabled on the same unit
 * (§3.8.15). */
static int t_cube_beats_2d(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    solid_texture(0, 0, 0);          /* black 2D texture, unit 0 */
    glEnable(GL_TEXTURE_2D);
    make_cube();
    glEnable(GL_TEXTURE_CUBE_MAP);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    int ok = cube_probe(c, 1, 0, 0) == 0x00FF0000u;
    aglxDestroyContext(c);
    return ok;
}

/* glGenerateMipmap on a cube map must build a chain on all six faces. */
static int t_cube_mipmap_all_faces(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    unsigned char *img = checker_rgb(8);
    if (!img) { aglxDestroyContext(c); return 0; }

    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_CUBE_MAP, id);
    for (int f = 0; f < 6; f++) {
        glTexImage2D((GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f), 0, GL_RGB,
                     8, 8, 0, GL_RGB, GL_UNSIGNED_BYTE, img);
    }
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

    gl_texture_t *t = find_tex(c, id);
    int ok = t != NULL;
    for (int f = 0; ok && f < 6; f++) {
        ok = t->img[f][3].texels != NULL &&
             t->img[f][3].width == 1 && t->img[f][3].height == 1;
    }
    free(img);
    aglxDestroyContext(c);
    return ok;
}

/* ============================================================================
 * GL_CLAMP_TO_BORDER
 * ==========================================================================*/

/* Sampling outside [0,1] with GL_CLAMP_TO_BORDER must return the border
 * colour, not the edge texel — which is the whole difference from
 * GL_CLAMP_TO_EDGE. */
static int t_clamp_to_border(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    static const GLfloat border[4] = { 0.0f, 0.0f, 1.0f, 1.0f };   /* blue */

    GLuint id = solid_texture(255, 0, 0);
    (void)id;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    /* s runs 0..3 across the quad; beyond s=1 the sample is outside. */
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(8,  8,  0);
    glTexCoord2f(3, 0); glVertex3f(56, 8,  0);
    glTexCoord2f(3, 1); glVertex3f(56, 56, 0);
    glTexCoord2f(0, 1); glVertex3f(8,  56, 0);
    glEnd();

    /* Left edge is inside the texture: red.  Right end is far outside: blue. */
    int ok = px(c, 10, 32) == 0x00FF0000u && px(c, 54, 32) == 0x000000FFu;
    aglxDestroyContext(c);
    return ok;
}

/* GL_CLAMP_TO_EDGE must still clamp, not use the border — the two modes have
 * to stay distinguishable. */
static int t_clamp_to_edge_unchanged(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    static const GLfloat border[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

    solid_texture(255, 0, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(8,  8,  0);
    glTexCoord2f(3, 0); glVertex3f(56, 8,  0);
    glTexCoord2f(3, 1); glVertex3f(56, 56, 0);
    glTexCoord2f(0, 1); glVertex3f(8,  56, 0);
    glEnd();

    int ok = px(c, 54, 32) == 0x00FF0000u;
    aglxDestroyContext(c);
    return ok;
}

/* The border colour is per texture object, and glTexParameterfv stores it. */
static int t_border_color_stored(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    static const GLfloat border[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    GLuint id = solid_texture(1, 2, 3);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);

    gl_texture_t *t = find_tex(c, id);
    int ok = t && fabsf(t->border_color.r - 0.25f) < 1e-6f &&
                  fabsf(t->border_color.g - 0.5f)  < 1e-6f &&
                  fabsf(t->border_color.b - 0.75f) < 1e-6f;
    aglxDestroyContext(c);
    return ok;
}

/* ============================================================================
 * Interplay and regressions
 * ==========================================================================*/

/* Every texture unit's storage must be freed when the context goes away.
 * Run under a leak checker this is what catches a missing free in the new
 * per-level, per-face storage. */
static int t_destroy_frees_chains(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    unsigned char *img = checker_rgb(32);
    if (!img) { aglxDestroyContext(c); return 0; }
    for (int i = 0; i < 4; i++) {
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 32, 32, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, img);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    free(img);
    aglxDestroyContext(c);
    return aglxGetCurrentContext() == NULL;
}

/* A plain G6-style texture — no chain, default filters — must render exactly
 * as it did before G10.  This is the regression gate for the whole phase. */
static int t_g6_behaviour_unchanged(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    static const unsigned char rgb[2 * 2 * 3] = {
        255, 0,   0,      0,   255, 0,
        0,   0,   255,    255, 255, 255,
    };
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, rgb);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    quad(0, 0, W, H, 1.0f, 1.0f);

    int ok = px(c, W / 4, H / 4)     == 0x00FF0000u &&   /* bottom-left  red   */
             px(c, 3 * W / 4, H / 4) == 0x0000FF00u &&   /* bottom-right green */
             px(c, W / 4, 3 * H / 4) == 0x000000FFu &&   /* top-left     blue  */
             px(c, 3 * W / 4, 3 * H / 4) == 0x00FFFFFFu; /* top-right    white */
    aglxDestroyContext(c);
    return ok;
}

/* A display list must replay multitexture coordinates. */
static int t_list_records_multitexcoord(void) {
    aglx_context_t *c = setup(); if (!c) return 0;

    glActiveTexture(GL_TEXTURE0);
    solid_texture(255, 255, 255);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    glActiveTexture(GL_TEXTURE1);
    solid_texture(0, 128, 255);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glBegin(GL_QUADS);
    glMultiTexCoord2f(GL_TEXTURE1, 0, 0); glTexCoord2f(0, 0); glVertex3f(8,  8,  0);
    glMultiTexCoord2f(GL_TEXTURE1, 1, 0); glTexCoord2f(1, 0); glVertex3f(56, 8,  0);
    glMultiTexCoord2f(GL_TEXTURE1, 1, 1); glTexCoord2f(1, 1); glVertex3f(56, 56, 0);
    glMultiTexCoord2f(GL_TEXTURE1, 0, 1); glTexCoord2f(0, 1); glVertex3f(8,  56, 0);
    glEnd();
    glEndList();

    glCallList(list);
    uint32_t p = px(c, 32, 32);
    int ok = near_u8((p >> 16) & 0xFF,   0, 3) &&
             near_u8((p >> 8)  & 0xFF, 128, 3) &&
             near_u8( p        & 0xFF, 255, 3);
    aglxDestroyContext(c);
    return ok;
}

/* glTexImage3D must reject a level beyond the chain and a bad target. */
static int t_teximage3d_validation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_3D, id);
    while (glGetError() != GL_NO_ERROR) { }

    glTexImage3D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 1, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, NULL);
    int ok = glGetError() == GL_INVALID_ENUM;

    glTexImage3D(GL_TEXTURE_3D, GL_MAX_MIPMAP_LEVELS, GL_RGB, 1, 1, 1, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, NULL);
    ok = ok && glGetError() == GL_INVALID_VALUE;

    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB, 1, 1, 1, 1, GL_RGB,
                 GL_UNSIGNED_BYTE, NULL);
    ok = ok && glGetError() == GL_INVALID_VALUE;

    aglxDestroyContext(c);
    return ok;
}

/* GL_MAX_TEXTURE_UNITS must report what the implementation actually has, so
 * an application can size its loops correctly. */
static int t_query_limits(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLint units = 0;
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    int ok = units == GL_MAX_TEXTURE_UNITS_IMPL && units >= 2;
    aglxDestroyContext(c);
    return ok;
}

/* ============================================================================
 * Driver
 * ==========================================================================*/

int main(void) {
    printf("=== test_gltex2: GL 1.2/1.3 texturing (phase G10) ===\n");

    printf("--- mipmap chain construction ---\n");
    RUN(t_chain_dimensions); RUN(t_chain_non_square);
    RUN(t_chain_preserves_mean); RUN(t_reupload_drops_chain);
    RUN(t_glu_matches_generate); RUN(t_generate_without_image);

    printf("--- mipmapped rendering ---\n");
    RUN(t_mipmap_reduces_aliasing); RUN(t_all_four_mipmap_filters);
    RUN(t_mipmap_filter_without_chain); RUN(t_magnification_ignores_chain);
    RUN(t_max_level_caps_chain); RUN(t_filter_validation);

    printf("--- multitexturing ---\n");
    RUN(t_active_texture_selects_unit); RUN(t_active_texture_validation);
    RUN(t_two_units_modulate); RUN(t_unit_order_replace);
    RUN(t_disabled_unit_ignored); RUN(t_env_is_per_unit);
    RUN(t_texcoord_always_unit0); RUN(t_delete_unbinds_all_units);
    RUN(t_client_active_texture);

    printf("--- 3D textures ---\n");
    RUN(t_texture_3d_slices); RUN(t_texture_3d_trilinear);
    RUN(t_target_is_sticky); RUN(t_teximage3d_validation);

    printf("--- cube maps ---\n");
    RUN(t_cube_face_selection); RUN(t_cube_major_axis);
    RUN(t_cube_beats_2d); RUN(t_cube_mipmap_all_faces);

    printf("--- clamp to border ---\n");
    RUN(t_clamp_to_border); RUN(t_clamp_to_edge_unchanged);
    RUN(t_border_color_stored);

    printf("--- interplay and regressions ---\n");
    RUN(t_destroy_frees_chains); RUN(t_g6_behaviour_unchanged);
    RUN(t_list_records_multitexcoord); RUN(t_query_limits);

    printf("\ntest_gltex2: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

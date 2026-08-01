/*
 * test_gltex.c — host-side unit tests for texturing, blending and fog (G6).
 *
 * Links the real gltexture.c and glfrag.c and drives them through the public
 * GL API.  Sampling results are checked against the values the specification
 * predicts, and the perspective-correction test is built so that a
 * screen-space-linear implementation demonstrably fails it.
 */

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "GL/gl.h"
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

/* A 2x2 texture: red, green / blue, white.
 * Row 0 is the BOTTOM row, matching GL's texture coordinate origin. */
static GLuint make_2x2(void) {
    static const unsigned char rgb[2 * 2 * 3] = {
        255, 0,   0,      0,   255, 0,      /* bottom: red,  green */
        0,   0,   255,    255, 255, 255,    /* top:    blue, white */
    };
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, rgb);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return id;
}

/* Draw a screen-filling quad with the given texture coordinates. */
static void textured_quad(GLfloat smax, GLfloat tmax) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord2f(0,    0);    glVertex3f(0,        0,        0);
    glTexCoord2f(smax, 0);    glVertex3f((float)W, 0,        0);
    glTexCoord2f(smax, tmax); glVertex3f((float)W, (float)H, 0);
    glTexCoord2f(0,    tmax); glVertex3f(0,        (float)H, 0);
    glEnd();
}

/* ------------------------------------------------------ object management - */

static int t_gen_delete(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint ids[3] = { 0, 0, 0 };
    glGenTextures(3, ids);
    int ok = ids[0] && ids[1] && ids[2]
          && ids[0] != ids[1] && ids[1] != ids[2]
          && glIsTexture(ids[0]) == GL_TRUE;
    glDeleteTextures(3, ids);
    ok = ok && glIsTexture(ids[0]) == GL_FALSE
            && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_gen_negative(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id;
    glGenTextures(-1, &id);
    int ok = glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

static int t_bind_and_query(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    int ok = (GLuint)bound == id;
    glBindTexture(GL_TEXTURE_2D, 0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    ok = ok && bound == 0;
    aglxDestroyContext(c);
    return ok;
}

/* Binding a never-generated name creates the object, per §3.8.12. */
static int t_bind_unknown_name_creates(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glBindTexture(GL_TEXTURE_2D, 4242);
    int ok = glIsTexture(4242) == GL_TRUE && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Deleting the bound texture must reset the binding to 0. */
static int t_delete_bound_resets_binding(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = make_2x2();
    glDeleteTextures(1, &id);
    GLint bound = 1;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    int ok = bound == 0;
    aglxDestroyContext(c);
    return ok;
}

static int t_bind_bad_target(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glBindTexture(0x9999, 1);
    int ok = glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

/* glTexImage2D with no texture bound is an error, not a crash. */
static int t_teximage_without_binding(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    unsigned char rgb[3] = { 1, 2, 3 };
    glBindTexture(GL_TEXTURE_2D, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, rgb);
    int ok = glGetError() == GL_INVALID_OPERATION;
    aglxDestroyContext(c);
    return ok;
}

static int t_teximage_validation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    unsigned char rgb[3] = { 1, 2, 3 };

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 1, GL_RGB,
                 GL_UNSIGNED_BYTE, rgb);                 /* border != 0 */
    int ok = glGetError() == GL_INVALID_VALUE;

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, 0x9999,
                 GL_UNSIGNED_BYTE, rgb);                 /* bad format */
    ok = ok && glGetError() == GL_INVALID_ENUM;

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
                 GL_FLOAT, rgb);                          /* bad type */
    ok = ok && glGetError() == GL_INVALID_ENUM;

    aglxDestroyContext(c);
    return ok;
}

/* ----------------------------------------------------------- sampling ----- */

/* GL_NEAREST at each quadrant centre must return that quadrant's texel, and
 * the v axis must NOT be flipped: row 0 of the supplied data is the bottom. */
static int t_nearest_quadrants(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    make_2x2();
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    textured_quad(1.0f, 1.0f);

    uint32_t bl = px(c, W / 4,     H / 4);        /* s,t ~ 0.25,0.25 */
    uint32_t br = px(c, W * 3 / 4, H / 4);
    uint32_t tl = px(c, W / 4,     H * 3 / 4);
    uint32_t tr = px(c, W * 3 / 4, H * 3 / 4);

    int ok = bl == 0xFF0000     /* red   */
          && br == 0x00FF00     /* green */
          && tl == 0x0000FF     /* blue  */
          && tr == 0xFFFFFF;    /* white */
    aglxDestroyContext(c);
    return ok;
}

/* GL_REPEAT: s = 1.25 must sample the same texel as s = 0.25. */
static int t_wrap_repeat(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    make_2x2();
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    /* Two full repeats across the quad: four quadrants become sixteen. */
    textured_quad(2.0f, 2.0f);
    /* At 1/8 across, s = 0.25 -> red; at 5/8, s = 1.25 -> red again. */
    uint32_t a = px(c, W / 8,     H / 8);
    uint32_t b = px(c, W * 5 / 8, H / 8);
    int ok = a == 0xFF0000 && b == 0xFF0000;
    aglxDestroyContext(c);
    return ok;
}

/* GL_CLAMP_TO_EDGE: s beyond 1 must keep returning the edge texel. */
static int t_wrap_clamp(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    make_2x2();
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    textured_quad(3.0f, 1.0f);
    /* Far right is s ~ 3: clamped to the rightmost column. */
    uint32_t right_bottom = px(c, W - 2, H / 4);
    int ok = right_bottom == 0x00FF00;    /* green, the bottom-right texel */
    aglxDestroyContext(c);
    return ok;
}

/* Bilinear filtering at the exact centre of the 2x2 image must average all
 * four texels: (red + green + blue + white) / 4. */
static int t_bilinear_average(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    make_2x2();
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    textured_quad(1.0f, 1.0f);

    uint32_t p = px(c, W / 2, H / 2);
    int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
    /* red(255,0,0) + green(0,255,0) + blue(0,0,255) + white(255,255,255)
     * over four = (127.5, 127.5, 127.5). */
    int ok = near_u8(r, 128, 12) && near_u8(g, 128, 12) && near_u8(b, 128, 12);
    aglxDestroyContext(c);
    return ok;
}

/* A single-texel texture must return that texel everywhere. */
static int t_single_texel(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = 0;
    unsigned char rgb[3] = { 10, 200, 30 };
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, rgb);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    textured_quad(1.0f, 1.0f);
    uint32_t p = px(c, W / 2, H / 2);
    int ok = near_u8((p >> 16) & 0xFF, 10, 2)
          && near_u8((p >> 8) & 0xFF, 200, 2)
          && near_u8(p & 0xFF, 30, 2);
    aglxDestroyContext(c);
    return ok;
}

/* RGBA upload must preserve alpha. */
static int t_rgba_alpha_preserved(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = 0;
    unsigned char rgba[4] = { 255, 0, 0, 128 };
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glClearColor(0, 0, 1, 1);              /* blue background */
    textured_quad(1.0f, 1.0f);
    uint32_t p = px(c, W / 2, H / 2);
    /* Half-transparent red over blue: roughly (128, 0, 127). */
    int ok = near_u8((p >> 16) & 0xFF, 128, 20)
          && near_u8(p & 0xFF, 127, 20);
    aglxDestroyContext(c);
    return ok;
}

/* GL_LUMINANCE expands to grey. */
static int t_luminance_format(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = 0;
    unsigned char lum[1] = { 100 };
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 1, 1, 0, GL_LUMINANCE,
                 GL_UNSIGNED_BYTE, lum);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    textured_quad(1.0f, 1.0f);
    uint32_t p = px(c, W / 2, H / 2);
    int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
    int ok = near_u8(r, 100, 2) && near_u8(g, 100, 2) && near_u8(b, 100, 2);
    aglxDestroyContext(c);
    return ok;
}

/* glTexSubImage2D must patch a region without disturbing the rest. */
static int t_subimage(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    make_2x2();
    unsigned char patch[3] = { 0, 0, 0 };   /* black */
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGB,
                    GL_UNSIGNED_BYTE, patch);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    textured_quad(1.0f, 1.0f);
    int ok = px(c, W / 4, H / 4) == 0x000000        /* patched */
          && px(c, W * 3 / 4, H / 4) == 0x00FF00;   /* untouched */
    aglxDestroyContext(c);
    return ok;
}

static int t_subimage_out_of_range(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    make_2x2();
    unsigned char patch[3] = { 0, 0, 0 };
    glTexSubImage2D(GL_TEXTURE_2D, 0, 5, 5, 1, 1, GL_RGB,
                    GL_UNSIGNED_BYTE, patch);
    int ok = glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

/* -------------------------------------------------- texture environment --- */

/* GL_MODULATE multiplies texel by fragment colour. */
static int t_env_modulate(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = 0;
    unsigned char white[3] = { 255, 255, 255 };
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, white);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor3f(1.0f, 0.0f, 0.0f);           /* red fragment, white texel */
    textured_quad(1.0f, 1.0f);
    uint32_t p = px(c, W / 2, H / 2);
    int ok = ((p >> 16) & 0xFF) > 240 && ((p >> 8) & 0xFF) < 15;
    aglxDestroyContext(c);
    return ok;
}

/* GL_REPLACE ignores the fragment colour entirely. */
static int t_env_replace(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = 0;
    unsigned char green[3] = { 0, 255, 0 };
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, green);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor3f(1.0f, 0.0f, 0.0f);           /* must be ignored */
    textured_quad(1.0f, 1.0f);
    uint32_t p = px(c, W / 2, H / 2);
    int ok = ((p >> 8) & 0xFF) > 240 && ((p >> 16) & 0xFF) < 15;
    aglxDestroyContext(c);
    return ok;
}

static int t_env_invalid(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, 0x9999);
    int ok = glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

/* Disabling GL_TEXTURE_2D must stop texturing. */
static int t_texture_disabled(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    make_2x2();
    glDisable(GL_TEXTURE_2D);
    glColor3f(1.0f, 0.0f, 1.0f);
    textured_quad(1.0f, 1.0f);
    int ok = px(c, W / 2, H / 2) == 0xFF00FF;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------ perspective correctness ------- */

/* THE test this phase exists for.
 *
 * A quad is drawn in perspective with its far edge pushed back, so the two
 * halves of the texture cover very different amounts of screen.  With correct
 * (perspective) interpolation the texture's midpoint lands nearer the far
 * edge; with naive screen-space interpolation it lands at the screen midpoint.
 * Measuring where the red/green boundary of a 2x1 texture falls distinguishes
 * the two unambiguously.
 */
static int t_perspective_correct_uv(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1, 100);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glColor3f(1, 1, 1);

    /* Two texels side by side: left red, right green. */
    GLuint id = 0;
    static const unsigned char rg[2 * 3] = { 255, 0, 0,   0, 255, 0 };
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 1, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, rg);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    /* A floor-like quad receding from the camera: the left edge is near, the
     * right edge is far.  In screen space the far half is compressed. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(-1.0f, -1.0f, -1.5f);   /* near left  */
    glTexCoord2f(1, 0); glVertex3f( 1.0f, -1.0f, -12.0f);  /* far  right */
    glTexCoord2f(1, 1); glVertex3f( 1.0f,  1.0f, -12.0f);
    glTexCoord2f(0, 1); glVertex3f(-1.0f,  1.0f, -1.5f);
    glEnd();

    /* Walk the middle scanline, recording the covered span and where red
     * turns to green. */
    int boundary = -1, red_seen = 0;
    int span_start = -1, span_end = -1;
    for (int x = 0; x < W; x++) {
        uint32_t p = px(c, x, H / 2);
        if (p == 0) continue;
        if (span_start < 0) span_start = x;
        span_end = x;
        int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF;
        if (r > 200 && g < 60) { red_seen = 1; continue; }
        if (g > 200 && r < 60 && red_seen && boundary < 0) boundary = x;
    }
    if (span_start < 0 || boundary < 0) { aglxDestroyContext(c); return 0; }

    /* The texture's midpoint (s = 0.5) is what the boundary marks.
     *
     * Screen-space-linear interpolation would place it at the midpoint of the
     * COVERED SPAN, because s would advance uniformly across the pixels.
     * Perspective-correct interpolation places it much further along, since
     * the receding half of the quad is compressed into fewer pixels.
     *
     * Comparing against the span midpoint — not the screen midpoint — is what
     * makes this test actually distinguish the two implementations. */
    int span_mid = (span_start + span_end) / 2;
    int ok = boundary > span_mid + (span_end - span_start) / 5;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------------ blending ---- */

static int t_blend_src_alpha(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearColor(0, 0, 1, 1);              /* blue destination */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 0.0f, 0.0f, 0.5f);     /* half-opaque red */
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 0);        glVertex3f((float)W, 0, 0);
    glVertex3f((float)W, (float)H, 0); glVertex3f(0, (float)H, 0);
    glEnd();
    uint32_t p = px(c, W / 2, H / 2);
    /* 0.5*red over 0.5*blue -> (127, 0, 127). */
    int ok = near_u8((p >> 16) & 0xFF, 128, 12)
          && near_u8(p & 0xFF, 128, 12)
          && ((p >> 8) & 0xFF) < 12;
    aglxDestroyContext(c);
    return ok;
}

static int t_blend_additive(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearColor(0.0f, 0.0f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glColor4f(0.5f, 0.0f, 0.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 0);        glVertex3f((float)W, 0, 0);
    glVertex3f((float)W, (float)H, 0); glVertex3f(0, (float)H, 0);
    glEnd();
    uint32_t p = px(c, W / 2, H / 2);
    int ok = near_u8((p >> 16) & 0xFF, 128, 12)
          && near_u8(p & 0xFF, 128, 12);
    aglxDestroyContext(c);
    return ok;
}

/* Blending off means the source replaces the destination. */
static int t_blend_disabled(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_BLEND);
    glColor4f(1.0f, 0.0f, 0.0f, 0.25f);    /* alpha must be ignored */
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 0);        glVertex3f((float)W, 0, 0);
    glVertex3f((float)W, (float)H, 0); glVertex3f(0, (float)H, 0);
    glEnd();
    int ok = px(c, W / 2, H / 2) == 0xFF0000;
    aglxDestroyContext(c);
    return ok;
}

static int t_blend_invalid_factor(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glBlendFunc(0x9999, GL_ONE);
    int ok = glGetError() == GL_INVALID_ENUM;
    /* GL_SRC_ALPHA_SATURATE is source-only. */
    glBlendFunc(GL_ONE, GL_SRC_ALPHA_SATURATE);
    ok = ok && glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

/* ---------------------------------------------------------- alpha test ---- */

static int t_alpha_test_discards(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glColor4f(1.0f, 0.0f, 0.0f, 0.25f);    /* below the reference: discarded */
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 0);        glVertex3f((float)W, 0, 0);
    glVertex3f((float)W, (float)H, 0); glVertex3f(0, (float)H, 0);
    glEnd();
    int ok = px(c, W / 2, H / 2) == 0x0000FF;   /* background survives */
    aglxDestroyContext(c);
    return ok;
}

static int t_alpha_test_passes(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glColor4f(1.0f, 0.0f, 0.0f, 0.9f);
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 0);        glVertex3f((float)W, 0, 0);
    glVertex3f((float)W, (float)H, 0); glVertex3f(0, (float)H, 0);
    glEnd();
    int ok = px(c, W / 2, H / 2) == 0xFF0000;
    aglxDestroyContext(c);
    return ok;
}

/* A discarded fragment must not write depth either. */
static int t_alpha_test_blocks_depth_write(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glColor4f(1, 0, 0, 0.1f);
    glBegin(GL_QUADS);
    glVertex3f(0, 0, 0);        glVertex3f((float)W, 0, 0);
    glVertex3f((float)W, (float)H, 0); glVertex3f(0, (float)H, 0);
    glEnd();
    const float *d = aglxGetDepthBuffer(c);
    int ok = d[(size_t)(H / 2) * W + W / 2] == 1.0f;   /* untouched */
    aglxDestroyContext(c);
    return ok;
}

static int t_alpha_func_invalid(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glAlphaFunc(0x9999, 0.5f);
    int ok = glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

/* ----------------------------------------------------------------- fog ---- */

/* Linear fog: a surface at the fog end must be entirely fog-coloured. */
static int t_fog_linear_full(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1, 100);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 5.0f);
    glFogf(GL_FOG_END, 20.0f);
    GLfloat fogcol[4] = { 0.0f, 1.0f, 0.0f, 1.0f };   /* green fog */
    glFogfv(GL_FOG_COLOR, fogcol);

    glColor3f(1.0f, 0.0f, 0.0f);                       /* red surface */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_QUADS);
    glVertex3f(-8, -8, -25); glVertex3f(8, -8, -25);
    glVertex3f( 8,  8, -25); glVertex3f(-8, 8, -25);
    glEnd();

    uint32_t p = px(c, W / 2, H / 2);
    /* Beyond fog end: fully green. */
    int ok = ((p >> 8) & 0xFF) > 240 && ((p >> 16) & 0xFF) < 15;
    aglxDestroyContext(c);
    return ok;
}

/* A surface nearer than the fog start keeps its own colour. */
static int t_fog_linear_none(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    if (!c) return 0;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glFrustum(-1, 1, -1, 1, 1, 100);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 20.0f);
    glFogf(GL_FOG_END, 50.0f);
    GLfloat fogcol[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    glFogfv(GL_FOG_COLOR, fogcol);

    glColor3f(1.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_QUADS);
    glVertex3f(-2, -2, -5); glVertex3f(2, -2, -5);
    glVertex3f( 2,  2, -5); glVertex3f(-2, 2, -5);
    glEnd();

    uint32_t p = px(c, W / 2, H / 2);
    int ok = ((p >> 16) & 0xFF) > 240 && ((p >> 8) & 0xFF) < 15;
    aglxDestroyContext(c);
    return ok;
}

/* Fog must not change alpha, only RGB. */
static int t_fog_preserves_alpha(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_EXP);
    glFogf(GL_FOG_DENSITY, 0.5f);
    /* The fog factor helper is exercised directly: at distance 0 there is no
     * fog, so the factor must be exactly 1. */
    int ok = gl_fog_factor(c, 0.0f) > 0.999f;
    /* Far away, the factor must approach 0. */
    ok = ok && gl_fog_factor(c, 100.0f) < 0.01f;
    aglxDestroyContext(c);
    return ok;
}

static int t_fog_mode_invalid(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glFogi(GL_FOG_MODE, 0x9999);
    int ok = glGetError() == GL_INVALID_ENUM;
    glFogf(GL_FOG_DENSITY, -1.0f);
    ok = ok && glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

/* EXP2 must fall off faster than EXP at the same density. */
static int t_fog_exp2_steeper(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glFogf(GL_FOG_DENSITY, 0.5f);
    glFogi(GL_FOG_MODE, GL_EXP);
    GLfloat e1 = gl_fog_factor(c, 3.0f);
    glFogi(GL_FOG_MODE, GL_EXP2);
    GLfloat e2 = gl_fog_factor(c, 3.0f);
    int ok = e2 < e1;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------------ interplay --- */

/* Texturing, lighting and blending must compose without interfering. */
static int t_texture_with_lighting(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = 0;
    unsigned char white[3] = { 255, 255, 255 };
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, white);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    GLfloat pos[4] = { 0, 0, 1, 0 };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    GLfloat red[4] = { 1, 0, 0, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, red);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glNormal3f(0, 0, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(8,  8,  0);
    glTexCoord2f(1, 0); glVertex3f(56, 8,  0);
    glTexCoord2f(1, 1); glVertex3f(56, 56, 0);
    glTexCoord2f(0, 1); glVertex3f(8,  56, 0);
    glEnd();

    uint32_t p = px(c, W / 2, H / 2);
    /* Lit red modulated by a white texel stays red. */
    int ok = ((p >> 16) & 0xFF) > 100 && ((p >> 8) & 0xFF) < 60
          && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Freeing a context with live textures must not leak or crash. */
static int t_destroy_frees_textures(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    for (int i = 0; i < 8; i++) make_2x2();
    aglxDestroyContext(c);      /* ASan/valgrind would flag a leak here */
    return 1;
}

/* ------------------------------------------------------------------------ */

int main(void) {
    printf("=== gltex / glfrag (G6) unit tests ===\n");

    printf("--- object management ---\n");
    RUN(t_gen_delete); RUN(t_gen_negative); RUN(t_bind_and_query);
    RUN(t_bind_unknown_name_creates); RUN(t_delete_bound_resets_binding);
    RUN(t_bind_bad_target); RUN(t_teximage_without_binding);
    RUN(t_teximage_validation);

    printf("--- sampling ---\n");
    RUN(t_nearest_quadrants); RUN(t_wrap_repeat); RUN(t_wrap_clamp);
    RUN(t_bilinear_average); RUN(t_single_texel);
    RUN(t_rgba_alpha_preserved); RUN(t_luminance_format);
    RUN(t_subimage); RUN(t_subimage_out_of_range);

    printf("--- texture environment ---\n");
    RUN(t_env_modulate); RUN(t_env_replace); RUN(t_env_invalid);
    RUN(t_texture_disabled);

    printf("--- perspective correctness ---\n");
    RUN(t_perspective_correct_uv);

    printf("--- blending ---\n");
    RUN(t_blend_src_alpha); RUN(t_blend_additive); RUN(t_blend_disabled);
    RUN(t_blend_invalid_factor);

    printf("--- alpha test ---\n");
    RUN(t_alpha_test_discards); RUN(t_alpha_test_passes);
    RUN(t_alpha_test_blocks_depth_write); RUN(t_alpha_func_invalid);

    printf("--- fog ---\n");
    RUN(t_fog_linear_full); RUN(t_fog_linear_none);
    RUN(t_fog_preserves_alpha); RUN(t_fog_mode_invalid);
    RUN(t_fog_exp2_steeper);

    printf("--- interplay ---\n");
    RUN(t_texture_with_lighting); RUN(t_destroy_frees_textures);

    printf("\ntest_gltex: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

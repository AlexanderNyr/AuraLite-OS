/*
 * test_glcoexist.c — the fixed-function and shader paths side by side (G11d).
 *
 * The plan said of this phase: "this is where most of the risk lives: the two
 * paths must not fight over state."  These tests are the systematic search for
 * places where they do.
 *
 * The three questions asked of every piece of shared state are:
 *
 *   1. Does the fixed-function path still behave exactly as it did?  Nine
 *      phases and 330 in-OS checks depend on it.
 *   2. Does fixed-function STATE leak into a shaded draw?  Lighting, fog, the
 *      alpha test and the texture environment have no meaning under ES 2.0,
 *      and a shader that quietly picked them up would be wrong in a way no
 *      application could diagnose.
 *   3. Is every combination that has no defined meaning REFUSED rather than
 *      given an invented one?  A hybrid that renders plausibly here and
 *      differently on real hardware is the worst outcome available.
 *
 * Four real defects were found by asking these, and each has a test below
 * named after the behaviour rather than the bug.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "GL/gl.h"
#include "GL/auraglx.h"
#include "glcontext.h"
#include "glvertex.h"
#include "auragui.h"

void gl_imm_reset(void);

static int tn = 0, passed = 0, failed = 0;

#define CHECK(cond, name) do {                          \
    tn++;                                               \
    if (cond) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", (name)); }  \
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

static int px_is(aglx_context_t *c, int x, int y, int r, int g, int b, int tol) {
    uint32_t p = px(c, x, y);
    return near_u8((int)((p >> 16) & 0xFF), r, tol) &&
           near_u8((int)((p >>  8) & 0xFF), g, tol) &&
           near_u8((int)( p        & 0xFF), b, tol);
}

static aglx_context_t *setup(void) {
    aglx_context_t *c = aglxCreateContext(1, W, H, AGLX_DEPTH);
    if (!c) return NULL;
    aglxMakeCurrent(c);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return c;
}

/* Set up the fixed-function projection over the pixel grid. */
static void ortho_pixels(void) {
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, 0, H, -10, 10);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
}

static GLuint build(const char *vs_src, const char *fs_src, const char *who) {
    GLint ok = 0;
    char log[512];

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(vs, sizeof log, NULL, log);
        printf("        [%s] vertex: %s", who, log);
        return 0;
    }
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(fs, sizeof log, NULL, log);
        printf("        [%s] fragment: %s", who, log);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        printf("        [%s] link: %s", who, log);
        return 0;
    }
    return p;
}

static const GLfloat ndc_quad[16] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 0.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
};

/* A red constant-colour program with a pass-through vertex stage. */
static GLuint red_program(void) {
    return build("attribute vec4 aPos;\nvoid main() { gl_Position = aPos; }\n",
                 "void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n",
                 "red");
}

static void bind_quad(GLuint prog) {
    GLint loc = glGetAttribLocation(prog, "aPos");
    if (loc < 0) return;
    glEnableVertexAttribArray((GLuint)loc);
    glVertexAttribPointer((GLuint)loc, 4, GL_FLOAT, GL_FALSE, 0, ndc_quad);
}

/* Draw a fixed-function quad over the given pixel rectangle. */
static void ff_quad(float x0, float y0, float x1, float y1) {
    glBegin(GL_QUADS);
    glVertex3f(x0, y0, 0); glVertex3f(x1, y0, 0);
    glVertex3f(x1, y1, 0); glVertex3f(x0, y1, 0);
    glEnd();
}

/* ============================================================================
 * Points and lines under a shader
 *
 * Found by audit: every G11c test drew triangles, so nothing exercised
 * gl_raster_point() or gl_raster_line() with a program bound.  Both wrote
 * v->color, which the shader vertex stage leaves at white — a shaded
 * GL_LINE_LOOP came out WHITE instead of running the fragment shader at all.
 * ==========================================================================*/

static void test_points_and_lines(void) {
    printf("--- points and lines under a shader ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    GLuint p = red_program();
    CHECK(p != 0, "the program links");
    if (!p) { aglxDestroyContext(c); return; }

    glUseProgram(p);
    bind_quad(p);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_LINE_LOOP, 0, 4);
    CHECK(px_is(c, 0, 32, 255, 0, 0, 1),
          "a shaded line runs the fragment shader");
    CHECK(!px_is(c, 0, 32, 255, 255, 255, 1),
          "and is not the unshaded white it used to be");

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_POINTS, 0, 4);
    CHECK(px_is(c, 0, 0, 255, 0, 0, 1),
          "a shaded point runs the fragment shader");

    /* Polygon mode routes triangles through the point and line rasterizers,
     * so it exercises the same path from a third direction. */
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(px_is(c, 0, 32, 255, 0, 0, 1),
          "glPolygonMode(GL_LINE) shades its edges");
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    /* A shaded line must interpolate its varyings along the segment, exactly
     * as a triangle interpolates them across its face. */
    GLuint p2 = build(
        "attribute vec4 aPos;\nattribute vec3 aCol;\nvarying vec3 vCol;\n"
        "void main() { vCol = aCol; gl_Position = aPos; }\n",
        "precision mediump float;\nvarying vec3 vCol;\n"
        "void main() { gl_FragColor = vec4(vCol, 1.0); }\n",
        "line varying");
    if (p2) {
        /* A horizontal line from red on the left to blue on the right. */
        static const GLfloat line[8] = {
            -1.0f, 0.0f, 0.0f, 1.0f,
             1.0f, 0.0f, 0.0f, 1.0f,
        };
        static const GLfloat cols[6] = { 1,0,0,  0,0,1 };
        glUseProgram(p2);
        GLint lp = glGetAttribLocation(p2, "aPos");
        GLint lc = glGetAttribLocation(p2, "aCol");
        glEnableVertexAttribArray((GLuint)lp);
        glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0, line);
        glEnableVertexAttribArray((GLuint)lc);
        glVertexAttribPointer((GLuint)lc, 3, GL_FLOAT, GL_FALSE, 0, cols);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_LINES, 0, 2);

        uint32_t left  = px(c, 2, 32);
        uint32_t right = px(c, 61, 32);
        CHECK(((left >> 16) & 0xFF) > 200 && (left & 0xFF) < 60,
              "a shaded line is red at its red end");
        CHECK(((right >> 16) & 0xFF) < 60 && (right & 0xFF) > 200,
              "and blue at its blue end");
        glDisableVertexAttribArray((GLuint)lc);
    }

    aglxDestroyContext(c);
}

/* ============================================================================
 * Combinations with no defined meaning are refused
 * ==========================================================================*/

static void test_refusals(void) {
    printf("--- undefined combinations are refused ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    GLuint p = red_program();
    CHECK(p != 0, "the program links");
    if (!p) { aglxDestroyContext(c); return; }

    ortho_pixels();

    /* Immediate mode with a program bound.
     *
     * glVertex is not an attribute, so a vertex shader has nothing to read
     * from it.  Before G11d the two half-combined: the fixed-function
     * matrices placed the geometry and the fragment shader coloured it — a
     * hybrid no GL implementation produces.  An application that forgot
     * glUseProgram(0) would have seen it render, look right, and behave
     * completely differently on real hardware. */
    glUseProgram(p);
    while (glGetError() != GL_NO_ERROR) { }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_QUADS);
    CHECK(glGetError() == GL_INVALID_OPERATION,
          "glBegin with a program bound is refused");
    glVertex3f(8, 8, 0); glVertex3f(56, 8, 0);
    glVertex3f(56, 56, 0); glVertex3f(8, 56, 0);
    glEnd();
    CHECK(px_is(c, 32, 32, 0, 0, 0, 1),
          "and nothing is drawn by the refused batch");

    /* The draw calls must still work: they open a batch on the application's
     * behalf and feed it through the shader path, so they are exempt. */
    bind_quad(p);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(px_is(c, 32, 32, 255, 0, 0, 1),
          "glDrawArrays is exempt from the immediate-mode rule");

    {
        static const GLushort idx[6] = { 0, 1, 2, 0, 2, 3 };
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
        CHECK(px_is(c, 32, 32, 255, 0, 0, 1),
              "glDrawElements is exempt too");
    }

    /* glUseProgram inside glBegin/glEnd: half a triangle would go through one
     * program and half through another. */
    glUseProgram(0);
    while (glGetError() != GL_NO_ERROR) { }
    glBegin(GL_QUADS);
    glUseProgram(p);
    CHECK(glGetError() == GL_INVALID_OPERATION,
          "glUseProgram inside glBegin/glEnd is refused");
    glEnd();
    glUseProgram(0);

    /* glUseProgram inside a display list.
     *
     * Display lists are GL 1.1 and the shader path is ES 2.0; the two never
     * coexisted in a real implementation, so there is no established meaning.
     * Executing it during GL_COMPILE — which is what happened before — meant
     * compiling a list silently changed the current program, and the next
     * unrelated draw call used a program the application never bound. */
    {
        GLuint dl = glGenLists(1);
        while (glGetError() != GL_NO_ERROR) { }
        glNewList(dl, GL_COMPILE);
        glUseProgram(p);
        CHECK(glGetError() == GL_INVALID_OPERATION,
              "glUseProgram inside a display list is refused");
        glEndList();

        GLint bound = -1;
        glGetIntegerv(GL_CURRENT_PROGRAM, &bound);
        CHECK(bound == 0,
              "and compiling the list did not change the current program");
        glDeleteLists(dl, 1);
    }

    aglxDestroyContext(c);
}

/* ============================================================================
 * Fixed-function state must not leak into a shaded draw
 *
 * Lighting, fog, the alpha test and the texture environment have no meaning
 * under ES 2.0.  A shader that picked them up would be wrong in a way the
 * application could not diagnose, because the state it would be reacting to
 * is state the shader never mentions.
 * ==========================================================================*/

static void test_no_state_leak(void) {
    printf("--- fixed-function state does not leak into a shader ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    GLuint p = red_program();
    if (!p) { CHECK(0, "the program links"); aglxDestroyContext(c); return; }
    glUseProgram(p);
    bind_quad(p);

    /* Lighting on, with a light that would darken everything. */
    {
        GLfloat pos[4] = { 0.0f, 0.0f, -1.0f, 0.0f };
        GLfloat dif[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);
        CHECK(px_is(c, 32, 32, 255, 0, 0, 1),
              "GL_LIGHTING does not affect a shaded fragment");
        glDisable(GL_LIGHTING);
        glDisable(GL_LIGHT0);
    }

    /* Fog on, dense enough to swamp anything it touched. */
    {
        GLfloat fog_col[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
        glEnable(GL_FOG);
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogf(GL_FOG_START, 0.0f);
        glFogf(GL_FOG_END, 0.001f);
        glFogfv(GL_FOG_COLOR, fog_col);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);
        CHECK(px_is(c, 32, 32, 255, 0, 0, 1),
              "GL_FOG does not affect a shaded fragment");
        glDisable(GL_FOG);
    }

    /* An alpha test that would reject every fragment. */
    {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 2.0f);      /* nothing can pass */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);
        CHECK(px_is(c, 32, 32, 255, 0, 0, 1),
              "GL_ALPHA_TEST does not discard a shaded fragment");
        glDisable(GL_ALPHA_TEST);
    }

    /* A texture environment that would replace the colour. */
    {
        static const unsigned char blue[3] = { 0, 0, 255 };
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, blue);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);
        CHECK(px_is(c, 32, 32, 255, 0, 0, 1),
              "the texture environment does not affect a shaded fragment");
        glDisable(GL_TEXTURE_2D);
        glDeleteTextures(1, &t);
    }

    /* GL_FLAT shading is a fixed-function concept; a shader interpolates its
     * varyings regardless. */
    {
        glShadeModel(GL_FLAT);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);
        CHECK(px_is(c, 32, 32, 255, 0, 0, 1),
              "GL_FLAT does not disturb a shaded draw");
        glShadeModel(GL_SMOOTH);
    }

    /* The fixed-function vertex array must not feed a shader: the shader
     * reads GENERIC attributes, addressed by number. */
    {
        static const GLfloat ff[12] = { 0,0,0,  4,0,0,  4,4,0 };
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, ff);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);
        CHECK(px_is(c, 32, 32, 255, 0, 0, 1),
              "glVertexPointer does not feed a shader");
        glDisableClientState(GL_VERTEX_ARRAY);
    }

    /* The MODELVIEW and PROJECTION matrices are equally irrelevant: a vertex
     * shader outputs clip coordinates directly. */
    {
        glMatrixMode(GL_PROJECTION); glLoadIdentity();
        glOrtho(0, 1000, 0, 1000, -1, 1);
        glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
        glTranslatef(500.0f, 500.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);
        CHECK(px_is(c, 32, 32, 255, 0, 0, 1),
              "the fixed-function matrices do not transform a shaded vertex");
        glLoadIdentity();
    }

    aglxDestroyContext(c);
}

/* ============================================================================
 * Framebuffer operations still apply to shaded fragments
 *
 * The other direction: depth, scissor, culling and blending are framebuffer
 * concerns, not shading ones, and a shader must NOT escape them.
 * ==========================================================================*/

static void test_framebuffer_ops_apply(void) {
    printf("--- framebuffer operations still apply ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    GLuint p = red_program();
    if (!p) { CHECK(0, "the program links"); aglxDestroyContext(c); return; }
    glUseProgram(p);
    bind_quad(p);

    /* Scissor. */
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 32, 32);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(px_is(c, 16, 16, 255, 0, 0, 1), "the scissor box keeps what it should");
    CHECK(px_is(c, 48, 48, 0, 0, 0, 1), "and clips what it should");
    glDisable(GL_SCISSOR_TEST);

    /* Culling.  The quad is counter-clockwise, so declaring clockwise the
     * front face culls it entirely. */
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(px_is(c, 32, 32, 0, 0, 0, 1), "culling removes a shaded primitive");
    glFrontFace(GL_CCW);
    glDisable(GL_CULL_FACE);

    /* Depth mask: the colour is written, the depth is not. */
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_QUADS, 0, 4);
    {
        const float *d = aglxGetDepthBuffer(c);
        CHECK(d && d[32 * W + 32] > 0.99f,
              "glDepthMask(GL_FALSE) suppresses the depth write");
        CHECK(px_is(c, 32, 32, 255, 0, 0, 1), "but the colour still lands");
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);

    /* Blending. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_QUADS, 0, 4);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(px_is(c, 32, 32, 255, 0, 0, 1),
          "additive blending accumulates a shaded surface");
    glDisable(GL_BLEND);

    /* Rendering into a framebuffer object. */
    {
        GLuint tex = 0, fbo = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex, 0);
        CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER)
              == GL_FRAMEBUFFER_COMPLETE, "the FBO is complete");

        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);
        unsigned char rgb[3];
        glReadPixels(8, 8, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
        CHECK(rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 0,
              "a shader renders into a framebuffer object");

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
    }

    aglxDestroyContext(c);
}

/* ============================================================================
 * The fixed-function path is unchanged
 *
 * The regression gate for nine phases of work.
 * ==========================================================================*/

static void test_fixed_function_intact(void) {
    printf("--- the fixed-function path is unchanged ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    ortho_pixels();

    /* Before any program exists. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(0, 0, 1);
    ff_quad(8, 8, 56, 56);
    CHECK(px_is(c, 32, 32, 0, 0, 255, 1), "immediate mode draws before");

    GLuint p = red_program();
    CHECK(p != 0, "the program links");
    if (!p) { aglxDestroyContext(c); return; }

    /* Merely creating and linking must change nothing. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(0, 1, 0);
    ff_quad(8, 8, 56, 56);
    CHECK(px_is(c, 32, 32, 0, 255, 0, 1),
          "an unused program does not disturb immediate mode");

    /* Alternate within one frame. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(p);
    bind_quad(p);
    glDrawArrays(GL_QUADS, 0, 4);
    glUseProgram(0);
    glColor3f(0, 0, 1);
    ff_quad(24, 24, 40, 40);
    CHECK(px_is(c, 32, 32, 0, 0, 255, 1),
          "immediate mode draws over a shaded background");
    CHECK(px_is(c, 4, 4, 255, 0, 0, 1), "and the shaded background survives");

    /* Fixed-function lighting, the most state-heavy path. */
    {
        GLfloat pos[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
        GLfloat dif[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
        glColor3f(1, 1, 1);
        glNormal3f(0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ff_quad(8, 8, 56, 56);
        CHECK(((px(c, 32, 32) >> 16) & 0xFF) > 180,
              "fixed-function lighting still lights");
        glDisable(GL_LIGHTING);
        glDisable(GL_LIGHT0);
    }

    /* Fixed-function texturing. */
    {
        static const unsigned char white[3] = { 255, 255, 255 };
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glColor3f(1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex3f(8, 8, 0);
        glTexCoord2f(1, 0); glVertex3f(56, 8, 0);
        glTexCoord2f(1, 1); glVertex3f(56, 56, 0);
        glTexCoord2f(0, 1); glVertex3f(8, 56, 0);
        glEnd();
        CHECK(px_is(c, 32, 32, 255, 0, 255, 1),
              "fixed-function texturing is unaffected");
        glDisable(GL_TEXTURE_2D);
        glDeleteTextures(1, &t);
    }

    /* Fixed-function points and lines, which G11d touched directly. */
    {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glColor3f(1, 1, 0);
        glBegin(GL_LINES);
        glVertex3f(4.5f, 32.5f, 0); glVertex3f(59.5f, 32.5f, 0);
        glEnd();
        CHECK(px_is(c, 32, 32, 255, 255, 0, 1),
              "an unshaded line still uses the vertex colour");

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glColor3f(0, 1, 1);
        glBegin(GL_POINTS);
        glVertex3f(20.5f, 20.5f, 0);
        glEnd();
        CHECK(px_is(c, 20, 20, 0, 255, 255, 1),
              "an unshaded point still uses the vertex colour");
    }

    /* A Gouraud-interpolated line, which the shading branch sits beside. */
    {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBegin(GL_LINES);
        glColor3f(1, 0, 0); glVertex3f(4.5f, 32.5f, 0);
        glColor3f(0, 0, 1); glVertex3f(59.5f, 32.5f, 0);
        glEnd();
        uint32_t left  = px(c, 6, 32);
        uint32_t right = px(c, 57, 32);
        CHECK(((left >> 16) & 0xFF) > 180 && (left & 0xFF) < 80,
              "an unshaded line still interpolates its endpoints");
        CHECK(((right >> 16) & 0xFF) < 80 && (right & 0xFF) > 180,
              "at both ends");
    }

    /* Display lists, vertex arrays and VBOs on the fixed-function path. */
    {
        GLuint dl = glGenLists(1);
        glNewList(dl, GL_COMPILE);
        glColor3f(1, 0, 0);
        ff_quad(8, 8, 56, 56);
        glEndList();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glCallList(dl);
        CHECK(px_is(c, 32, 32, 255, 0, 0, 1),
              "a display list still replays");
        glDeleteLists(dl, 1);
    }

    {
        static const GLfloat verts[12] = {
            8, 8, 0,  56, 8, 0,  56, 56, 0,  8, 56, 0
        };
        glColor3f(0, 1, 0);
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, verts);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);
        CHECK(px_is(c, 32, 32, 0, 255, 0, 1),
              "a fixed-function vertex array still draws");
        glDisableClientState(GL_VERTEX_ARRAY);
    }

    aglxDestroyContext(c);
}

/* ============================================================================
 * Switching between the paths
 * ==========================================================================*/

static void test_switching(void) {
    printf("--- switching between the paths ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    GLuint red = red_program();
    GLuint green = build(
        "attribute vec4 aPos;\nvoid main() { gl_Position = aPos; }\n",
        "void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n",
        "green");
    CHECK(red != 0 && green != 0, "both programs link");
    if (!red || !green) { aglxDestroyContext(c); return; }

    ortho_pixels();

    /* Switching between two programs within a frame. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(red);
    bind_quad(red);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(px_is(c, 32, 32, 255, 0, 0, 1), "the first program draws");

    glUseProgram(green);
    bind_quad(green);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(px_is(c, 32, 32, 0, 255, 0, 1), "the second program takes over");

    /* Attribute arrays are context state, not program state: they survive a
     * program switch, which is what lets an application bind its geometry
     * once and draw it with several programs. */
    glUseProgram(0);
    glUseProgram(red);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(px_is(c, 32, 32, 255, 0, 0, 1),
          "attribute arrays survive a program switch");

    /* Uniform values belong to the program and survive unbinding it. */
    {
        GLuint p = build(
            "attribute vec4 aPos;\nvoid main() { gl_Position = aPos; }\n",
            "precision mediump float;\nuniform vec3 uC;\n"
            "void main() { gl_FragColor = vec4(uC, 1.0); }\n",
            "uniform persistence");
        if (p) {
            glUseProgram(p);
            bind_quad(p);
            glUniform3f(glGetUniformLocation(p, "uC"), 0.0f, 0.0f, 1.0f);
            glUseProgram(0);
            glUseProgram(p);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDrawArrays(GL_QUADS, 0, 4);
            CHECK(px_is(c, 32, 32, 0, 0, 255, 1),
                  "uniform values survive unbinding the program");
        }
    }

    /* Deleting the bound program reverts to fixed function. */
    glUseProgram(red);
    glDeleteProgram(red);
    {
        GLint bound = -1;
        glGetIntegerv(GL_CURRENT_PROGRAM, &bound);
        CHECK(bound == 0, "deleting the bound program unbinds it");
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(0, 1, 1);
    ff_quad(8, 8, 56, 56);
    CHECK(px_is(c, 32, 32, 0, 255, 255, 1),
          "and immediate mode works immediately afterwards");

    /* Resizing the context with a program bound. */
    {
        glUseProgram(green);
        bind_quad(green);
        CHECK(aglxResize(c, 32, 32) == 0, "the context resizes");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);
        const uint32_t *cb = aglxGetColorBuffer(c);
        CHECK((cb[(size_t)(32 - 1 - 16) * 32 + 16] & 0x00FFFFFF) == 0x0000FF00u,
              "and the shader draws into the resized buffer");
        aglxResize(c, W, H);
    }

    /* Programs are per-context: a fresh context sees none of them. */
    {
        aglx_context_t *c2 = aglxCreateContext(1, 32, 32, AGLX_DEPTH);
        aglxMakeCurrent(c2);
        GLint bound = -1;
        glGetIntegerv(GL_CURRENT_PROGRAM, &bound);
        CHECK(bound == 0, "a fresh context has no program bound");
        CHECK(glIsProgram(green) == GL_FALSE,
              "and cannot see another context's programs");
        aglxDestroyContext(c2);
        aglxMakeCurrent(c);
    }

    aglxDestroyContext(c);
}

/* ============================================================================
 * Limits and error paths shared by both
 * ==========================================================================*/

static void test_limits(void) {
    printf("--- shared limits ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    /* The varying budget is a hard limit and must be diagnosed by name. */
    {
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        const char *v =
            "attribute vec4 aPos;\n"
            "varying vec4 a; varying vec4 b; varying vec4 cc; varying vec4 d;\n"
            "varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;\n"
            "varying vec4 i; varying vec4 j;\n"
            "void main() { a=b=cc=d=e=f=g=h=i=j=vec4(1.0); gl_Position=aPos; }\n";
        glShaderSource(vs, 1, &v, NULL);
        glCompileShader(vs);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        const char *f =
            "precision mediump float;\n"
            "varying vec4 a; varying vec4 b; varying vec4 cc; varying vec4 d;\n"
            "varying vec4 e; varying vec4 f; varying vec4 g; varying vec4 h;\n"
            "varying vec4 i; varying vec4 j;\n"
            "void main() { gl_FragColor = a+b+cc+d+e+f+g+h+i+j; }\n";
        glShaderSource(fs, 1, &f, NULL);
        glCompileShader(fs);
        GLuint p = glCreateProgram();
        glAttachShader(p, vs); glAttachShader(p, fs);
        glLinkProgram(p);
        GLint ok = 1;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        CHECK(ok == GL_FALSE, "exceeding the varying budget fails the link");
        char log[512];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        CHECK(strstr(log, "budget") != NULL,
              "and the diagnostic says what was exceeded");
    }

    /* A shader sampling a unit with nothing bound gets black, not garbage. */
    {
        GLuint p = build(
            "attribute vec4 aPos;\nvoid main() { gl_Position = aPos; }\n",
            "precision mediump float;\nuniform sampler2D uT;\n"
            "void main() { gl_FragColor = texture2D(uT, vec2(0.5)); }\n",
            "unbound sampler");
        if (p) {
            glUseProgram(p);
            bind_quad(p);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDrawArrays(GL_QUADS, 0, 4);
            CHECK(px_is(c, 32, 32, 0, 0, 0, 1),
                  "sampling an unbound unit yields black");
            CHECK(glGetError() == GL_NO_ERROR, "and raises no error");
        }
    }

    /* A sampler pointing beyond the implementation's texture units. */
    {
        GLuint p = build(
            "attribute vec4 aPos;\nvoid main() { gl_Position = aPos; }\n",
            "precision mediump float;\nuniform sampler2D uT;\n"
            "void main() { gl_FragColor = texture2D(uT, vec2(0.5)); }\n",
            "out-of-range sampler");
        if (p) {
            glUseProgram(p);
            bind_quad(p);
            glUniform1i(glGetUniformLocation(p, "uT"), 99);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDrawArrays(GL_QUADS, 0, 4);
            CHECK(px_is(c, 32, 32, 0, 0, 0, 1),
                  "a sampler beyond the unit count yields black, not a fault");
        }
    }

    aglxDestroyContext(c);
}

/* ============================================================================
 * Driver
 * ==========================================================================*/

int main(void) {
    printf("=== test_glcoexist: fixed function and shaders together (G11d) ===\n");

    test_points_and_lines();
    test_refusals();
    test_no_state_leak();
    test_framebuffer_ops_apply();
    test_fixed_function_intact();
    test_switching();
    test_limits();

    printf("\ntest_glcoexist: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

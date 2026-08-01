/*
 * test_glprog.c — host-side unit tests for the shader pipeline (phase G11c).
 *
 * This is the phase where shaders stop being a library and become GL: the
 * tests therefore drive the PUBLIC API — glCreateShader, glLinkProgram,
 * glUseProgram, glVertexAttribPointer, glUniform* — and check rendered
 * pixels, not internal state.
 *
 * Two properties matter most and are tested hardest:
 *
 *   1. A shader draws the right pixels.  Anything less is a shader stack that
 *      compiles and renders nothing, which is worse than no shader stack.
 *   2. The fixed-function path is unchanged.  Nine phases and 289 in-OS
 *      checks depend on it, so every shader test is followed by evidence that
 *      turning the program off restores the old behaviour exactly.
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

/* Build a program, reporting whichever stage failed and why.  A test that
 * says only "the program did not link" costs an hour; the log says which
 * line of which shader. */
static GLuint build(const char *vs_src, const char *fs_src, const char *who) {
    GLint ok = 0;
    char log[512];

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(vs, sizeof log, NULL, log);
        printf("        [%s] vertex shader: %s", who, log);
        return 0;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(fs, sizeof log, NULL, log);
        printf("        [%s] fragment shader: %s", who, log);
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

/* A full-screen quad in normalised device coordinates. */
static const GLfloat ndc_quad[16] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 0.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
};

static void draw_ndc_quad(GLuint prog, const char *attrib) {
    GLint loc = glGetAttribLocation(prog, attrib);
    if (loc < 0) return;
    glEnableVertexAttribArray((GLuint)loc);
    glVertexAttribPointer((GLuint)loc, 4, GL_FLOAT, GL_FALSE, 0, ndc_quad);
    glDrawArrays(GL_QUADS, 0, 4);
}

/* ============================================================================
 * Shader objects
 * ==========================================================================*/

static void test_shader_objects(void) {
    printf("--- shader objects ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    GLuint s = glCreateShader(GL_VERTEX_SHADER);
    CHECK(s != 0, "glCreateShader returns a name");
    CHECK(glIsShader(s) == GL_TRUE, "glIsShader recognises it");
    CHECK(glIsShader(9999) == GL_FALSE, "glIsShader rejects a stranger");

    GLint type = 0;
    glGetShaderiv(s, GL_SHADER_TYPE, &type);
    CHECK(type == GL_VERTEX_SHADER, "the shader remembers its type");

    while (glGetError() != GL_NO_ERROR) { }
    glCreateShader(GL_TRIANGLES);
    CHECK(glGetError() == GL_INVALID_ENUM, "a bad shader type is refused");

    /* Compiling without source must fail with something to read. */
    glCompileShader(s);
    GLint ok = 1;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    CHECK(ok == GL_FALSE, "compiling with no source fails");
    GLint len = 0;
    glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
    CHECK(len > 1, "and leaves an info log");

    const char *src = "void main() { gl_Position = vec4(0.0); }\n";
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    CHECK(ok == GL_TRUE, "a valid shader compiles");
    glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
    CHECK(len <= 1, "a clean compile leaves an empty log");

    /* Concatenation: GL joins the strings, which is how applications prepend
     * a version line or a block of shared definitions. */
    GLuint s2 = glCreateShader(GL_FRAGMENT_SHADER);
    const char *parts[3] = {
        "precision mediump float;\n",
        "const float K = 0.5;\n",
        "void main() { gl_FragColor = vec4(K); }\n"
    };
    glShaderSource(s2, 3, parts, NULL);
    glCompileShader(s2);
    glGetShaderiv(s2, GL_COMPILE_STATUS, &ok);
    CHECK(ok == GL_TRUE, "glShaderSource concatenates its strings");

    /* An explicit length array, including a substring. */
    GLuint s3 = glCreateShader(GL_FRAGMENT_SHADER);
    const char *long_src = "void main() { gl_FragColor = vec4(1.0); }\nGARBAGE";
    GLint lengths[1] = { (GLint)strlen(long_src) - 7 };
    glShaderSource(s3, 1, &long_src, lengths);
    glCompileShader(s3);
    glGetShaderiv(s3, GL_COMPILE_STATUS, &ok);
    CHECK(ok == GL_TRUE, "glShaderSource honours an explicit length");

    /* A bad shader reports where and why. */
    GLuint bad = glCreateShader(GL_FRAGMENT_SHADER);
    const char *bad_src = "void main() {\n  float f = 1;\n"
                          "  gl_FragColor = vec4(f);\n}\n";
    glShaderSource(bad, 1, &bad_src, NULL);
    glCompileShader(bad);
    glGetShaderiv(bad, GL_COMPILE_STATUS, &ok);
    CHECK(ok == GL_FALSE, "a type error fails to compile");
    char log[256];
    glGetShaderInfoLog(bad, sizeof log, NULL, log);
    CHECK(strstr(log, "0:2:") != NULL, "the log carries the line number");
    CHECK(strstr(log, "implicit conversion") != NULL,
          "the log explains the rule");

    /* The buffer contract: at most bufSize bytes, always terminated. */
    char tiny[8];
    GLsizei got = 0;
    memset(tiny, 0x7F, sizeof tiny);
    glGetShaderInfoLog(bad, (GLsizei)sizeof tiny, &got, tiny);
    CHECK(tiny[7] == '\0' && got == 7,
          "glGetShaderInfoLog respects a short buffer");

    /* Re-specifying source invalidates the compiled result. */
    glShaderSource(s, 1, &src, NULL);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    CHECK(ok == GL_FALSE, "new source clears the compile status");

    glDeleteShader(s2);
    CHECK(glIsShader(s2) == GL_FALSE, "glDeleteShader removes it");

    aglxDestroyContext(c);
}

/* ============================================================================
 * Programs and linking
 * ==========================================================================*/

static void test_linking(void) {
    printf("--- linking ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    GLuint p = glCreateProgram();
    CHECK(p != 0 && glIsProgram(p) == GL_TRUE, "glCreateProgram");

    /* Linking with nothing attached must say so. */
    glLinkProgram(p);
    GLint ok = 1;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    CHECK(ok == GL_FALSE, "an empty program does not link");
    char log[512];
    glGetProgramInfoLog(p, sizeof log, NULL, log);
    CHECK(strstr(log, "vertex and a fragment") != NULL,
          "and says what is missing");

    /* Using an unlinked program is an error, not a silent fallback: falling
     * back to fixed function would render the scene wrongly with no clue. */
    while (glGetError() != GL_NO_ERROR) { }
    glUseProgram(p);
    CHECK(glGetError() == GL_INVALID_OPERATION,
          "an unlinked program cannot be used");

    /* A program whose shaders did not compile. */
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    const char *broken = "void main() { nonsense }\n";
    glShaderSource(vs, 1, &broken, NULL);
    glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char *fs_ok = "void main() { gl_FragColor = vec4(1.0); }\n";
    glShaderSource(fs, 1, &fs_ok, NULL);
    glCompileShader(fs);
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    CHECK(ok == GL_FALSE, "a program with an uncompiled shader does not link");

    GLint attached = 0;
    glGetProgramiv(p, GL_ATTACHED_SHADERS, &attached);
    CHECK(attached == 2, "glGetProgramiv counts the attached shaders");

    /* Attaching the same shader twice is an error. */
    while (glGetError() != GL_NO_ERROR) { }
    glAttachShader(p, fs);
    CHECK(glGetError() == GL_INVALID_OPERATION,
          "attaching the same shader twice is refused");

    /* A varying the fragment shader reads and the vertex shader never
     * declares must be diagnosed: otherwise it silently reads zeros, and the
     * scene renders black for no visible reason. */
    GLuint p2 = build(
        "attribute vec4 aPos;\nvoid main() { gl_Position = aPos; }\n",
        "precision mediump float;\nvarying vec3 vMissing;\n"
        "void main() { gl_FragColor = vec4(vMissing, 1.0); }\n",
        "missing varying");
    CHECK(p2 == 0, "an undeclared varying fails the link");

    /* Building it properly, to check the diagnostic names the varying. */
    {
        GLuint v = glCreateShader(GL_VERTEX_SHADER);
        const char *vsrc = "attribute vec4 aPos;\n"
                           "void main() { gl_Position = aPos; }\n";
        glShaderSource(v, 1, &vsrc, NULL); glCompileShader(v);
        GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
        const char *fsrc = "precision mediump float;\nvarying vec3 vMissing;\n"
                           "void main() { gl_FragColor = vec4(vMissing, 1.0); }\n";
        glShaderSource(f, 1, &fsrc, NULL); glCompileShader(f);
        GLuint pr = glCreateProgram();
        glAttachShader(pr, v); glAttachShader(pr, f);
        glLinkProgram(pr);
        glGetProgramInfoLog(pr, sizeof log, NULL, log);
        CHECK(strstr(log, "vMissing") != NULL,
              "the link error names the varying");
    }

    /* A varying whose type differs between the stages. */
    {
        GLuint v = glCreateShader(GL_VERTEX_SHADER);
        const char *vsrc = "attribute vec4 aPos;\nvarying vec2 vC;\n"
                           "void main() { vC = vec2(1.0); gl_Position = aPos; }\n";
        glShaderSource(v, 1, &vsrc, NULL); glCompileShader(v);
        GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
        const char *fsrc = "precision mediump float;\nvarying vec3 vC;\n"
                           "void main() { gl_FragColor = vec4(vC, 1.0); }\n";
        glShaderSource(f, 1, &fsrc, NULL); glCompileShader(f);
        GLuint pr = glCreateProgram();
        glAttachShader(pr, v); glAttachShader(pr, f);
        glLinkProgram(pr);
        glGetProgramiv(pr, GL_LINK_STATUS, &ok);
        CHECK(ok == GL_FALSE, "a varying type mismatch fails the link");
        glGetProgramInfoLog(pr, sizeof log, NULL, log);
        CHECK(strstr(log, "vec2") && strstr(log, "vec3"),
              "and names both types");
    }

    /* A uniform declared in both stages is ONE uniform, sharing a location. */
    {
        GLuint pr = build(
            "attribute vec4 aPos;\nuniform vec3 uShared;\n"
            "void main() { gl_Position = aPos + vec4(uShared, 0.0); }\n",
            "precision mediump float;\nuniform vec3 uShared;\n"
            "void main() { gl_FragColor = vec4(uShared, 1.0); }\n",
            "shared uniform");
        CHECK(pr != 0, "a uniform in both stages links");
        if (pr) {
            GLint n = 0;
            glGetProgramiv(pr, GL_ACTIVE_UNIFORMS, &n);
            CHECK(n == 1, "and counts as one uniform, not two");
        }
    }

    /* Disagreeing about a shared uniform's type must fail. */
    {
        GLuint v = glCreateShader(GL_VERTEX_SHADER);
        const char *vsrc = "attribute vec4 aPos;\nuniform vec3 uU;\n"
                           "void main() { gl_Position = aPos + vec4(uU,0.0); }\n";
        glShaderSource(v, 1, &vsrc, NULL); glCompileShader(v);
        GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
        const char *fsrc = "precision mediump float;\nuniform vec4 uU;\n"
                           "void main() { gl_FragColor = uU; }\n";
        glShaderSource(f, 1, &fsrc, NULL); glCompileShader(f);
        GLuint pr = glCreateProgram();
        glAttachShader(pr, v); glAttachShader(pr, f);
        glLinkProgram(pr);
        glGetProgramiv(pr, GL_LINK_STATUS, &ok);
        CHECK(ok == GL_FALSE, "a uniform type mismatch fails the link");
    }

    aglxDestroyContext(c);
}

/* ============================================================================
 * Rendering
 * ==========================================================================*/

static void test_rendering(void) {
    printf("--- rendering ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    /* The simplest possible shader pair: a pass-through vertex shader and a
     * constant fragment shader. */
    GLuint p = build(
        "attribute vec4 aPos;\nvoid main() { gl_Position = aPos; }\n",
        "void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n",
        "constant colour");
    CHECK(p != 0, "the simplest program links");
    if (!p) { aglxDestroyContext(c); return; }

    glUseProgram(p);
    GLint bound = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &bound);
    CHECK(bound == (GLint)p, "glGetIntegerv reports the bound program");

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    draw_ndc_quad(p, "aPos");
    CHECK(px_is(c, 32, 32, 255, 0, 0, 1), "a shader fills the framebuffer");
    CHECK(px_is(c, 2, 2, 255, 0, 0, 1), "including the corners");
    CHECK(glGetError() == GL_NO_ERROR, "no error was raised");

    /* Varyings, interpolated across the primitive.  A per-corner colour is
     * the clearest way to prove the interpolation runs and is not, say,
     * flat-shading the first vertex. */
    GLuint p2 = build(
        "attribute vec4 aPos;\nattribute vec3 aCol;\nvarying vec3 vCol;\n"
        "void main() { vCol = aCol; gl_Position = aPos; }\n",
        "precision mediump float;\nvarying vec3 vCol;\n"
        "void main() { gl_FragColor = vec4(vCol, 1.0); }\n",
        "varying colour");
    CHECK(p2 != 0, "a program with a varying links");
    if (p2) {
        static const GLfloat cols[12] = {
            1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 1.0f,
            1.0f, 1.0f, 0.0f,
        };
        glUseProgram(p2);
        GLint lp = glGetAttribLocation(p2, "aPos");
        GLint lc = glGetAttribLocation(p2, "aCol");
        CHECK(lp >= 0 && lc >= 0 && lp != lc,
              "the two attributes get distinct locations");

        glEnableVertexAttribArray((GLuint)lp);
        glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0, ndc_quad);
        glEnableVertexAttribArray((GLuint)lc);
        glVertexAttribPointer((GLuint)lc, 3, GL_FLOAT, GL_FALSE, 0, cols);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);

        CHECK(px_is(c, 2, 2, 245, 0, 10, 24),
              "the bottom-left corner takes its own colour");
        CHECK(px_is(c, 61, 2, 10, 235, 10, 24),
              "the bottom-right corner takes its own colour");
        CHECK(px_is(c, 61, 61, 10, 0, 245, 24),
              "the top-right corner takes its own colour");

        /* A point inside the quad must be a BLEND, not any one corner's
         * colour.  Sampled off the diagonal on purpose: a quad is drawn as
         * two triangles, and a pixel exactly on the shared edge sees only the
         * two corners its triangle owns -- the centre of this quad genuinely
         * has no green in it, which looks like a bug and is not.
         *
         * (24,40) sits inside the top-left triangle, whose corners are red,
         * blue and yellow, so all three contribute. */
        uint32_t mid = px(c, 24, 40);
        int mr = (int)((mid >> 16) & 0xFF), mg = (int)((mid >> 8) & 0xFF);
        int mb = (int)(mid & 0xFF);
        CHECK(mr > 30 && mr < 220 && mb > 30 && mg > 30,
              "an interior pixel blends its triangle's three corners");

        /* And a pixel near each corner is dominated by that corner, which is
         * what proves the interpolation is oriented correctly rather than
         * merely varying. */
        CHECK((int)((px(c, 4, 4) >> 16) & 0xFF) > 200 &&
              (int)(px(c, 4, 4) & 0xFF) < 60,
              "the interpolation is oriented, not merely varying");

        glDisableVertexAttribArray((GLuint)lc);
    }

    /* Uniforms driving both stages. */
    GLuint p3 = build(
        "attribute vec4 aPos;\nuniform float uScale;\n"
        "void main() { gl_Position = vec4(aPos.xy * uScale, 0.0, 1.0); }\n",
        "precision mediump float;\nuniform vec3 uColor;\n"
        "void main() { gl_FragColor = vec4(uColor, 1.0); }\n",
        "uniforms");
    CHECK(p3 != 0, "a program with uniforms links");
    if (p3) {
        glUseProgram(p3);
        GLint uc = glGetUniformLocation(p3, "uColor");
        GLint us = glGetUniformLocation(p3, "uScale");
        CHECK(uc >= 0 && us >= 0, "both uniforms have locations");
        CHECK(glGetUniformLocation(p3, "uNothing") == -1,
              "an unknown uniform reports -1");

        glUniform3f(uc, 0.0f, 1.0f, 0.0f);
        glUniform1f(us, 0.5f);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw_ndc_quad(p3, "aPos");

        /* Half scale: the quad covers the middle half of the window. */
        CHECK(px_is(c, 32, 32, 0, 255, 0, 1), "the uniform colour is used");
        CHECK(px_is(c, 2, 2, 0, 0, 0, 1),
              "the uniform scale shrank the geometry");

        /* Changing a uniform between draws must take effect. */
        glUniform3f(uc, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw_ndc_quad(p3, "aPos");
        CHECK(px_is(c, 32, 32, 0, 0, 255, 1), "a changed uniform takes effect");

        /* A size mismatch must be refused rather than partly applied. */
        while (glGetError() != GL_NO_ERROR) { }
        glUniform2f(uc, 1.0f, 1.0f);
        CHECK(glGetError() == GL_INVALID_OPERATION,
              "a uniform size mismatch is refused");

        /* Location -1 is a defined no-op: applications pass it unchecked. */
        while (glGetError() != GL_NO_ERROR) { }
        glUniform1f(-1, 1.0f);
        CHECK(glGetError() == GL_NO_ERROR, "writing to location -1 is a no-op");
    }

    /* A matrix uniform, in the column-major order glUniformMatrix uses. */
    GLuint p4 = build(
        "attribute vec4 aPos;\nuniform mat4 uM;\n"
        "void main() { gl_Position = uM * aPos; }\n",
        "void main() { gl_FragColor = vec4(1.0, 1.0, 0.0, 1.0); }\n",
        "matrix uniform");
    CHECK(p4 != 0, "a program with a matrix uniform links");
    if (p4) {
        /* Scale by a half and shift right, so the covered region is
         * unambiguous and asymmetric. */
        static const GLfloat m[16] = {
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.5f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.5f, 0.0f, 0.0f, 1.0f,
        };
        glUseProgram(p4);
        glUniformMatrix4fv(glGetUniformLocation(p4, "uM"), 1, GL_FALSE, m);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw_ndc_quad(p4, "aPos");

        /* The quad now spans x in [0,1] NDC, y in [-0.5,0.5]. */
        CHECK(px_is(c, 48, 32, 255, 255, 0, 1),
              "a matrix uniform transforms the geometry");
        CHECK(px_is(c, 8, 32, 0, 0, 0, 1),
              "and the untouched region stays clear");
    }

    /* discard in a fragment shader must drop the fragment. */
    GLuint p5 = build(
        "attribute vec4 aPos;\nvarying vec2 vP;\n"
        "void main() { vP = aPos.xy; gl_Position = aPos; }\n",
        "precision mediump float;\nvarying vec2 vP;\n"
        "void main() {\n  if (vP.x > 0.0) discard;\n"
        "  gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);\n}\n",
        "discard");
    CHECK(p5 != 0, "a discarding shader links");
    if (p5) {
        glUseProgram(p5);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw_ndc_quad(p5, "aPos");
        CHECK(px_is(c, 8, 32, 255, 0, 255, 1),
              "the kept half of the quad is drawn");
        CHECK(px_is(c, 56, 32, 0, 0, 0, 1),
              "the discarded half is not");
    }

    /* The depth test still applies to shaded fragments. */
    GLuint p6 = build(
        "attribute vec4 aPos;\nuniform float uZ;\n"
        "void main() { gl_Position = vec4(aPos.xy, uZ, 1.0); }\n",
        "precision mediump float;\nuniform vec3 uC;\n"
        "void main() { gl_FragColor = vec4(uC, 1.0); }\n",
        "depth");
    CHECK(p6 != 0, "the depth-test program links");
    if (p6) {
        glUseProgram(p6);
        GLint uz = glGetUniformLocation(p6, "uZ");
        GLint uc = glGetUniformLocation(p6, "uC");
        glEnable(GL_DEPTH_TEST);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUniform1f(uz, -0.5f);          /* nearer */
        glUniform3f(uc, 1.0f, 0.0f, 0.0f);
        draw_ndc_quad(p6, "aPos");

        glUniform1f(uz, 0.5f);           /* farther */
        glUniform3f(uc, 0.0f, 0.0f, 1.0f);
        draw_ndc_quad(p6, "aPos");

        CHECK(px_is(c, 32, 32, 255, 0, 0, 1),
              "the depth test rejects the farther shaded quad");
        glDisable(GL_DEPTH_TEST);
    }

    /* Blending still applies too. */
    if (p3) {
        glUseProgram(p3);
        glUniform1f(glGetUniformLocation(p3, "uScale"), 1.0f);
        glUniform3f(glGetUniformLocation(p3, "uColor"), 1.0f, 1.0f, 1.0f);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        draw_ndc_quad(p3, "aPos");
        draw_ndc_quad(p3, "aPos");
        glDisable(GL_BLEND);
        CHECK(px_is(c, 32, 32, 255, 255, 255, 1),
              "additive blending saturates a shaded surface");
    }

    aglxDestroyContext(c);
}

/* ============================================================================
 * Attributes
 * ==========================================================================*/

static void test_attributes(void) {
    printf("--- vertex attributes ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    GLuint p = build(
        "attribute vec4 aPos;\nattribute vec4 aCol;\nvarying vec4 vCol;\n"
        "void main() { vCol = aCol; gl_Position = aPos; }\n",
        "precision mediump float;\nvarying vec4 vCol;\n"
        "void main() { gl_FragColor = vCol; }\n",
        "attributes");
    CHECK(p != 0, "the attribute program links");
    if (!p) { aglxDestroyContext(c); return; }

    glUseProgram(p);
    GLint lp = glGetAttribLocation(p, "aPos");
    GLint lc = glGetAttribLocation(p, "aCol");

    /* A DISABLED array supplies the generic value.  GL's default is
     * (0,0,0,1), so a shader reading an attribute nobody supplied gets a
     * valid homogeneous point rather than a degenerate one. */
    glEnableVertexAttribArray((GLuint)lp);
    glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0, ndc_quad);
    glDisableVertexAttribArray((GLuint)lc);
    glVertexAttrib4f((GLuint)lc, 0.0f, 1.0f, 1.0f, 1.0f);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(px_is(c, 32, 32, 0, 255, 255, 1),
          "a disabled array uses the generic attribute value");

    /* Normalised unsigned bytes, the commonest packed colour format. */
    static const unsigned char bytes[16] = {
        255, 0, 0, 255,
        255, 0, 0, 255,
        255, 0, 0, 255,
        255, 0, 0, 255,
    };
    glEnableVertexAttribArray((GLuint)lc);
    glVertexAttribPointer((GLuint)lc, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, bytes);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(px_is(c, 32, 32, 255, 0, 0, 2),
          "a normalised unsigned-byte attribute is scaled to [0,1]");

    /* A three-component array feeding a vec4 attribute: w defaults to 1. */
    GLuint p2 = build(
        "attribute vec4 aPos;\nvarying float vW;\n"
        "void main() { vW = aPos.w; gl_Position = vec4(aPos.xy, 0.0, 1.0); }\n",
        "precision mediump float;\nvarying float vW;\n"
        "void main() { gl_FragColor = vec4(vW, 0.0, 0.0, 1.0); }\n",
        "short array");
    if (p2) {
        static const GLfloat xyz[12] = {
            -1.0f, -1.0f, 0.0f,
             1.0f, -1.0f, 0.0f,
             1.0f,  1.0f, 0.0f,
            -1.0f,  1.0f, 0.0f,
        };
        glUseProgram(p2);
        GLint l = glGetAttribLocation(p2, "aPos");
        glEnableVertexAttribArray((GLuint)l);
        glVertexAttribPointer((GLuint)l, 3, GL_FLOAT, GL_FALSE, 0, xyz);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);
        CHECK(px_is(c, 32, 32, 255, 0, 0, 2),
              "a 3-component array leaves w at 1.0");
    }

    /* Interleaved data with a stride. */
    GLuint p3 = build(
        "attribute vec4 aPos;\nattribute vec3 aCol;\nvarying vec3 vCol;\n"
        "void main() { vCol = aCol; gl_Position = aPos; }\n",
        "precision mediump float;\nvarying vec3 vCol;\n"
        "void main() { gl_FragColor = vec4(vCol, 1.0); }\n",
        "interleaved");
    if (p3) {
        /* x,y,z,w,r,g,b per vertex. */
        static const GLfloat inter[28] = {
            -1,-1,0,1,  0,1,0,
             1,-1,0,1,  0,1,0,
             1, 1,0,1,  0,1,0,
            -1, 1,0,1,  0,1,0,
        };
        glUseProgram(p3);
        GLint la = glGetAttribLocation(p3, "aPos");
        GLint lb = glGetAttribLocation(p3, "aCol");
        glEnableVertexAttribArray((GLuint)la);
        glVertexAttribPointer((GLuint)la, 4, GL_FLOAT, GL_FALSE,
                              7 * (GLsizei)sizeof(GLfloat), inter);
        glEnableVertexAttribArray((GLuint)lb);
        glVertexAttribPointer((GLuint)lb, 3, GL_FLOAT, GL_FALSE,
                              7 * (GLsizei)sizeof(GLfloat), inter + 4);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);
        CHECK(px_is(c, 32, 32, 0, 255, 0, 1),
              "interleaved attributes with a stride");
    }

    /* glBindAttribLocation before linking must be honoured. */
    {
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        const char *vsrc = "attribute vec4 aPos;\n"
                           "void main() { gl_Position = aPos; }\n";
        glShaderSource(vs, 1, &vsrc, NULL); glCompileShader(vs);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        const char *fsrc = "void main() { gl_FragColor = vec4(1.0); }\n";
        glShaderSource(fs, 1, &fsrc, NULL); glCompileShader(fs);
        GLuint pr = glCreateProgram();
        glAttachShader(pr, vs); glAttachShader(pr, fs);
        glBindAttribLocation(pr, 5, "aPos");
        glLinkProgram(pr);
        GLint ok = 0;
        glGetProgramiv(pr, GL_LINK_STATUS, &ok);
        CHECK(ok == GL_TRUE, "a program with a bound attribute links");
        CHECK(glGetAttribLocation(pr, "aPos") == 5,
              "glBindAttribLocation is honoured at link time");
    }

    /* Out-of-range indices are refused. */
    while (glGetError() != GL_NO_ERROR) { }
    glEnableVertexAttribArray(GL_MAX_VERTEX_ATTRIBS_IMPL);
    CHECK(glGetError() == GL_INVALID_VALUE,
          "an out-of-range attribute index is refused");
    glVertexAttribPointer(0, 5, GL_FLOAT, GL_FALSE, 0, ndc_quad);
    CHECK(glGetError() == GL_INVALID_VALUE,
          "a 5-component attribute is refused");

    aglxDestroyContext(c);
}

/* ============================================================================
 * Textures through a shader
 * ==========================================================================*/

static void test_texturing(void) {
    printf("--- texturing ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    /* A 2x2 texture, bottom row red/green, top row blue/white. */
    static const unsigned char rgb[2 * 2 * 3] = {
        255, 0,   0,      0,   255, 0,
        0,   0,   255,    255, 255, 255,
    };
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, rgb);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    GLuint p = build(
        "attribute vec4 aPos;\nattribute vec2 aUV;\nvarying vec2 vUV;\n"
        "void main() { vUV = aUV; gl_Position = aPos; }\n",
        "precision mediump float;\nvarying vec2 vUV;\n"
        "uniform sampler2D uTex;\n"
        "void main() { gl_FragColor = texture2D(uTex, vUV); }\n",
        "texturing");
    CHECK(p != 0, "a texturing program links");
    if (!p) { aglxDestroyContext(c); return; }

    static const GLfloat uv[8] = { 0,0, 1,0, 1,1, 0,1 };
    glUseProgram(p);
    GLint lp = glGetAttribLocation(p, "aPos");
    GLint lu = glGetAttribLocation(p, "aUV");
    glEnableVertexAttribArray((GLuint)lp);
    glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0, ndc_quad);
    glEnableVertexAttribArray((GLuint)lu);
    glVertexAttribPointer((GLuint)lu, 2, GL_FLOAT, GL_FALSE, 0, uv);

    /* A sampler defaults to unit 0, so this must work with no glUniform1i. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(px_is(c, 16, 16, 255, 0, 0, 1),
          "a sampler defaults to texture unit 0");
    CHECK(px_is(c, 48, 16, 0, 255, 0, 1), "the quadrants map correctly");
    CHECK(px_is(c, 16, 48, 0, 0, 255, 1),
          "and the v axis is not flipped");

    /* glUniform1i points the sampler at another unit. */
    static const unsigned char yellow[3] = { 255, 255, 0 };
    GLuint tex2 = 0;
    glActiveTexture(GL_TEXTURE1);
    glGenTextures(1, &tex2);
    glBindTexture(GL_TEXTURE_2D, tex2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, yellow);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glActiveTexture(GL_TEXTURE0);

    glUniform1i(glGetUniformLocation(p, "uTex"), 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(px_is(c, 32, 32, 255, 255, 0, 1),
          "glUniform1i selects the sampler's texture unit");

    /* A shader samples what is BOUND, regardless of glEnable(GL_TEXTURE_2D):
     * the enables are a fixed-function concept with no meaning under ES 2.0. */
    glDisable(GL_TEXTURE_2D);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(px_is(c, 32, 32, 255, 255, 0, 1),
          "a shader ignores glEnable(GL_TEXTURE_2D)");

    aglxDestroyContext(c);
}

/* ============================================================================
 * Coexistence with the fixed-function path
 *
 * Nine phases depend on fixed function.  Every one of these is really a
 * regression test for the other 289 checks.
 * ==========================================================================*/

static void test_coexistence(void) {
    printf("--- coexistence with fixed function ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    /* Fixed function before any program exists. */
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, 0, H, -10, 10);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(0, 0, 1);
    glBegin(GL_QUADS);
    glVertex3f(8, 8, 0); glVertex3f(56, 8, 0);
    glVertex3f(56, 56, 0); glVertex3f(8, 56, 0);
    glEnd();
    CHECK(px_is(c, 32, 32, 0, 0, 255, 1), "fixed function draws before");

    GLuint p = build(
        "attribute vec4 aPos;\nvoid main() { gl_Position = aPos; }\n",
        "void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n",
        "coexistence");
    CHECK(p != 0, "the program links");
    if (!p) { aglxDestroyContext(c); return; }

    /* Creating and linking a program must not disturb fixed function. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(0, 1, 0);
    glBegin(GL_QUADS);
    glVertex3f(8, 8, 0); glVertex3f(56, 8, 0);
    glVertex3f(56, 56, 0); glVertex3f(8, 56, 0);
    glEnd();
    CHECK(px_is(c, 32, 32, 0, 255, 0, 1),
          "an unused program does not disturb fixed function");

    /* Switch to the shader. */
    glUseProgram(p);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    draw_ndc_quad(p, "aPos");
    CHECK(px_is(c, 32, 32, 255, 0, 0, 1), "the shader takes over");

    /* Switch back, in the same frame. */
    glUseProgram(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(1, 1, 0);
    glBegin(GL_QUADS);
    glVertex3f(8, 8, 0); glVertex3f(56, 8, 0);
    glVertex3f(56, 56, 0); glVertex3f(8, 56, 0);
    glEnd();
    CHECK(px_is(c, 32, 32, 255, 255, 0, 1),
          "glUseProgram(0) restores fixed function");
    CHECK(px_is(c, 2, 2, 0, 0, 0, 1),
          "and the fixed-function geometry is placed as before");

    /* Alternating within one frame: the classic mixed-renderer case. */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(p);
    draw_ndc_quad(p, "aPos");
    glUseProgram(0);
    glColor3f(0, 0, 1);
    glBegin(GL_QUADS);
    glVertex3f(24, 24, 0); glVertex3f(40, 24, 0);
    glVertex3f(40, 40, 0); glVertex3f(24, 40, 0);
    glEnd();
    CHECK(px_is(c, 32, 32, 0, 0, 255, 1),
          "fixed-function geometry draws over shaded geometry");
    CHECK(px_is(c, 4, 4, 255, 0, 0, 1),
          "and the shaded background survives");

    /* Fixed-function texturing must still work after a shader used a
     * sampler: the two use the same texture objects. */
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
    }

    /* Fixed-function lighting, the most state-heavy path, must be intact. */
    {
        GLfloat pos[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
        GLfloat dif[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
        glColor3f(1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glNormal3f(0, 0, 1);
        glBegin(GL_QUADS);
        glVertex3f(8, 8, 0); glVertex3f(56, 8, 0);
        glVertex3f(56, 56, 0); glVertex3f(8, 56, 0);
        glEnd();
        uint32_t lit = px(c, 32, 32);
        CHECK(((lit >> 16) & 0xFF) > 180,
              "fixed-function lighting still lights");
        glDisable(GL_LIGHTING);
        glDisable(GL_LIGHT0);
    }

    /* Deleting the bound program reverts to fixed function rather than
     * leaving the rasterizer pointing at freed state. */
    glUseProgram(p);
    glDeleteProgram(p);
    GLint bound = -1;
    glGetIntegerv(GL_CURRENT_PROGRAM, &bound);
    CHECK(bound == 0, "deleting the bound program unbinds it");
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3f(0, 1, 1);
    glBegin(GL_QUADS);
    glVertex3f(8, 8, 0); glVertex3f(56, 8, 0);
    glVertex3f(56, 56, 0); glVertex3f(8, 56, 0);
    glEnd();
    CHECK(px_is(c, 32, 32, 0, 255, 255, 1),
          "and fixed function still draws afterwards");

    aglxDestroyContext(c);
}

/* ============================================================================
 * Robustness
 * ==========================================================================*/

static void test_robustness(void) {
    printf("--- robustness ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    /* Every entry point must tolerate a name that does not exist. */
    while (glGetError() != GL_NO_ERROR) { }
    glCompileShader(9999);
    CHECK(glGetError() == GL_INVALID_VALUE, "glCompileShader on a stranger");
    glLinkProgram(9999);
    CHECK(glGetError() == GL_INVALID_VALUE, "glLinkProgram on a stranger");
    glUseProgram(9999);
    CHECK(glGetError() == GL_INVALID_VALUE, "glUseProgram on a stranger");
    glAttachShader(9999, 9998);
    CHECK(glGetError() == GL_INVALID_VALUE, "glAttachShader on strangers");

    /* Deleting a shader that is still attached must be deferred, or the
     * program would hold a dangling name and a relink would compile
     * whatever landed in the slot next. */
    {
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        const char *v = "attribute vec4 a;\nvoid main(){ gl_Position = a; }\n";
        glShaderSource(vs, 1, &v, NULL);
        glCompileShader(vs);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        const char *f = "void main(){ gl_FragColor = vec4(1.0); }\n";
        glShaderSource(fs, 1, &f, NULL);
        glCompileShader(fs);
        GLuint pr = glCreateProgram();
        glAttachShader(pr, vs);
        glAttachShader(pr, fs);
        glLinkProgram(pr);

        glDeleteShader(vs);
        CHECK(glIsShader(vs) == GL_TRUE,
              "deleting an attached shader is deferred");

        glUseProgram(pr);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw_ndc_quad(pr, "a");
        CHECK(px_is(c, 32, 32, 255, 255, 255, 1),
              "and the program still renders");

        glUseProgram(0);
        glDetachShader(pr, vs);
        glDeleteShader(vs);
        CHECK(glIsShader(vs) == GL_FALSE,
              "detaching then deleting completes the deletion");
    }

    /* A shader whose runtime hits a limit must not hang the draw.  The
     * fragment comes out magenta and the info log says why -- an obviously
     * wrong colour is far easier to notice than a stale one. */
    {
        GLuint pr = build(
            "attribute vec4 aPos;\nvoid main() { gl_Position = aPos; }\n",
            "precision mediump float;\n"
            "void main() {\n"
            "  float a = 0.0;\n"
            "  while (a < 1.0) { }\n"
            "  gl_FragColor = vec4(a);\n"
            "}\n", "infinite loop");
        CHECK(pr != 0, "a shader with an infinite loop still compiles");
        if (pr) {
            glUseProgram(pr);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            /* One pixel's worth, so the test finishes: a full-screen quad
             * would burn the iteration budget 4096 times. */
            static const GLfloat tiny[16] = {
                -1.0f, -1.0f, 0.0f, 1.0f,
                -0.95f, -1.0f, 0.0f, 1.0f,
                -0.95f, -0.95f, 0.0f, 1.0f,
                -1.0f, -0.95f, 0.0f, 1.0f,
            };
            GLint l = glGetAttribLocation(pr, "aPos");
            glEnableVertexAttribArray((GLuint)l);
            glVertexAttribPointer((GLuint)l, 4, GL_FLOAT, GL_FALSE, 0, tiny);
            glDrawArrays(GL_QUADS, 0, 4);
            CHECK(px_is(c, 0, 0, 255, 0, 255, 1),
                  "a runaway fragment shader produces magenta, not a hang");
            glUseProgram(0);
        }
    }

    /* Relinking must rebuild the tables: a stale location writing into the
     * wrong slot is a bug that only shows up after an application changes a
     * shader at run time. */
    {
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        const char *v1 = "attribute vec4 aPos;\nuniform float uA;\n"
                         "void main(){ gl_Position = aPos * uA; }\n";
        glShaderSource(vs, 1, &v1, NULL); glCompileShader(vs);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        const char *f1 = "void main(){ gl_FragColor = vec4(1.0); }\n";
        glShaderSource(fs, 1, &f1, NULL); glCompileShader(fs);
        GLuint pr = glCreateProgram();
        glAttachShader(pr, vs); glAttachShader(pr, fs);
        glLinkProgram(pr);
        GLint n1 = 0;
        glGetProgramiv(pr, GL_ACTIVE_UNIFORMS, &n1);
        CHECK(n1 == 1, "the first link finds one uniform");

        /* Replace the vertex shader with one declaring two uniforms. */
        const char *v2 = "attribute vec4 aPos;\nuniform float uA;\n"
                         "uniform float uB;\n"
                         "void main(){ gl_Position = aPos * uA * uB; }\n";
        glShaderSource(vs, 1, &v2, NULL); glCompileShader(vs);
        glLinkProgram(pr);
        GLint n2 = 0;
        glGetProgramiv(pr, GL_ACTIVE_UNIFORMS, &n2);
        CHECK(n2 == 2, "a relink rebuilds the uniform table");
    }

    /* Destroying a context with programs alive must free everything.  Under
     * a leak checker this catches a missed free in the shader storage. */
    {
        aglx_context_t *c2 = aglxCreateContext(1, 32, 32, AGLX_DEPTH);
        aglxMakeCurrent(c2);
        for (int i = 0; i < 4; i++) {
            build("attribute vec4 a;\nvoid main(){ gl_Position = a; }\n",
                  "void main(){ gl_FragColor = vec4(1.0); }\n", "leak");
        }
        aglxDestroyContext(c2);
        aglxMakeCurrent(c);
        CHECK(1, "destroying a context frees its shaders and programs");
    }

    /* Running out of shader slots must be reported, not silently wrap. */
    {
        int made = 0;
        for (int i = 0; i < GL_MAX_SHADERS_IMPL + 4; i++) {
            if (glCreateShader(GL_VERTEX_SHADER) != 0) made++;
        }
        CHECK(made <= GL_MAX_SHADERS_IMPL,
              "shader creation stops at the implementation limit");
    }

    aglxDestroyContext(c);
}

/* ============================================================================
 * Realistic use
 * ==========================================================================*/

static void test_realistic(void) {
    printf("--- realistic use ---\n");
    aglx_context_t *c = setup();
    if (!c) { CHECK(0, "context"); return; }

    /* The shape of a real ES 2.0 renderer: an MVP matrix, an interpolated
     * normal, a light direction uniform and a Lambert term. */
    GLuint p = build(
        "attribute vec4 aPosition;\n"
        "attribute vec3 aNormal;\n"
        "uniform mat4 uMVP;\n"
        "varying vec3 vNormal;\n"
        "void main() {\n"
        "  vNormal = aNormal;\n"
        "  gl_Position = uMVP * aPosition;\n"
        "}\n",
        "precision mediump float;\n"
        "varying vec3 vNormal;\n"
        "uniform vec3 uLightDir;\n"
        "uniform vec3 uColor;\n"
        "void main() {\n"
        "  float ndl = max(dot(normalize(vNormal), normalize(uLightDir)), 0.0);\n"
        "  gl_FragColor = vec4(uColor * ndl, 1.0);\n"
        "}\n",
        "lit quad");
    CHECK(p != 0, "a realistic lit program links");
    if (!p) { aglxDestroyContext(c); return; }

    static const GLfloat identity[16] = {
        1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1
    };
    static const GLfloat normals[12] = {
        0,0,1,  0,0,1,  0,0,1,  0,0,1
    };

    glUseProgram(p);
    glUniformMatrix4fv(glGetUniformLocation(p, "uMVP"), 1, GL_FALSE, identity);
    glUniform3f(glGetUniformLocation(p, "uColor"), 0.8f, 0.4f, 0.2f);
    glUniform3f(glGetUniformLocation(p, "uLightDir"), 0.0f, 0.0f, 1.0f);

    GLint lp = glGetAttribLocation(p, "aPosition");
    GLint ln = glGetAttribLocation(p, "aNormal");
    glEnableVertexAttribArray((GLuint)lp);
    glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0, ndc_quad);
    glEnableVertexAttribArray((GLuint)ln);
    glVertexAttribPointer((GLuint)ln, 3, GL_FLOAT, GL_FALSE, 0, normals);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_QUADS, 0, 4);
    /* Light head-on: N.L is 1, so the surface is its full colour. */
    CHECK(px_is(c, 32, 32, 204, 102, 51, 3),
          "a lit surface with the light head-on shows its base colour");

    /* Light behind: N.L is negative, clamped to zero, so the surface is
     * black.  Only the uniform changed between the two draws. */
    glUniform3f(glGetUniformLocation(p, "uLightDir"), 0.0f, 0.0f, -1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_QUADS, 0, 4);
    CHECK(px_is(c, 32, 32, 0, 0, 0, 2),
          "and turns black with the light behind");

    /* glDrawElements with a shader: indices index the generic attributes. */
    {
        static const GLushort idx[6] = { 0, 1, 2, 0, 2, 3 };
        glUniform3f(glGetUniformLocation(p, "uLightDir"), 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
        CHECK(px_is(c, 32, 32, 204, 102, 51, 3),
              "glDrawElements works with a shader");
    }

    /* A vertex buffer object feeding a generic attribute. */
    {
        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof ndc_quad,
                     ndc_quad, GL_STATIC_DRAW);
        glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0,
                              (const void *)0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_QUADS, 0, 4);
        CHECK(px_is(c, 32, 32, 204, 102, 51, 3),
              "a VBO feeds a generic vertex attribute");
        glDeleteBuffers(1, &vbo);
        /* Put the client pointer back for anything that follows. */
        glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0, ndc_quad);
    }

    /* Implementation limits must be queryable, so an application can size
     * its own tables. */
    {
        GLint v = 0;
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &v);
        CHECK(v == GL_MAX_VERTEX_ATTRIBS_IMPL && v >= 8,
              "GL_MAX_VERTEX_ATTRIBS reports at least the ES 2.0 minimum");
        glGetIntegerv(GL_MAX_VARYING_VECTORS, &v);
        CHECK(v >= 8, "GL_MAX_VARYING_VECTORS reports at least 8");
    }

    aglxDestroyContext(c);
}

/* ============================================================================
 * Driver
 * ==========================================================================*/

int main(void) {
    printf("=== test_glprog: the shader pipeline (phase G11c) ===\n");

    test_shader_objects();
    test_linking();
    test_rendering();
    test_attributes();
    test_texturing();
    test_coexistence();
    test_robustness();
    test_realistic();

    printf("\ntest_glprog: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

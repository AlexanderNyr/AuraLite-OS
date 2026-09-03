/* glshade.c — GL2 phase L5: the visual twin of /glcube, lit from GLSL.
 *
 * /glcube lights its cube with glEnable(GL_LIGHTING).  This demo draws the
 * same rotating cube with the same window chrome, but the lighting is a
 * Lambert term computed in a fragment shader, and the geometry arrives
 * through generic vertex attributes and glDrawArrays — the shape of a real
 * ES 2.0 renderer.  (GL2_PLAN D8: it never touches glBegin; the fixed-
 * function pipeline is not a fallback path here.)
 *
 * A fixed-function fallback is deliberately NOT allowed in this binary.  If
 * the program fails to compile or link, glshade prints the info log and
 * exits non-zero: a shader demo that silently lights nothing is worse than
 * no shader demo (GL2_PLAN L5).
 *
 * Frame limit for automated testing, same convention as /glcube: an optional
 * decimal frame count in /tmp/glshade.frames; absent or unreadable means
 * "run until closed".
 *
 * Controls: q / Esc / window close quit; space pauses; a/d/w/s or mouse
 * drag rotate — the same keys as /glcube.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>

#include "auragui.h"
#include "GL/gl.h"
#include "GL/auraglx.h"

#define CUBE_W 320
#define CUBE_H 240
#define WIN_W (CUBE_W + 16)
#define WIN_H (CUBE_H + 48)

/* ============================================================================
 * A minimal column-major mat4 (the app owns its matrices in the ES style).
 *============================================================================*/

typedef GLfloat mat4[16];

static void mat_identity(mat4 m) {
    static const GLfloat i[16] = {
        1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1
    };
    memcpy(m, i, sizeof i);
}

/* out = a * b (column-major, as glLoadMatrix and uMVP expect). */
static void mat_mul(mat4 out, const mat4 a, const mat4 b) {
    mat4 r;
    for (int c = 0; c < 4; c++) {
        for (int ro = 0; ro < 4; ro++) {
            GLfloat s = 0.0f;
            for (int k = 0; k < 4; k++) {
                s += a[k * 4 + ro] * b[c * 4 + k];
            }
            r[c * 4 + ro] = s;
        }
    }
    memcpy(out, r, sizeof r);
}

/* gluPerspective(45, aspect, 1, 50) written out. */
static void mat_perspective(mat4 m, GLfloat fovy_deg, GLfloat aspect,
                            GLfloat znear, GLfloat zfar) {
    mat_identity(m);
    GLfloat t = 1.0f / tanf(fovy_deg * 3.14159265f / 360.0f);
    m[0]  = t / aspect;
    m[5]  = t;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = -1.0f;
    m[14] = 2.0f * zfar * znear / (znear - zfar);
    m[15] = 0.0f;
}

static void mat_translate_z(mat4 m, GLfloat z) {
    mat_identity(m);
    m[14] = z;
}

static void mat_rotate(mat4 m, GLfloat deg, char axis) {
    mat_identity(m);
    GLfloat r = deg * 3.14159265f / 180.0f;
    GLfloat c = cosf(r), s = sinf(r);
    if (axis == 'x') { m[5] = c; m[6] = s; m[9] = -s; m[10] = c; }
    else             { m[0] = c; m[2] = -s; m[8] = s; m[10] = c; }
}

/* ============================================================================
 * The shaders.
 *============================================================================*/

static const char *shade_vs =
    "attribute vec3 aPos;\n"
    "attribute vec3 aNormal;\n"
    "attribute vec3 aColor;\n"
    "uniform mat4 uMVP;\n"
    "uniform mat4 uRot;\n"
    "varying vec3 vNormal;\n"
    "varying vec3 vColor;\n"
    "void main() {\n"
    "  gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "  /* uRot is a pure rotation, so the normal needs no inverse. */\n"
    "  vNormal = (uRot * vec4(aNormal, 0.0)).xyz;\n"
    "  vColor = aColor;\n"
    "}\n";

static const char *shade_fs =
    "precision mediump float;\n"
    "varying vec3 vNormal;\n"
    "varying vec3 vColor;\n"
    "uniform vec3 uLightDir;\n"
    "void main() {\n"
    "  float ndl = max(dot(normalize(vNormal), normalize(uLightDir)), 0.0);\n"
    "  gl_FragColor = vec4(vColor * (0.25 + 0.75 * ndl), 1.0);\n"
    "}\n";

static GLuint build_program(void) {
    GLint ok = 0;
    char log[512];

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &shade_vs, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(vs, sizeof log, NULL, log);
        printf("glshade: vertex shader: %s", log);
        return 0;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &shade_fs, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(fs, sizeof log, NULL, log);
        printf("glshade: fragment shader: %s", log);
        return 0;
    }

    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        printf("glshade: link: %s", log);
        return 0;
    }
    return p;
}

/* ============================================================================
 * Geometry: the same multi-coloured cube /glcube draws, as one array.
 *============================================================================*/

/* Per face: 6 vertices of (x, y, z, nx, ny, nz, r, g, b). */
static GLfloat cube_verts[36 * 9];
static GLint   cube_vert_count;

static void face(GLfloat *o, int *n,
                 GLfloat ax, GLfloat ay, GLfloat az,
                 GLfloat bx, GLfloat by, GLfloat bz,
                 GLfloat cx, GLfloat cy, GLfloat cz,
                 GLfloat dx, GLfloat dy, GLfloat dz,
                 GLfloat nx, GLfloat ny, GLfloat nz,
                 GLfloat r, GLfloat g, GLfloat bl) {
    const GLfloat v[4][3] = { { ax, ay, az }, { bx, by, bz },
                              { cx, cy, cz }, { dx, dy, dz } };
    const GLuint idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) {
        const GLfloat *p = v[idx[i]];
        *o++ = p[0]; *o++ = p[1]; *o++ = p[2];
        *o++ = nx;   *o++ = ny;   *o++ = nz;
        *o++ = r;    *o++ = g;    *o++ = bl;
        (*n)++;
    }
}

static void build_cube(void) {
    GLfloat *o = cube_verts;
    static const GLfloat e = 1.0f;
    GLint n = 0;
    /* +z */ face(o, &n, -e,-e, e,  e,-e, e,  e, e, e, -e, e, e,  0, 0, 1, 0.9f, 0.3f, 0.3f);
    /* -z */ face(o, &n,  e,-e,-e, -e,-e,-e, -e, e,-e,  e, e,-e,  0, 0,-1, 0.3f, 0.9f, 0.4f);
    /* +x */ face(o, &n,  e,-e, e,  e,-e,-e,  e, e,-e,  e, e, e,  1, 0, 0, 0.4f, 0.5f, 0.95f);
    /* -x */ face(o, &n, -e,-e,-e, -e,-e, e, -e, e, e, -e, e,-e, -1, 0, 0, 0.95f, 0.8f, 0.3f);
    /* +y */ face(o, &n, -e, e, e,  e, e, e,  e, e,-e, -e, e,-e,  0, 1, 0, 0.6f, 0.4f, 0.9f);
    /* -y */ face(o, &n, -e,-e,-e,  e,-e,-e,  e,-e, e, -e,-e, e,  0,-1, 0, 0.3f, 0.85f, 0.85f);
    cube_vert_count = n;
}

/* Optional frame limit (see the header comment). */
static long read_frame_limit(void) {
    int fd = open("/tmp/glshade.frames", O_RDONLY);
    if (fd < 0) return 0;
    char buf[32] = { 0 };
    int n = (int)read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    long v = strtol(buf, NULL, 10);
    return (v > 0) ? v : 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    long max_frames = read_frame_limit();  /* 0 = run until closed */

    int wid = ag_window_create(60, 40, WIN_W, WIN_H, "OpenGL Shade",
                               AG_WIN_DEFAULT);
    if (wid < 0) {
        printf("glshade: cannot create window\n");
        return 1;
    }
    ag_window_show(wid);

    aglx_context_t *ctx = aglxCreateContext(wid, CUBE_W, CUBE_H, AGLX_DEPTH);
    if (!ctx) {
        printf("glshade: cannot create GL context\n");
        ag_window_destroy(wid);
        return 1;
    }
    aglxMakeCurrent(ctx);

    printf("glshade: %s | %s\n",
           (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION));

    GLuint prog = build_program();
    if (prog == 0) {
        /* No fixed-function fallback (L5): a shader demo that cannot shade
         * must say so and stop. */
        printf("glshade: no program, no fallback — exiting\n");
        aglxMakeCurrent(NULL);
        aglxDestroyContext(ctx);
        ag_window_destroy(wid);
        return 1;
    }
    glUseProgram(prog);

    build_cube();

    GLint a_pos    = glGetAttribLocation(prog, "aPos");
    GLint a_norm   = glGetAttribLocation(prog, "aNormal");
    GLint a_color  = glGetAttribLocation(prog, "aColor");
    GLint u_mvp    = glGetUniformLocation(prog, "uMVP");
    GLint u_rot    = glGetUniformLocation(prog, "uRot");
    GLint u_light  = glGetUniformLocation(prog, "uLightDir");

    glEnableVertexAttribArray((GLuint)a_pos);
    glEnableVertexAttribArray((GLuint)a_norm);
    glEnableVertexAttribArray((GLuint)a_color);
    glVertexAttribPointer((GLuint)a_pos,   3, GL_FLOAT, GL_FALSE,
                          9 * sizeof(GLfloat), cube_verts);
    glVertexAttribPointer((GLuint)a_norm,  3, GL_FLOAT, GL_FALSE,
                          9 * sizeof(GLfloat), cube_verts + 3);
    glVertexAttribPointer((GLuint)a_color, 3, GL_FLOAT, GL_FALSE,
                          9 * sizeof(GLfloat), cube_verts + 6);
    glUniform3f(u_light, 0.35f, 0.5f, 0.8f);   /* up, right, at the eye */

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glClearColor(0.06f, 0.07f, 0.10f, 1.0f);

    mat4 proj, view, rotx, roty, rot, mv, mvp;
    mat_perspective(proj, 45.0f, (GLfloat)CUBE_W / (GLfloat)CUBE_H, 1.0f, 50.0f);
    mat_translate_z(view, -6.0f);

    GLfloat angle_x = 25.0f, angle_y = 30.0f;
    int paused = 0, running = 1;
    long frames = 0;
    int dragging = 0, last_mx = 0, last_my = 0;

    while (running) {
        ag_event_t ev;
        while (ag_poll_event(wid, &ev) > 0) {
            switch (ev.type) {
            case AG_EVT_CLOSE_REQ:
                running = 0;
                break;
            case AG_EVT_KEY_DOWN:
                if (ev.key == 'q' || ev.key == 27) running = 0;
                else if (ev.key == ' ') paused = !paused;
                else if (ev.key == 'a') angle_y -= 5.0f;
                else if (ev.key == 'd') angle_y += 5.0f;
                else if (ev.key == 'w') angle_x -= 5.0f;
                else if (ev.key == 's') angle_x += 5.0f;
                break;
            case AG_EVT_MOUSE_DOWN:
                dragging = 1; last_mx = ev.x; last_my = ev.y;
                break;
            case AG_EVT_MOUSE_UP:
                dragging = 0;
                break;
            case AG_EVT_MOUSE_MOVE:
                if (dragging) {
                    angle_y += (GLfloat)(ev.x - last_mx);
                    angle_x += (GLfloat)(ev.y - last_my);
                    last_mx = ev.x; last_my = ev.y;
                }
                break;
            default:
                break;
            }
        }
        if (!running) break;

        if (!paused) {
            angle_y += 1.6f;
            angle_x += 0.7f;
            if (angle_y >= 360.0f) angle_y -= 360.0f;
            if (angle_x >= 360.0f) angle_x -= 360.0f;
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        mat_rotate(rotx, angle_x, 'x');
        mat_rotate(roty, angle_y, 'y');
        mat_mul(rot, rotx, roty);        /* the normal's rotation */
        mat_mul(mv, view, rot);
        mat_mul(mvp, proj, mv);

        glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
        glUniformMatrix4fv(u_rot, 1, GL_FALSE, rot);

        glDrawArrays(GL_TRIANGLES, 0, cube_vert_count);

        aglxSwapBuffers(ctx);

        frames++;
        if (max_frames > 0 && frames >= max_frames) {
            printf("glshade: rendered %ld frames\n", frames);
            break;
        }
    }

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) printf("glshade: GL error 0x%04x\n", err);
    else if (max_frames <= 0)
        printf("glshade: clean exit, %ld frames\n", frames);

    aglxMakeCurrent(NULL);
    aglxDestroyContext(ctx);
    ag_window_destroy(wid);
    return (err == GL_NO_ERROR) ? 0 : 1;
}

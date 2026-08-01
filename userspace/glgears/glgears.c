/* glgears.c — the classic three-gear OpenGL demo, ported to AuraLite.
 *
 * Phase G8 of GL_PLAN.md.
 *
 * This is the traditional reference benchmark for a fixed-function GL: three
 * interlocking gears, lit by one light, each built from quads and quad strips.
 * Porting it is a meaningful check of the whole stack precisely because it was
 * written against real OpenGL — if it renders here, the API surface behaves
 * the way applications expect rather than the way this implementation happens
 * to be built.
 *
 * Each gear is compiled into a display list once, which is what the original
 * does and what makes the per-frame cost pure transform plus rasterisation.
 *
 * Frame limit for automated testing: the shell's "run" uses spawn(), which
 * does not forward argv, so an optional decimal frame count is read from
 * /tmp/glgears.frames (the same convention /apm and /glcube use).  Absent
 * means "run until the window is closed".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

#include "auragui.h"
#include "GL/gl.h"
#include "GL/glu.h"
#include "GL/auraglx.h"

#define GEARS_W 280
#define GEARS_H 210
#define WIN_W (GEARS_W + 16)
#define WIN_H (GEARS_H + 48)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Build one gear: a toothed wheel with a bored centre.
 *
 * inner_radius  hole in the middle
 * outer_radius  base of the teeth
 * width         thickness along z
 * teeth         number of teeth
 * tooth_depth   how far the teeth stick out
 *
 * This is the original 1990s glxgears geometry, kept faithful so the visual
 * result can be compared against any other GL implementation.
 */
static void build_gear(GLfloat inner_radius, GLfloat outer_radius,
                       GLfloat width, GLint teeth, GLfloat tooth_depth) {
    GLfloat r0 = inner_radius;
    GLfloat r1 = outer_radius - tooth_depth / 2.0f;
    GLfloat r2 = outer_radius + tooth_depth / 2.0f;
    GLfloat da = 2.0f * (GLfloat)M_PI / (GLfloat)teeth / 4.0f;
    GLfloat w2 = width * 0.5f;

    glShadeModel(GL_FLAT);

    /* ---- front face ---- */
    glNormal3f(0.0f, 0.0f, 1.0f);
    glBegin(GL_QUAD_STRIP);
    for (GLint i = 0; i <= teeth; i++) {
        GLfloat angle = (GLfloat)i * 2.0f * (GLfloat)M_PI / (GLfloat)teeth;
        glVertex3f(r0 * cosf(angle), r0 * sinf(angle), w2);
        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), w2);
        if (i < teeth) {
            glVertex3f(r0 * cosf(angle), r0 * sinf(angle), w2);
            glVertex3f(r1 * cosf(angle + 3 * da), r1 * sinf(angle + 3 * da), w2);
        }
    }
    glEnd();

    /* ---- front sides of the teeth ---- */
    glBegin(GL_QUADS);
    for (GLint i = 0; i < teeth; i++) {
        GLfloat angle = (GLfloat)i * 2.0f * (GLfloat)M_PI / (GLfloat)teeth;
        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), w2);
        glVertex3f(r2 * cosf(angle + da), r2 * sinf(angle + da), w2);
        glVertex3f(r2 * cosf(angle + 2 * da), r2 * sinf(angle + 2 * da), w2);
        glVertex3f(r1 * cosf(angle + 3 * da), r1 * sinf(angle + 3 * da), w2);
    }
    glEnd();

    /* ---- back face ---- */
    glNormal3f(0.0f, 0.0f, -1.0f);
    glBegin(GL_QUAD_STRIP);
    for (GLint i = 0; i <= teeth; i++) {
        GLfloat angle = (GLfloat)i * 2.0f * (GLfloat)M_PI / (GLfloat)teeth;
        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), -w2);
        glVertex3f(r0 * cosf(angle), r0 * sinf(angle), -w2);
        if (i < teeth) {
            glVertex3f(r1 * cosf(angle + 3 * da), r1 * sinf(angle + 3 * da), -w2);
            glVertex3f(r0 * cosf(angle), r0 * sinf(angle), -w2);
        }
    }
    glEnd();

    /* ---- back sides of the teeth ---- */
    glBegin(GL_QUADS);
    for (GLint i = 0; i < teeth; i++) {
        GLfloat angle = (GLfloat)i * 2.0f * (GLfloat)M_PI / (GLfloat)teeth;
        glVertex3f(r1 * cosf(angle + 3 * da), r1 * sinf(angle + 3 * da), -w2);
        glVertex3f(r2 * cosf(angle + 2 * da), r2 * sinf(angle + 2 * da), -w2);
        glVertex3f(r2 * cosf(angle + da), r2 * sinf(angle + da), -w2);
        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), -w2);
    }
    glEnd();

    /* ---- outward faces of the teeth ---- */
    glBegin(GL_QUAD_STRIP);
    for (GLint i = 0; i < teeth; i++) {
        GLfloat angle = (GLfloat)i * 2.0f * (GLfloat)M_PI / (GLfloat)teeth;

        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), w2);
        glVertex3f(r1 * cosf(angle), r1 * sinf(angle), -w2);
        GLfloat u = r2 * cosf(angle + da) - r1 * cosf(angle);
        GLfloat v = r2 * sinf(angle + da) - r1 * sinf(angle);
        GLfloat len = sqrtf(u * u + v * v);
        if (len > 1e-6f) { u /= len; v /= len; }
        glNormal3f(v, -u, 0.0f);
        glVertex3f(r2 * cosf(angle + da), r2 * sinf(angle + da), w2);
        glVertex3f(r2 * cosf(angle + da), r2 * sinf(angle + da), -w2);

        glNormal3f(cosf(angle), sinf(angle), 0.0f);
        glVertex3f(r2 * cosf(angle + 2 * da), r2 * sinf(angle + 2 * da), w2);
        glVertex3f(r2 * cosf(angle + 2 * da), r2 * sinf(angle + 2 * da), -w2);

        u = r1 * cosf(angle + 3 * da) - r2 * cosf(angle + 2 * da);
        v = r1 * sinf(angle + 3 * da) - r2 * sinf(angle + 2 * da);
        glNormal3f(v, -u, 0.0f);
        glVertex3f(r1 * cosf(angle + 3 * da), r1 * sinf(angle + 3 * da), w2);
        glVertex3f(r1 * cosf(angle + 3 * da), r1 * sinf(angle + 3 * da), -w2);

        glNormal3f(cosf(angle), sinf(angle), 0.0f);
    }
    glVertex3f(r1 * cosf(0.0f), r1 * sinf(0.0f), w2);
    glVertex3f(r1 * cosf(0.0f), r1 * sinf(0.0f), -w2);
    glEnd();

    glShadeModel(GL_SMOOTH);

    /* ---- inside of the bore ---- */
    glBegin(GL_QUAD_STRIP);
    for (GLint i = 0; i <= teeth; i++) {
        GLfloat angle = (GLfloat)i * 2.0f * (GLfloat)M_PI / (GLfloat)teeth;
        glNormal3f(-cosf(angle), -sinf(angle), 0.0f);
        glVertex3f(r0 * cosf(angle), r0 * sinf(angle), -w2);
        glVertex3f(r0 * cosf(angle), r0 * sinf(angle), w2);
    }
    glEnd();
}

static long read_frame_limit(void) {
    int fd = open("/tmp/glgears.frames", O_RDONLY);
    if (fd < 0) return 0;
    char buf[32];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    long v = strtol(buf, NULL, 10);
    return (v > 0) ? v : 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    long max_frames = read_frame_limit();

    int wid = ag_window_create(70, 50, WIN_W, WIN_H, "glgears", AG_WIN_DEFAULT);
    if (wid < 0) {
        printf("glgears: cannot create window\n");
        return 1;
    }
    ag_window_show(wid);

    aglx_context_t *ctx = aglxCreateContext(wid, GEARS_W, GEARS_H, AGLX_DEFAULT);
    if (!ctx) {
        printf("glgears: cannot create GL context\n");
        ag_window_destroy(wid);
        return 1;
    }
    aglxMakeCurrent(ctx);

    printf("glgears: %s | %s\n",
           (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION));

    /* One directional light, as in the original. */
    {
        GLfloat pos[4] = { 5.0f, 5.0f, 10.0f, 0.0f };
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
    }
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_NORMALIZE);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, (GLdouble)GEARS_W / (GLdouble)GEARS_H, 1.0, 60.0);
    glMatrixMode(GL_MODELVIEW);

    /* Each gear is compiled once: only the matrix around it changes. */
    GLuint gear1 = glGenLists(1);
    glNewList(gear1, GL_COMPILE);
    {
        GLfloat red[4] = { 0.8f, 0.1f, 0.0f, 1.0f };
        glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, red);
        build_gear(1.0f, 4.0f, 1.0f, 20, 0.7f);
    }
    glEndList();

    GLuint gear2 = glGenLists(1);
    glNewList(gear2, GL_COMPILE);
    {
        GLfloat green[4] = { 0.0f, 0.8f, 0.2f, 1.0f };
        glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, green);
        build_gear(0.5f, 2.0f, 2.0f, 10, 0.7f);
    }
    glEndList();

    GLuint gear3 = glGenLists(1);
    glNewList(gear3, GL_COMPILE);
    {
        GLfloat blue[4] = { 0.2f, 0.2f, 1.0f, 1.0f };
        glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, blue);
        build_gear(1.3f, 2.0f, 0.5f, 10, 0.7f);
    }
    glEndList();

    GLfloat view_rotx = 20.0f, view_roty = 30.0f, view_rotz = 0.0f;
    GLfloat angle = 0.0f;
    int running = 1, paused = 0;
    long frames = 0;

    while (running) {
        ag_event_t ev;
        while (ag_poll_event(wid, &ev) > 0) {
            switch (ev.type) {
            case AG_EVT_CLOSE_REQ: running = 0; break;
            case AG_EVT_KEY_DOWN:
                if (ev.key == 'q' || ev.key == 27) running = 0;
                else if (ev.key == ' ') paused = !paused;
                else if (ev.key == 'a') view_roty -= 5.0f;
                else if (ev.key == 'd') view_roty += 5.0f;
                else if (ev.key == 'w') view_rotx -= 5.0f;
                else if (ev.key == 's') view_rotx += 5.0f;
                break;
            default: break;
            }
        }
        if (!running) break;

        if (!paused) angle += 2.0f;
        if (angle >= 360.0f) angle -= 360.0f;

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glPushMatrix();
        glTranslatef(0.0f, 0.0f, -40.0f);
        glRotatef(view_rotx, 1.0f, 0.0f, 0.0f);
        glRotatef(view_roty, 0.0f, 1.0f, 0.0f);
        glRotatef(view_rotz, 0.0f, 0.0f, 1.0f);

        glPushMatrix();
        glTranslatef(-3.0f, -2.0f, 0.0f);
        glRotatef(angle, 0.0f, 0.0f, 1.0f);
        glCallList(gear1);
        glPopMatrix();

        glPushMatrix();
        glTranslatef(3.1f, -2.0f, 0.0f);
        /* The -2x ratio and 9-degree offset are what make the teeth mesh. */
        glRotatef(-2.0f * angle - 9.0f, 0.0f, 0.0f, 1.0f);
        glCallList(gear2);
        glPopMatrix();

        glPushMatrix();
        glTranslatef(-3.1f, 4.2f, 0.0f);
        glRotatef(-2.0f * angle - 25.0f, 0.0f, 0.0f, 1.0f);
        glCallList(gear3);
        glPopMatrix();

        glPopMatrix();

        aglxSwapBuffers(ctx);

        frames++;
        if (max_frames > 0 && frames >= max_frames) {
            printf("glgears: rendered %ld frames\n", frames);
            break;
        }
    }

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        printf("glgears: GL error 0x%04x (%s)\n", err,
               (const char *)gluErrorString(err));
    } else {
        printf("glgears: clean exit, %ld frames\n", frames);
    }

    glDeleteLists(gear1, 1);
    glDeleteLists(gear2, 1);
    glDeleteLists(gear3, 1);
    aglxDestroyContext(ctx);
    ag_window_destroy(wid);
    return 0;
}

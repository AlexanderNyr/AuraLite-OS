/* glcube.c — rotating cube demo for the AuraLite OpenGL stack.
 *
 * Phase G2 renders it as a wireframe, because the triangle rasterizer does not
 * exist yet.  The SAME source becomes a solid shaded cube in G3 with no
 * changes: that is the point of writing it against the GL API rather than
 * against the rasterizer.
 *
 * Controls:
 *   arrow keys / mouse drag  rotate
 *   space                    pause
 *   q / Esc / window close   quit
 *
 * Frame limit for automated testing:
 *   The shell's "run" command uses spawn(), which does not forward argv, so a
 *   command-line frame count would never arrive.  The project's existing
 *   convention for this (see cmd_apm in userspace/init/init.c) is to pass
 *   arguments through a file, so /glcube reads an optional decimal frame count
 *   from /tmp/glcube.frames.  Absent or unreadable means "run until closed".
 *   The integration test writes that file so the demo cannot hang CI.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "auragui.h"
#include "GL/gl.h"
#include "GL/auraglx.h"

/* Modest resolution: a software rasterizer under emulation should not be
 * asked to fill a large buffer to prove a point. */
#define CUBE_W 320
#define CUBE_H 240

#define WIN_W (CUBE_W + 16)
#define WIN_H (CUBE_H + 48)

/* Cube corners. */
static const GLfloat verts[8][3] = {
    { -1, -1, -1 }, {  1, -1, -1 }, {  1,  1, -1 }, { -1,  1, -1 },
    { -1, -1,  1 }, {  1, -1,  1 }, {  1,  1,  1 }, { -1,  1,  1 },
};

/* Faces as quads, each with its own colour.  Vertex order is counter-clockwise
 * when viewed from outside, so back-face culling works once G3 enables it. */
static const int faces[6][4] = {
    { 0, 3, 2, 1 },   /* back   (-z) */
    { 4, 5, 6, 7 },   /* front  (+z) */
    { 0, 4, 7, 3 },   /* left   (-x) */
    { 1, 2, 6, 5 },   /* right  (+x) */
    { 0, 1, 5, 4 },   /* bottom (-y) */
    { 3, 7, 6, 2 },   /* top    (+y) */
};

static const GLfloat face_colors[6][3] = {
    { 1.0f, 0.3f, 0.3f },   /* red    */
    { 0.3f, 1.0f, 0.3f },   /* green  */
    { 0.3f, 0.5f, 1.0f },   /* blue   */
    { 1.0f, 1.0f, 0.3f },   /* yellow */
    { 1.0f, 0.5f, 0.2f },   /* orange */
    { 0.8f, 0.4f, 1.0f },   /* purple */
};

static void draw_cube(void) {
    glBegin(GL_QUADS);
    for (int f = 0; f < 6; f++) {
        glColor3f(face_colors[f][0], face_colors[f][1], face_colors[f][2]);
        for (int v = 0; v < 4; v++) {
            const GLfloat *p = verts[faces[f][v]];
            glVertex3f(p[0], p[1], p[2]);
        }
    }
    glEnd();
}

/* Ground grid, to make the perspective projection obvious. */
static void draw_grid(void) {
    glColor3f(0.25f, 0.25f, 0.30f);
    glBegin(GL_LINES);
    for (int i = -4; i <= 4; i++) {
        GLfloat t = (GLfloat)i * 0.75f;
        glVertex3f(t, -1.6f, -3.0f);  glVertex3f(t, -1.6f, 3.0f);
        glVertex3f(-3.0f, -1.6f, t);  glVertex3f(3.0f, -1.6f, t);
    }
    glEnd();
}

/* Read the optional frame limit from /tmp/glcube.frames (see the note above). */
static long read_frame_limit(void) {
    int fd = open("/tmp/glcube.frames", O_RDONLY);
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
    long max_frames = read_frame_limit();  /* 0 = run until closed */

    int wid = ag_window_create(60, 40, WIN_W, WIN_H, "OpenGL Cube",
                               AG_WIN_DEFAULT);
    if (wid < 0) {
        printf("glcube: cannot create window\n");
        return 1;
    }
    ag_window_show(wid);

    aglx_context_t *ctx = aglxCreateContext(wid, CUBE_W, CUBE_H, AGLX_DEFAULT);
    if (!ctx) {
        printf("glcube: cannot create GL context\n");
        ag_window_destroy(wid);
        return 1;
    }
    aglxMakeCurrent(ctx);

    printf("glcube: %s | %s\n",
           (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION));

    /* Projection is set once: it only depends on the buffer size. */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    {
        /* gluPerspective(45, aspect, 1, 50) written as a frustum, since GLU
         * does not exist until phase G8. */
        double aspect = (double)CUBE_W / (double)CUBE_H;
        double top    = 1.0 * 0.41421356;    /* tan(45deg / 2) */
        glFrustum(-top * aspect, top * aspect, -top, top, 1.0, 50.0);
    }
    glMatrixMode(GL_MODELVIEW);

    glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
    glShadeModel(GL_SMOOTH);

    GLfloat angle_x = 25.0f, angle_y = 30.0f;
    int paused = 0, running = 1;
    long frames = 0;

    /* Mouse-drag rotation state. */
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

        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -6.0f);   /* move the scene away from the eye */

        glPushMatrix();
        draw_grid();                        /* grid does not spin */
        glPopMatrix();

        glRotatef(angle_x, 1.0f, 0.0f, 0.0f);
        glRotatef(angle_y, 0.0f, 1.0f, 0.0f);
        draw_cube();

        aglxSwapBuffers(ctx);

        frames++;
        if (max_frames > 0 && frames >= max_frames) {
            printf("glcube: rendered %ld frames\n", frames);
            break;
        }
    }

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) printf("glcube: GL error 0x%04x\n", err);
    else                    printf("glcube: clean exit, %ld frames\n", frames);

    aglxDestroyContext(ctx);
    ag_window_destroy(wid);
    return 0;
}

/* glcube.c — rotating cube demo for the AuraLite OpenGL stack.
 *
 * From phase G7 the cube geometry is compiled into a DISPLAY LIST once and
 * replayed each frame, and the ground grid is drawn from a vertex array — so
 * the demo exercises both G7 submission paths rather than only immediate mode.  The geometry and
 * matrix code are unchanged from the G2 wireframe version — only the two
 * glEnable() calls below were added, which is the point of writing the demo
 * against the GL API rather than against the rasterizer.
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

/* Outward normal of each face, in the same order as `faces`. */
static const GLfloat face_normals[6][3] = {
    {  0,  0, -1 },   /* back   */
    {  0,  0,  1 },   /* front  */
    { -1,  0,  0 },   /* left   */
    {  1,  0,  0 },   /* right  */
    {  0, -1,  0 },   /* bottom */
    {  0,  1,  0 },   /* top    */
};

static const GLfloat face_colors[6][3] = {
    { 1.0f, 0.3f, 0.3f },   /* red    */
    { 0.3f, 1.0f, 0.3f },   /* green  */
    { 0.3f, 0.5f, 1.0f },   /* blue   */
    { 1.0f, 1.0f, 0.3f },   /* yellow */
    { 1.0f, 0.5f, 0.2f },   /* orange */
    { 0.8f, 0.4f, 1.0f },   /* purple */
};

/* Build a procedural checkerboard.  Generating it in code keeps the demo free
 * of asset files, which the initrd would otherwise have to carry. */
static GLuint make_checker_texture(void) {
    #define TEX_N 32
    static unsigned char img[TEX_N * TEX_N * 3];
    for (int y = 0; y < TEX_N; y++) {
        for (int x = 0; x < TEX_N; x++) {
            int on = ((x / 4) + (y / 4)) & 1;
            unsigned char v = on ? 235 : 120;
            unsigned char *p = &img[(y * TEX_N + x) * 3];
            p[0] = v; p[1] = v; p[2] = v;
        }
    }
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, TEX_N, TEX_N, 0, GL_RGB,
                 GL_UNSIGNED_BYTE, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    #undef TEX_N
    return id;
}

/* ---- Mipmapped ground plane (phase G10) ----
 *
 * A textured floor is the canonical mipmap demonstration: at a grazing angle
 * the texture is minified by a large and rapidly changing factor, which is
 * exactly where point sampling produces the moire shimmer that mipmapping
 * exists to remove.
 *
 * The plane is TESSELLATED into a grid of quads rather than drawn as a single
 * large one.  That is not decoration: this implementation picks one mipmap
 * level per triangle (see the note at the top of libgl/src/gltexture.c), so a
 * single quad stretching to the horizon would get one averaged level across
 * the whole thing -- too blurry near the camera and still aliased far away.
 * Sixteen strips give each piece a level that suits its own depth.
 */
#define FLOOR_TEX_N   64
#define FLOOR_TILES   16
#define FLOOR_EXTENT  6.0f
#define FLOOR_Y      -1.61f     /* just below the wireframe grid */

static GLuint make_floor_texture(void) {
    static unsigned char img[FLOOR_TEX_N * FLOOR_TEX_N * 3];
    for (int y = 0; y < FLOOR_TEX_N; y++) {
        for (int x = 0; x < FLOOR_TEX_N; x++) {
            /* A fine 2x2-texel checker: minified even slightly, this is the
             * pattern that aliases worst, which is the point. */
            int on = ((x / 2) + (y / 2)) & 1;
            unsigned char *p = &img[(y * FLOOR_TEX_N + x) * 3];
            p[0] = on ? 210 : 40;
            p[1] = on ? 210 : 55;
            p[2] = on ? 225 : 90;
        }
    }
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, FLOOR_TEX_N, FLOOR_TEX_N, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, img);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return id;
}

static void draw_floor(GLuint floor_tex) {
    glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, floor_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

    GLfloat step = 2.0f * FLOOR_EXTENT / (GLfloat)FLOOR_TILES;
    GLfloat uv_step = 2.0f;             /* texture repeats per tile */

    glBegin(GL_QUADS);
    for (int iz = 0; iz < FLOOR_TILES; iz++) {
        for (int ix = 0; ix < FLOOR_TILES; ix++) {
            GLfloat x0 = -FLOOR_EXTENT + (GLfloat)ix * step;
            GLfloat z0 = -FLOOR_EXTENT + (GLfloat)iz * step;
            GLfloat x1 = x0 + step, z1 = z0 + step;
            GLfloat u0 = (GLfloat)ix * uv_step, v0 = (GLfloat)iz * uv_step;
            GLfloat u1 = u0 + uv_step, v1 = v0 + uv_step;

            glTexCoord2f(u0, v0); glVertex3f(x0, FLOOR_Y, z0);
            glTexCoord2f(u1, v0); glVertex3f(x1, FLOOR_Y, z0);
            glTexCoord2f(u1, v1); glVertex3f(x1, FLOOR_Y, z1);
            glTexCoord2f(u0, v1); glVertex3f(x0, FLOOR_Y, z1);
        }
    }
    glEnd();

    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glEnable(GL_LIGHTING);
}

/* ---- Render-to-texture mirror (phase G12) ----
 *
 * A second view of the same cube is rendered into a texture through a
 * framebuffer object, and that texture is then pasted onto a panel in the
 * corner of the window -- a picture-in-picture "security monitor".
 *
 * It is a cheap demonstration with a real property to check: the panel must
 * show the scene the RIGHT WAY UP.  A texture stores row 0 at the bottom and
 * the window's framebuffer stores it at the top, so an implementation that
 * flips unconditionally renders correctly to the window and upside-down into
 * a texture.  That bug happened during this phase; the panel is what makes it
 * visible at a glance rather than only in a unit test.
 */
#define MIRROR_N 64

static GLuint mirror_tex = 0;
static GLuint mirror_fbo = 0;
static GLuint mirror_depth = 0;
static int    mirror_ok = 0;

static void mirror_init(void) {
    glGenTextures(1, &mirror_tex);
    glBindTexture(GL_TEXTURE_2D, mirror_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, MIRROR_N, MIRROR_N, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* The mirror needs its own depth buffer: the window's belongs to the
     * window, and rendering a 3D scene without depth would show the cube's
     * back faces through its front ones. */
    glGenRenderbuffers(1, &mirror_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, mirror_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                          MIRROR_N, MIRROR_N);

    glGenFramebuffers(1, &mirror_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, mirror_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, mirror_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, mirror_depth);

    mirror_ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER)
                 == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!mirror_ok) printf("glcube: mirror FBO incomplete, panel disabled\n");
}

/* Texture coordinates for the four corners of every face. */
static const GLfloat face_uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };

static void draw_cube(void) {
    glBegin(GL_QUADS);
    for (int f = 0; f < 6; f++) {
        glColor3f(face_colors[f][0], face_colors[f][1], face_colors[f][2]);
        glNormal3f(face_normals[f][0], face_normals[f][1], face_normals[f][2]);
        for (int v = 0; v < 4; v++) {
            const GLfloat *p = verts[faces[f][v]];
            glTexCoord2f(face_uv[v][0], face_uv[v][1]);
            glVertex3f(p[0], p[1], p[2]);
        }
    }
    glEnd();
}

/* Ground grid, to make the perspective projection obvious. */
/* Grid vertices, built once and drawn from a client vertex array (phase G7). */
#define GRID_LINES 9
#define GRID_VERTS (GRID_LINES * 4)
static GLfloat grid_verts[GRID_VERTS * 3];

static void build_grid(void) {
    int v = 0;
    for (int i = -4; i <= 4; i++) {
        GLfloat t = (GLfloat)i * 0.75f;
        grid_verts[v*3+0] = t;     grid_verts[v*3+1] = -1.6f; grid_verts[v*3+2] = -3.0f; v++;
        grid_verts[v*3+0] = t;     grid_verts[v*3+1] = -1.6f; grid_verts[v*3+2] =  3.0f; v++;
        grid_verts[v*3+0] = -3.0f; grid_verts[v*3+1] = -1.6f; grid_verts[v*3+2] = t;     v++;
        grid_verts[v*3+0] =  3.0f; grid_verts[v*3+1] = -1.6f; grid_verts[v*3+2] = t;     v++;
    }
}

static void draw_grid(void) {
    /* The grid is a flat, untextured reference, not a lit surface. */
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glColor3f(0.25f, 0.25f, 0.30f);

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, grid_verts);
    glDrawArrays(GL_LINES, 0, GRID_VERTS);
    glDisableClientState(GL_VERTEX_ARRAY);

    glEnable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
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
    build_grid();

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

    /* Phase G3: a depth buffer resolves which face is in front per pixel, and
     * culling skips the three faces pointing away from the camera (roughly
     * halving the fill cost).  The cube's faces are wound counter-clockwise
     * when seen from outside, so the defaults are correct. */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    /* Phase G5: a single white light above and to the right of the camera.
     * GL_COLOR_MATERIAL lets the per-face glColor calls keep working while
     * lighting is on: each face's colour becomes its ambient+diffuse material,
     * so the cube stays multi-coloured but now has directional shading. */
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_NORMALIZE);
    {
        GLfloat light_pos[4] = { 2.0f, 3.0f, 4.0f, 0.0f };  /* directional */
        GLfloat diffuse[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
        GLfloat ambient[4]   = { 0.25f, 0.25f, 0.3f, 1.0f };
        GLfloat specular[4]  = { 0.9f, 0.9f, 0.9f, 1.0f };
        glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
        glLightfv(GL_LIGHT0, GL_AMBIENT, ambient);
        glLightfv(GL_LIGHT0, GL_SPECULAR, specular);
        glMaterialfv(GL_FRONT, GL_SPECULAR, specular);
        glMaterialf(GL_FRONT, GL_SHININESS, 24.0f);
    }

    /* Phase G6: a checkerboard modulated with the per-face colour, so the
     * texture darkens and tints rather than replacing the shading. */
    GLuint checker = make_checker_texture();
    /* Phase G10: the mipmapped floor, built before the cube's own texture is
     * bound so the binding left current below is the cube's. */
    GLuint floor_tex = make_floor_texture();
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, checker);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    /* Phase G7: compile the cube once.  Its geometry never changes, only the
     * matrix around it, which is exactly what display lists are for. */
    GLuint cube_list = glGenLists(1);
    if (cube_list != 0) {
        glNewList(cube_list, GL_COMPILE);
        draw_cube();
        glEndList();
    }

    /* Phase G12: the off-screen target for the picture-in-picture panel. */
    mirror_init();

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

        /* ---- Pass 1 (G12): the same cube, from above, into a texture ----
         *
         * Rendered BEFORE the window pass so the panel shows the current
         * frame rather than the previous one. */
        if (mirror_ok) {
            glBindFramebuffer(GL_FRAMEBUFFER, mirror_fbo);
            glViewport(0, 0, MIRROR_N, MIRROR_N);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            /* A square 50-degree frustum, written out rather than calling
             * gluPerspective: this demo deliberately depends on libgl only. */
            {
                double t = 1.0 * 0.46630766;   /* tan(50deg / 2) */
                glFrustum(-t, t, -t, t, 1.0, 40.0);
            }
            glMatrixMode(GL_MODELVIEW);

            glClearColor(0.05f, 0.05f, 0.12f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            glLoadIdentity();
            /* A high, tilted vantage point, so the panel is visibly a
             * different view and not a copy of the main one. */
            glTranslatef(0.0f, 0.0f, -7.0f);
            glRotatef(55.0f, 1.0f, 0.0f, 0.0f);
            glRotatef(angle_y, 0.0f, 1.0f, 0.0f);
            if (cube_list != 0) glCallList(cube_list);
            else                draw_cube();

            /* Back to the window, and restore its projection. */
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, CUBE_W, CUBE_H);
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            {
                double aspect = (double)CUBE_W / (double)CUBE_H;
                double top    = 1.0 * 0.41421356;   /* tan(45deg / 2) */
                glFrustum(-top * aspect, top * aspect, -top, top, 1.0, 50.0);
            }
            glMatrixMode(GL_MODELVIEW);
            glClearColor(0.05f, 0.06f, 0.10f, 1.0f);
        }

        /* ---- Pass 2: the window ---- */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glLoadIdentity();
        glTranslatef(0.0f, 0.0f, -6.0f);   /* move the scene away from the eye */

        glPushMatrix();
        draw_floor(floor_tex);              /* mipmapped ground (G10) */
        glBindTexture(GL_TEXTURE_2D, checker);
        draw_grid();                        /* grid does not spin */
        glPopMatrix();

        glRotatef(angle_x, 1.0f, 0.0f, 0.0f);
        glRotatef(angle_y, 0.0f, 1.0f, 0.0f);
        if (cube_list != 0) glCallList(cube_list);
        else                draw_cube();

        /* ---- The picture-in-picture panel ----
         *
         * Drawn last, in an orthographic overlay, with depth testing off so
         * it always sits on top of the scene. */
        if (mirror_ok) {
            glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
            glOrtho(0, CUBE_W, 0, CUBE_H, -1, 1);
            glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();

            glDisable(GL_DEPTH_TEST);
            glDisable(GL_LIGHTING);
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, mirror_tex);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
            glColor3f(1.0f, 1.0f, 1.0f);

            GLfloat px0 = (GLfloat)(CUBE_W - MIRROR_N - 8);
            GLfloat py0 = (GLfloat)(CUBE_H - MIRROR_N - 8);
            GLfloat px1 = px0 + MIRROR_N, py1 = py0 + MIRROR_N;

            glBegin(GL_QUADS);
            glTexCoord2f(0, 0); glVertex3f(px0, py0, 0);
            glTexCoord2f(1, 0); glVertex3f(px1, py0, 0);
            glTexCoord2f(1, 1); glVertex3f(px1, py1, 0);
            glTexCoord2f(0, 1); glVertex3f(px0, py1, 0);
            glEnd();

            /* A thin frame, so the panel reads as an inset rather than as a
             * rendering artefact. */
            glDisable(GL_TEXTURE_2D);
            glColor3f(0.6f, 0.7f, 0.9f);
            glBegin(GL_LINE_LOOP);
            glVertex3f(px0 - 1, py0 - 1, 0); glVertex3f(px1 + 1, py0 - 1, 0);
            glVertex3f(px1 + 1, py1 + 1, 0); glVertex3f(px0 - 1, py1 + 1, 0);
            glEnd();

            glEnable(GL_TEXTURE_2D);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            glBindTexture(GL_TEXTURE_2D, checker);
            glEnable(GL_LIGHTING);
            glEnable(GL_DEPTH_TEST);

            glMatrixMode(GL_PROJECTION); glPopMatrix();
            glMatrixMode(GL_MODELVIEW);  glPopMatrix();
        }

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

    if (cube_list != 0) glDeleteLists(cube_list, 1);
    if (mirror_fbo)   glDeleteFramebuffers(1, &mirror_fbo);
    if (mirror_depth) glDeleteRenderbuffers(1, &mirror_depth);
    if (mirror_tex)   glDeleteTextures(1, &mirror_tex);
    aglxDestroyContext(ctx);
    ag_window_destroy(wid);
    return 0;
}

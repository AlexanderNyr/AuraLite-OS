/*
 * wv_canvas.c — <canvas> rendering (WEBVIEW_PLAN W7).  See wv_canvas.h.
 *
 * The scene is a lit, depth-buffered cube drawn with the immediate-mode
 * API — the same primitives /glcube uses, minus the texture pass, sized
 * to the canvas.  Rendered into an FBO (G12) and read back with
 * glReadPixels, which returns rows BOTTOM-first (GL convention), so the
 * readback is flipped into the page's top-left-origin XRGB buffer.
 *
 * This file is the only web-view module that includes GL headers: the
 * layout and paint layers stay GL-free, and the host unit test links the
 * real libgl sources (LIBGL_TEST_SRCS) exactly like the other GL tests.
 */

#define _POSIX_C_SOURCE 200809L   /* clock_gettime */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "GL/gl.h"
#include "GL/auraglx.h"
#include "wv_canvas.h"

/* ---- the cube scene (same geometry as /glcube) ---- */

static const float cverts[8][3] = {
    { -1, -1, -1 }, {  1, -1, -1 }, {  1,  1, -1 }, { -1,  1, -1 },
    { -1, -1,  1 }, {  1, -1,  1 }, {  1,  1,  1 }, { -1,  1,  1 },
};
static const int cfaces[6][4] = {
    { 0, 3, 2, 1 },   /* back    */
    { 4, 5, 6, 7 },   /* front   */
    { 0, 4, 7, 3 },   /* left    */
    { 1, 2, 6, 5 },   /* right   */
    { 0, 1, 5, 4 },   /* bottom  */
    { 3, 7, 6, 2 },   /* top     */
};
static const float cnormals[6][3] = {
    { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 },
    { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 },
};
static const float ccolors[6][3] = {
    { 0.9f, 0.3f, 0.3f }, { 0.3f, 0.9f, 0.3f }, { 0.3f, 0.5f, 0.9f },
    { 0.9f, 0.9f, 0.3f }, { 0.9f, 0.5f, 0.2f }, { 0.8f, 0.4f, 0.9f },
};

static void wv_cube_draw(void) {
    glBegin(GL_QUADS);
    for (int f = 0; f < 6; f++) {
        glColor3f(ccolors[f][0], ccolors[f][1], ccolors[f][2]);
        glNormal3f(cnormals[f][0], cnormals[f][1], cnormals[f][2]);
        for (int v = 0; v < 4; v++) {
            const float *p = cverts[cfaces[f][v]];
            glVertex3f(p[0], p[1], p[2]);
        }
    }
    glEnd();
}

int wv_canvas_render_cube(uint32_t *out, int w, int h, int wid,
                          long *render_us) {
    if (!out || w < 8 || h < 8 || w > 256 || h > 256) return -1;

    aglx_context_t *ctx = aglxCreateContext(wid, w, h, 0);
    if (!ctx) return -1;
    if (aglxMakeCurrent(ctx) != 0) {
        aglxDestroyContext(ctx);
        return -1;
    }

    /* FBO: colour texture + depth renderbuffer (the G12 pattern). */
    GLuint tex = 0, rb = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, rb);
    GLenum fst = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fst != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteRenderbuffers(1, &rb);
        glDeleteTextures(1, &tex);
        aglxDestroyContext(ctx);
        return -1;
    }

    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    {
        double t = 0.41421356;                 /* tan(45/2) */
        double asp = (double)w / (double)h;
        glFrustum(-t * asp, t * asp, -t, t, 1.0, 50.0);
    }
    glMatrixMode(GL_MODELVIEW);
    glEnable(GL_DEPTH_TEST);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    glClearColor(0.08f, 0.10f, 0.16f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -4.5f);
    glRotatef(28.0f, 1.0f, 0.0f, 0.0f);
    glRotatef(33.0f, 0.0f, 1.0f, 0.0f);
    wv_cube_draw();

    /* Readback: RGBA bytes, rows bottom-first (GL convention). */
    unsigned char *tmp = (unsigned char *)malloc((size_t)w * (size_t)h * 4);
    if (!tmp) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteRenderbuffers(1, &rb);
        glDeleteTextures(1, &tex);
        aglxDestroyContext(ctx);
        return -1;
    }
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long sec = t1.tv_sec - t0.tv_sec;
    long nsec = t1.tv_nsec - t0.tv_nsec;
    if (nsec < 0) { sec--; nsec += 1000000000L; }
    if (render_us) *render_us = sec * 1000000L + nsec / 1000L;

    /* Flip rows and pack into XRGB8888 (top-left origin). */
    for (int y = 0; y < h; y++) {
        const unsigned char *src = tmp + (size_t)(h - 1 - y) * (size_t)w * 4;
        uint32_t *dst = out + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            uint32_t r = src[x * 4 + 0];
            uint32_t g = src[x * 4 + 1];
            uint32_t b = src[x * 4 + 2];
            dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    free(tmp);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &rb);
    glDeleteTextures(1, &tex);
    aglxDestroyContext(ctx);
    return 0;
}

void wv_canvas_blit(uint32_t *page, int pw, int ph,
                    const uint32_t *cv, int cw, int ch,
                    int x, int y, int scroll_y) {
    if (!page || !cv || cw <= 0 || ch <= 0) return;
    int py = y - scroll_y;
    if (py + ch <= 0 || py >= ph) return;        /* fully off-screen */
    for (int row = 0; row < ch; row++) {
        int yy = py + row;
        if (yy < 0 || yy >= ph) continue;
        const uint32_t *src = cv + (size_t)row * (size_t)cw;
        uint32_t *dst = page + (size_t)yy * (size_t)pw;
        int x0 = x < 0 ? 0 : x;
        int x1 = x + cw;
        if (x1 > pw) x1 = pw;
        for (int xx = x0; xx < x1; xx++)
            dst[xx] = src[xx - x];
    }
}

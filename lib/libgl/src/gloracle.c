/* lib/libgl/src/gloracle.c — GPU_ACCEL_PLAN H6: the frame oracle.
 *
 * See gloracle.h for what the oracle is for and what its tolerance means.
 * This file is deliberately small and boring: a fixed scene, a hash, a
 * comparison.  Everything that could make two runs differ is either absent
 * (no clock, no RNG, no allocation in the scene) or fixed here (the viewport,
 * the projection, the colours, the depth function).
 *
 * THE SCENE, AND WHY THIS ONE
 *
 * H5 made a depth-tested, blended, textured draw eligible.  A scene that
 * exercises none of those would let a regression in any of them through.  So
 * the scene is two triangles forming a quad, drawn with the depth test on and
 * the two faces at different depths, with per-vertex colours that differ in
 * every channel: a wrong vertex format shows up as swapped/missing colour, a
 * wrong depth function shows up as the far triangle winning, and a wrong
 * stride shows up as geometry that does not reach its vertices.  Each of
 * those is a mutation case in test_gloracle.c.
 *
 * The scene stops short of texturing on purpose.  A texture's presence in a
 * hash depends on the sampler's filtering, which is exactly the sort of thing
 * that legitimately differs between implementations; a textured oracle would
 * need a tolerance wide enough to hide real regressions.  Texture coverage is
 * H3's own tests' business (test_tex_packets, test_glvirgl's inventory).
 */

#include "gloracle.h"

#include "GL/gl.h"
#include "GL/auraglx.h"
#include "glcontext.h"

#include <string.h>

#include "../../../kernel/lib/sha256.h"

void gloracle_hash(const uint32_t *pixels, uint8_t out[32]) {
    ksha256(pixels, (size_t)GLORACLE_W * GLORACLE_H * sizeof(uint32_t), out);
}

void gloracle_hash_hex(const uint32_t *pixels, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t d[32];
    gloracle_hash(pixels, d);
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hex[d[i] >> 4];
        out[i * 2 + 1] = hex[d[i] & 0x0F];
    }
    out[64] = '\0';
}

void gloracle_compare(const uint32_t *a, const uint32_t *b,
                      gloracle_verdict_t *v) {
    memset(v, 0, sizeof *v);
    v->pixels_total   = (uint32_t)GLORACLE_W * GLORACLE_H;
    v->tolerance_pixels =
        (v->pixels_total * GLORACLE_TOLERANCE_PERMILLE) / 1000u;

    for (uint32_t i = 0; i < v->pixels_total; i++) {
        uint32_t pa = a[i], pb = b[i];
        int over = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            int ca = (int)((pa >> sh) & 0xFFu);
            int cb = (int)((pb >> sh) & 0xFFu);
            int d  = ca > cb ? ca - cb : cb - ca;
            if ((uint32_t)d > v->max_channel_delta)
                v->max_channel_delta = (uint32_t)d;
            /* A channel past the epsilon makes the WHOLE pixel count as
             * differing: that is the rule that stops a half-brightness band
             * from hiding inside "small numeric noise" on every pixel. */
            if ((uint32_t)d > GLORACLE_CHANNEL_EPSILON) over = 1;
        }
        if (over) v->pixels_differing++;
    }
    v->pass = v->pixels_differing <= v->tolerance_pixels;
}

int gloracle_render(uint32_t *out) {
    if (!out) return -1;

    /* The oracle creates and destroys its own context, which means it must
     * PUT BACK whatever was current before.  Without this, aglxDestroyContext
     * left a dangling current context and the caller's next glGetError()
     * read freed memory -- a real defect this phase's own h6_no_pending_error
     * check caught on its first run in the guest (0x502 from a context that
     * no longer existed). */
    /* NOTE: `prev` may be a DANGLING pointer.  gltest's L6 block destroys its
     * context without unbinding it first, so the "previous" context can
     * already be freed by the time the oracle runs.  Installing a dangling
     * pointer as current would be worse than leaving none, so the oracle
     * unbinds instead when there was nothing valid to restore -- the caller
     * gets a clean NULL current, which is a legal state (aglxMakeCurrent
     * accepts NULL) rather than a use-after-free waiting to happen. */
    aglx_context_t *prev = aglxGetCurrentContext();

    aglx_context_t *ctx = aglxCreateContext(0, GLORACLE_W, GLORACLE_H,
                                            AGLX_DEPTH);
    if (!ctx) return -1;
    aglxMakeCurrent(ctx);

    /* Fixed viewport and an orthographic projection in PIXELS, so a vertex at
     * (x, y) lands at pixel (x, y) no matter what the window's size is.  The
     * depth range is the default [-1, 1] mapped to [0, 1] by the common
     * transform; the two triangles differ in z by enough that no rounding can
     * swap their order. */
    glViewport(0, 0, GLORACLE_W, GLORACLE_H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, GLORACLE_W, 0, GLORACLE_H, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* The background is a known colour with alpha 255, so "nothing was drawn"
     * is a distinguishable frame rather than a zero buffer. */
    glClearColor(0.125f, 0.25f, 0.5f, 1.0f);
    glClearDepth(1.0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);          /* the tolerance model's term 2 needs none */
    glDisable(GL_CULL_FACE);      /* both faces are meant to be visible     */
    glShadeModel(GL_SMOOTH);

    /* Triangle A: the NEAR one, z = 0.0.  Drawn first -- if the depth test is
     * wrong in the "later wins" direction, B overwrites it and the hash moves
     * by far more than an edge pixel. */
    glBegin(GL_TRIANGLES);
      glColor3f(1.0f, 0.0f, 0.0f); glVertex3f( 4.0f,  4.0f, 0.0f);
      glColor3f(0.0f, 1.0f, 0.0f); glVertex3f(60.0f,  4.0f, 0.0f);
      glColor3f(0.0f, 0.0f, 1.0f); glVertex3f( 4.0f, 60.0f, 0.0f);
    glEnd();

    /* Triangle B: the FAR one, z = 0.5, overlapping A's hypotenuse.  With
     * GL_LESS it must lose every pixel where they overlap. */
    glBegin(GL_TRIANGLES);
      glColor3f(1.0f, 1.0f, 0.0f); glVertex3f(60.0f, 60.0f, 0.5f);
      glColor3f(0.0f, 1.0f, 1.0f); glVertex3f(60.0f,  4.0f, 0.5f);
      glColor3f(1.0f, 0.0f, 1.0f); glVertex3f( 4.0f, 60.0f, 0.5f);
    glEnd();

    glDisable(GL_DEPTH_TEST);
    glFinish();

    /* glReadPixels returns rows bottom-to-top, which is the orientation the
     * GL coordinate system agrees with; the oracle uses it rather than poking
     * the context's buffer so the comparison is on the same surface a guest
     * could read. */
    glReadPixels(0, 0, GLORACLE_W, GLORACLE_H, GL_RGBA, GL_UNSIGNED_BYTE, out);

    glGetError();                 /* clear anything a driver left behind */
    aglxDestroyContext(ctx);
    /* Restore only if the pointer is still a live context; see the note at
     * the top of the function.  aglxDestroyContext() clears the current slot
     * when the context IS current, so a stale `prev` shows up here as a
     * pointer the library no longer considers live. */
    aglxMakeCurrent(prev);        /* NULL is a legal, clean state */
    return 0;
}

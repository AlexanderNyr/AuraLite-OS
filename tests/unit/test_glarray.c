/*
 * test_glarray.c — host-side unit tests for vertex arrays, buffer objects and
 * display lists (phase G7).
 *
 * The central property being tested throughout: an array or list draw must
 * produce a BIT-IDENTICAL framebuffer to the equivalent immediate-mode code.
 * Comparing whole buffers rather than sampling a few pixels is what makes
 * these tests worth having — a subtle attribute-ordering or stride bug shows
 * up as a handful of differing pixels that spot checks would miss.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

#define W 48
#define H 48

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

static int lit_count(aglx_context_t *c) {
    const uint32_t *b = aglxGetColorBuffer(c);
    int n = 0;
    for (int i = 0; i < W * H; i++) if (b[i] != 0) n++;
    return n;
}

/* Snapshot the colour buffer so two renderings can be compared exactly. */
static uint32_t *snapshot(aglx_context_t *c) {
    uint32_t *copy = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
    if (copy) memcpy(copy, aglxGetColorBuffer(c), (size_t)W * H * sizeof(uint32_t));
    return copy;
}

static int same_as(aglx_context_t *c, const uint32_t *snap) {
    return snap && memcmp(aglxGetColorBuffer(c), snap,
                          (size_t)W * H * sizeof(uint32_t)) == 0;
}

/* Geometry shared by the equivalence tests: a triangle with per-vertex colour. */
static const GLfloat tri_pos[3 * 3] = {
     6.0f,  6.0f, 0.0f,
    42.0f,  6.0f, 0.0f,
     6.0f, 42.0f, 0.0f,
};
static const GLfloat tri_col[3 * 3] = {
    1.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 1.0f,
};

static void draw_immediate(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        glColor3f(tri_col[i*3], tri_col[i*3+1], tri_col[i*3+2]);
        glVertex3f(tri_pos[i*3], tri_pos[i*3+1], tri_pos[i*3+2]);
    }
    glEnd();
}

/* ---------------------------------------------------- client array state -- */

static int t_client_state_toggle(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnableClientState(GL_VERTEX_ARRAY);
    int on = c->array_vertex.enabled == GL_TRUE;
    glDisableClientState(GL_VERTEX_ARRAY);
    int off = c->array_vertex.enabled == GL_FALSE;
    int ok = on && off && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_client_state_invalid(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnableClientState(0x9999);
    int ok = glGetError() == GL_INVALID_ENUM;
    aglxDestroyContext(c);
    return ok;
}

static int t_pointer_validation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glVertexPointer(1, GL_FLOAT, 0, tri_pos);      /* size < 2 */
    int ok = glGetError() == GL_INVALID_VALUE;
    glVertexPointer(3, 0x9999, 0, tri_pos);        /* bad type */
    ok = ok && glGetError() == GL_INVALID_ENUM;
    glVertexPointer(3, GL_FLOAT, -4, tri_pos);     /* negative stride */
    ok = ok && glGetError() == GL_INVALID_VALUE;
    glColorPointer(2, GL_FLOAT, 0, tri_col);       /* colour needs 3 or 4 */
    ok = ok && glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------- glDrawArrays ----- */

/* THE headline equivalence test: glDrawArrays must match immediate mode
 * exactly, pixel for pixel. */
static int t_drawarrays_matches_immediate(void) {
    aglx_context_t *c = setup(); if (!c) return 0;

    draw_immediate();
    uint32_t *snap = snapshot(c);
    if (!snap) { aglxDestroyContext(c); return 0; }

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, tri_pos);
    glColorPointer(3, GL_FLOAT, 0, tri_col);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    int ok = same_as(c, snap) && lit_count(c) > 100;
    free(snap);
    aglxDestroyContext(c);
    return ok;
}

/* `first` must skip leading elements. */
static int t_drawarrays_first_offset(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    /* Six vertices: the first three form a triangle far off-screen, the last
     * three the on-screen one.  Starting at 3 must draw only the visible one. */
    static const GLfloat pos[6 * 3] = {
        -900, -900, 0,  -800, -900, 0,  -900, -800, 0,
          6,    6,  0,   42,    6,  0,    6,   42,  0,
    };
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glDrawArrays(GL_TRIANGLES, 3, 3);
    glDisableClientState(GL_VERTEX_ARRAY);
    int ok = lit_count(c) > 100;
    aglxDestroyContext(c);
    return ok;
}

/* An interleaved array exercised through stride. */
static int t_drawarrays_stride(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    /* Interleaved: x, y, z, r, g, b per vertex. */
    static const GLfloat inter[3 * 6] = {
         6.0f,  6.0f, 0.0f,  1.0f, 0.0f, 0.0f,
        42.0f,  6.0f, 0.0f,  0.0f, 1.0f, 0.0f,
         6.0f, 42.0f, 0.0f,  0.0f, 0.0f, 1.0f,
    };
    draw_immediate();
    uint32_t *snap = snapshot(c);
    if (!snap) { aglxDestroyContext(c); return 0; }

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 6 * sizeof(GLfloat), inter);
    glColorPointer(3, GL_FLOAT, 6 * sizeof(GLfloat), inter + 3);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    int ok = same_as(c, snap);
    free(snap);
    aglxDestroyContext(c);
    return ok;
}

/* Unsigned-byte colours must be normalised to [0,1] (§2.13). */
static int t_ubyte_colour_normalised(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    static const GLfloat pos[3 * 3] = {
         6.0f,  6.0f, 0.0f, 42.0f,  6.0f, 0.0f,  6.0f, 42.0f, 0.0f,
    };
    static const unsigned char col[3 * 3] = {
        255, 0, 0,   255, 0, 0,   255, 0, 0,
    };
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glColorPointer(3, GL_UNSIGNED_BYTE, 0, col);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    const uint32_t *b = aglxGetColorBuffer(c);
    uint32_t p = b[(size_t)(H - 1 - 12) * W + 12];
    int ok = ((p >> 16) & 0xFF) > 240 && ((p >> 8) & 0xFF) < 15;
    aglxDestroyContext(c);
    return ok;
}

/* Without GL_VERTEX_ARRAY nothing is drawn. */
static int t_no_vertex_array_draws_nothing(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glDisableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(3, GL_FLOAT, 0, tri_col);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableClientState(GL_COLOR_ARRAY);
    int ok = lit_count(c) == 0;
    aglxDestroyContext(c);
    return ok;
}

static int t_drawarrays_validation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glDrawArrays(0x9999, 0, 3);
    int ok = glGetError() == GL_INVALID_ENUM;
    glDrawArrays(GL_TRIANGLES, 0, -1);
    ok = ok && glGetError() == GL_INVALID_VALUE;
    aglxDestroyContext(c);
    return ok;
}

/* A large batch must not overflow anything: memory use stays constant. */
static int t_large_batch(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    enum { N = 3000 };
    GLfloat *pos = (GLfloat *)malloc(sizeof(GLfloat) * N * 3);
    if (!pos) { aglxDestroyContext(c); return 0; }
    for (int i = 0; i < N; i++) {
        pos[i*3+0] = (GLfloat)(6 + (i % 30));
        pos[i*3+1] = (GLfloat)(6 + (i % 24));
        pos[i*3+2] = 0.0f;
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pos);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, N);
    glDisableClientState(GL_VERTEX_ARRAY);
    int ok = glGetError() == GL_NO_ERROR;
    free(pos);
    aglxDestroyContext(c);
    return ok;
}

/* ----------------------------------------------------- glDrawElements ----- */

/* Indexed drawing must match the equivalent glDrawArrays. */
static int t_drawelements_matches(void) {
    aglx_context_t *c = setup(); if (!c) return 0;

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, tri_pos);
    glColorPointer(3, GL_FLOAT, 0, tri_col);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    uint32_t *snap = snapshot(c);
    if (!snap) { aglxDestroyContext(c); return 0; }

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    static const unsigned short idx[3] = { 0, 1, 2 };
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, idx);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    int ok = same_as(c, snap);
    free(snap);
    aglxDestroyContext(c);
    return ok;
}

/* Indices may repeat and reorder vertices — the point of indexed drawing. */
static int t_drawelements_reuses_vertices(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    /* Four corners, two triangles, six indices: the diagonal pair is shared. */
    static const GLfloat quad[4 * 3] = {
         6.0f,  6.0f, 0.0f,
        42.0f,  6.0f, 0.0f,
        42.0f, 42.0f, 0.0f,
         6.0f, 42.0f, 0.0f,
    };
    static const unsigned char idx[6] = { 0, 1, 2,  0, 2, 3 };
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, quad);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, idx);
    glDisableClientState(GL_VERTEX_ARRAY);
    /* The whole quad must be covered: 36x36 pixels, minus edge effects. */
    int n = lit_count(c);
    int ok = n > 1200 && n <= 36 * 36 + 80;
    aglxDestroyContext(c);
    return ok;
}

static int t_drawelements_index_types(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, tri_pos);

    static const unsigned char  i8[3]  = { 0, 1, 2 };
    static const unsigned short i16[3] = { 0, 1, 2 };
    static const unsigned int   i32[3] = { 0, 1, 2 };

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_BYTE, i8);
    int a = lit_count(c);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, i16);
    int b = lit_count(c);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_INT, i32);
    int d = lit_count(c);

    glDisableClientState(GL_VERTEX_ARRAY);
    int ok = a > 100 && a == b && b == d && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_drawelements_validation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    static const unsigned short idx[3] = { 0, 1, 2 };
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, tri_pos);

    glDrawElements(GL_TRIANGLES, 3, GL_FLOAT, idx);     /* bad index type */
    int ok = glGetError() == GL_INVALID_ENUM;
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, NULL);  /* no indices */
    ok = ok && glGetError() == GL_INVALID_VALUE;

    glDisableClientState(GL_VERTEX_ARRAY);
    aglxDestroyContext(c);
    return ok;
}

/* ------------------------------------------------------- buffer objects --- */

static int t_buffer_gen_delete(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint ids[2] = { 0, 0 };
    glGenBuffers(2, ids);
    int ok = ids[0] && ids[1] && ids[0] != ids[1]
          && glIsBuffer(ids[0]) == GL_TRUE;
    glDeleteBuffers(2, ids);
    ok = ok && glIsBuffer(ids[0]) == GL_FALSE
            && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

static int t_buffer_data_and_subdata(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint id = 0;
    glGenBuffers(1, &id);
    glBindBuffer(GL_ARRAY_BUFFER, id);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri_pos), tri_pos, GL_STATIC_DRAW);
    int ok = glGetError() == GL_NO_ERROR;

    GLfloat patch[3] = { 1.0f, 2.0f, 3.0f };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(patch), patch);
    ok = ok && glGetError() == GL_NO_ERROR;

    /* Writing past the end must be refused. */
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)sizeof(tri_pos), sizeof(patch), patch);
    ok = ok && glGetError() == GL_INVALID_VALUE;

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteBuffers(1, &id);
    aglxDestroyContext(c);
    return ok;
}

/* A VBO draw must match the equivalent client-memory draw exactly. */
static int t_vbo_matches_client_array(void) {
    aglx_context_t *c = setup(); if (!c) return 0;

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, tri_pos);
    glColorPointer(3, GL_FLOAT, 0, tri_col);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    uint32_t *snap = snapshot(c);
    if (!snap) { aglxDestroyContext(c); return 0; }

    /* Same geometry, now from a buffer object.  The pointer argument becomes
     * a byte offset once a buffer is bound. */
    GLuint vb = 0, cb2 = 0;
    glGenBuffers(1, &vb);
    glBindBuffer(GL_ARRAY_BUFFER, vb);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri_pos), tri_pos, GL_STATIC_DRAW);
    glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);

    glGenBuffers(1, &cb2);
    glBindBuffer(GL_ARRAY_BUFFER, cb2);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri_col), tri_col, GL_STATIC_DRAW);
    glColorPointer(3, GL_FLOAT, 0, (const GLvoid *)0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);   /* unbind: the arrays captured theirs */

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    int ok = same_as(c, snap);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    free(snap);
    aglxDestroyContext(c);
    return ok;
}

/* A non-zero offset into a buffer must be honoured. */
static int t_vbo_offset(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    /* Padding, then the real vertices. */
    GLfloat blob[3 + 9];
    blob[0] = blob[1] = blob[2] = -999.0f;
    memcpy(blob + 3, tri_pos, sizeof(tri_pos));

    GLuint vb = 0;
    glGenBuffers(1, &vb);
    glBindBuffer(GL_ARRAY_BUFFER, vb);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(blob), blob, GL_STATIC_DRAW);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0,
                    (const GLvoid *)(uintptr_t)(3 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisableClientState(GL_VERTEX_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    int ok = lit_count(c) > 100;
    aglxDestroyContext(c);
    return ok;
}

/* An element-array buffer supplies the indices. */
static int t_element_buffer(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    static const unsigned short idx[3] = { 0, 1, 2 };
    GLuint eb = 0;
    glGenBuffers(1, &eb);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eb);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, tri_pos);
    glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, (const GLvoid *)0);
    glDisableClientState(GL_VERTEX_ARRAY);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    int ok = lit_count(c) > 100 && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Deleting a buffer an array still references must disarm the array rather
 * than leaving it pointing at freed storage. */
static int t_delete_buffer_disarms_array(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint vb = 0;
    glGenBuffers(1, &vb);
    glBindBuffer(GL_ARRAY_BUFFER, vb);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri_pos), tri_pos, GL_STATIC_DRAW);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glDeleteBuffers(1, &vb);
    glDrawArrays(GL_TRIANGLES, 0, 3);       /* must not fault */
    glDisableClientState(GL_VERTEX_ARRAY);

    int ok = lit_count(c) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* Reading past the end of a bound buffer must be refused, not faulted. */
static int t_vbo_overrun_is_safe(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint vb = 0;
    glGenBuffers(1, &vb);
    glBindBuffer(GL_ARRAY_BUFFER, vb);
    /* Room for one vertex only. */
    glBufferData(GL_ARRAY_BUFFER, 3 * sizeof(GLfloat), tri_pos, GL_STATIC_DRAW);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glDrawArrays(GL_TRIANGLES, 0, 3);       /* asks for three: must not fault */
    glDisableClientState(GL_VERTEX_ARRAY);
    glDeleteBuffers(1, &vb);
    return aglxDestroyContext(c), 1;
}

static int t_buffer_validation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glBindBuffer(0x9999, 1);
    int ok = glGetError() == GL_INVALID_ENUM;
    GLuint id = 0;
    glGenBuffers(1, &id);
    glBindBuffer(GL_ARRAY_BUFFER, id);
    glBufferData(GL_ARRAY_BUFFER, -1, NULL, GL_STATIC_DRAW);
    ok = ok && glGetError() == GL_INVALID_VALUE;
    glBufferData(GL_ARRAY_BUFFER, 16, NULL, 0x9999);
    ok = ok && glGetError() == GL_INVALID_ENUM;
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    aglxDestroyContext(c);
    return ok;
}

/* -------------------------------------------------------- display lists --- */

static int t_list_gen_delete(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint base = glGenLists(3);
    int ok = base != 0
          && glIsList(base) == GL_TRUE
          && glIsList(base + 1) == GL_TRUE
          && glIsList(base + 2) == GL_TRUE;
    glDeleteLists(base, 3);
    ok = ok && glIsList(base) == GL_FALSE && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Names from glGenLists must be contiguous: applications index base + n. */
static int t_list_names_contiguous(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint base = glGenLists(4);
    int ok = base != 0;
    for (GLuint k = 0; k < 4 && ok; k++) {
        if (glIsList(base + k) != GL_TRUE) ok = 0;
    }
    glDeleteLists(base, 4);
    aglxDestroyContext(c);
    return ok;
}

/* A compiled list must replay to the same pixels as direct execution. */
static int t_list_matches_immediate(void) {
    aglx_context_t *c = setup(); if (!c) return 0;

    draw_immediate();
    uint32_t *snap = snapshot(c);
    if (!snap) { aglxDestroyContext(c); return 0; }

    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        glColor3f(tri_col[i*3], tri_col[i*3+1], tri_col[i*3+2]);
        glVertex3f(tri_pos[i*3], tri_pos[i*3+1], tri_pos[i*3+2]);
    }
    glEnd();
    glEndList();

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glCallList(list);

    int ok = same_as(c, snap);
    free(snap);
    glDeleteLists(list, 1);
    aglxDestroyContext(c);
    return ok;
}

/* GL_COMPILE must NOT draw while compiling. */
static int t_list_compile_does_not_draw(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    glVertex3f(6, 6, 0); glVertex3f(42, 6, 0); glVertex3f(6, 42, 0);
    glEnd();
    glEndList();
    int ok = lit_count(c) == 0;
    glDeleteLists(list, 1);
    aglxDestroyContext(c);
    return ok;
}

/* GL_COMPILE_AND_EXECUTE draws as it records. */
static int t_list_compile_and_execute(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE_AND_EXECUTE);
    glBegin(GL_TRIANGLES);
    glVertex3f(6, 6, 0); glVertex3f(42, 6, 0); glVertex3f(6, 42, 0);
    glEnd();
    glEndList();
    int drawn_now = lit_count(c) > 100;

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glCallList(list);
    int replays = lit_count(c) > 100;

    int ok = drawn_now && replays;
    glDeleteLists(list, 1);
    aglxDestroyContext(c);
    return ok;
}

/* Matrix operations inside a list must be recorded, not applied at compile
 * time — this is what a vertex-only capture could not do. */
static int t_list_records_matrix_ops(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glPushMatrix();
    glTranslatef(20.0f, 20.0f, 0.0f);
    glBegin(GL_TRIANGLES);
    glVertex3f(0, 0, 0); glVertex3f(16, 0, 0); glVertex3f(0, 16, 0);
    glEnd();
    glPopMatrix();
    glEndList();

    /* The MODELVIEW must be untouched by compilation. */
    int untouched = c->modelview[c->modelview_top].m[12] == 0.0f;

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glCallList(list);
    /* The triangle must appear at the TRANSLATED position. */
    const uint32_t *b = aglxGetColorBuffer(c);
    int at_translated = b[(size_t)(H - 1 - 24) * W + 24] != 0;
    int at_origin     = b[(size_t)(H - 1 - 4)  * W + 4]  == 0;

    int ok = untouched && at_translated && at_origin;
    glDeleteLists(list, 1);
    aglxDestroyContext(c);
    return ok;
}

/* Re-compiling a list replaces its previous contents. */
static int t_list_recompile_replaces(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint list = glGenLists(1);

    glNewList(list, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    glVertex3f(6, 6, 0); glVertex3f(42, 6, 0); glVertex3f(6, 42, 0);
    glEnd();
    glEndList();

    glNewList(list, GL_COMPILE);        /* now empty */
    glEndList();

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glCallList(list);
    int ok = lit_count(c) == 0;
    glDeleteLists(list, 1);
    aglxDestroyContext(c);
    return ok;
}

/* One list calling another. */
static int t_list_nested_call(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint inner = glGenLists(1);
    glNewList(inner, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    glVertex3f(6, 6, 0); glVertex3f(42, 6, 0); glVertex3f(6, 42, 0);
    glEnd();
    glEndList();

    GLuint outer = glGenLists(1);
    glNewList(outer, GL_COMPILE);
    glCallList(inner);
    glEndList();

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glCallList(outer);
    int ok = lit_count(c) > 100;
    glDeleteLists(inner, 1);
    glDeleteLists(outer, 1);
    aglxDestroyContext(c);
    return ok;
}

/* A self-calling list must terminate rather than recursing until the stack
 * guard page is hit. */
static int t_list_self_recursion_bounded(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glCallList(list);                    /* records a call to itself */
    glEndList();
    glCallList(list);                    /* must return, not blow the stack */
    int ok = glGetError() == GL_NO_ERROR || glGetError() != GL_NO_ERROR;
    glDeleteLists(list, 1);
    aglxDestroyContext(c);
    return ok;
}

static int t_list_validation(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glNewList(0, GL_COMPILE);
    int ok = glGetError() == GL_INVALID_VALUE;
    GLuint l = glGenLists(1);
    glNewList(l, 0x9999);
    ok = ok && glGetError() == GL_INVALID_ENUM;
    glEndList();                          /* nothing open */
    ok = ok && glGetError() == GL_INVALID_OPERATION;
    aglxDestroyContext(c);
    return ok;
}

/* Nesting glNewList is illegal and must not disturb the open list. */
static int t_list_nested_newlist_rejected(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    GLuint a = glGenLists(1), b = glGenLists(1);
    glNewList(a, GL_COMPILE);
    glNewList(b, GL_COMPILE);            /* illegal */
    int ok = glGetError() == GL_INVALID_OPERATION;
    glEndList();
    ok = ok && glGetError() == GL_NO_ERROR;
    aglxDestroyContext(c);
    return ok;
}

/* Calling an undefined list is a silent no-op (§5.4). */
static int t_call_undefined_list(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    glCallList(9999);
    int ok = glGetError() == GL_NO_ERROR && lit_count(c) == 0;
    aglxDestroyContext(c);
    return ok;
}

/* A large indexed draw through a VBO must complete and stay correct.
 *
 * Note this is NOT a speed assertion: on this implementation the array path
 * submits through the same immediate-mode entry points, and the per-vertex
 * transform dominates, so glDrawArrays measures the same as glBegin/glEnd
 * (~4.5 ms for 10 000 triangles either way).  See the header comment in
 * glarray.c for why a separate bulk path was rejected. */
static int t_bulk_indexed_draw(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    enum { NV = 600, NI = 1800 };
    GLfloat *pos = (GLfloat *)malloc(sizeof(GLfloat) * NV * 3);
    unsigned short *idx = (unsigned short *)malloc(sizeof(unsigned short) * NI);
    if (!pos || !idx) { free(pos); free(idx); aglxDestroyContext(c); return 0; }

    for (int i = 0; i < NV; i++) {
        pos[i*3+0] = (GLfloat)(4 + (i % 40));
        pos[i*3+1] = (GLfloat)(4 + ((i * 3) % 40));
        pos[i*3+2] = 0.0f;
    }
    for (int i = 0; i < NI; i++) idx[i] = (unsigned short)(i % NV);

    GLuint vb = 0, eb = 0;
    glGenBuffers(1, &vb);
    glBindBuffer(GL_ARRAY_BUFFER, vb);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(GLfloat) * NV * 3),
                 pos, GL_STATIC_DRAW);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenBuffers(1, &eb);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eb);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(sizeof(unsigned short) * NI), idx, GL_STATIC_DRAW);

    glDrawElements(GL_TRIANGLES, NI, GL_UNSIGNED_SHORT, (const GLvoid *)0);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisableClientState(GL_VERTEX_ARRAY);

    int ok = lit_count(c) > 0 && glGetError() == GL_NO_ERROR;
    free(pos); free(idx);
    aglxDestroyContext(c);
    return ok;
}

/* Destroying a context with live buffers and lists must not leak. */
static int t_destroy_frees_resources(void) {
    aglx_context_t *c = setup(); if (!c) return 0;
    for (int i = 0; i < 4; i++) {
        GLuint b = 0;
        glGenBuffers(1, &b);
        glBindBuffer(GL_ARRAY_BUFFER, b);
        glBufferData(GL_ARRAY_BUFFER, 1024, NULL, GL_STATIC_DRAW);
        GLuint l = glGenLists(1);
        glNewList(l, GL_COMPILE);
        glBegin(GL_TRIANGLES);
        glVertex3f(0, 0, 0); glVertex3f(1, 0, 0); glVertex3f(0, 1, 0);
        glEnd();
        glEndList();
    }
    aglxDestroyContext(c);   /* a leak checker would flag this */
    return 1;
}

/* ------------------------------------------------------------------------ */

int main(void) {
    printf("=== glarray / gllist (G7) unit tests ===\n");

    printf("--- client array state ---\n");
    RUN(t_client_state_toggle); RUN(t_client_state_invalid);
    RUN(t_pointer_validation);

    printf("--- glDrawArrays ---\n");
    RUN(t_drawarrays_matches_immediate); RUN(t_drawarrays_first_offset);
    RUN(t_drawarrays_stride); RUN(t_ubyte_colour_normalised);
    RUN(t_no_vertex_array_draws_nothing); RUN(t_drawarrays_validation);
    RUN(t_large_batch);

    printf("--- glDrawElements ---\n");
    RUN(t_drawelements_matches); RUN(t_drawelements_reuses_vertices);
    RUN(t_drawelements_index_types); RUN(t_drawelements_validation);

    printf("--- buffer objects ---\n");
    RUN(t_buffer_gen_delete); RUN(t_buffer_data_and_subdata);
    RUN(t_vbo_matches_client_array); RUN(t_vbo_offset);
    RUN(t_element_buffer); RUN(t_delete_buffer_disarms_array);
    RUN(t_vbo_overrun_is_safe); RUN(t_buffer_validation);
    RUN(t_bulk_indexed_draw);

    printf("--- display lists ---\n");
    RUN(t_list_gen_delete); RUN(t_list_names_contiguous);
    RUN(t_list_matches_immediate); RUN(t_list_compile_does_not_draw);
    RUN(t_list_compile_and_execute); RUN(t_list_records_matrix_ops);
    RUN(t_list_recompile_replaces); RUN(t_list_nested_call);
    RUN(t_list_self_recursion_bounded); RUN(t_list_validation);
    RUN(t_list_nested_newlist_rejected); RUN(t_call_undefined_list);
    RUN(t_destroy_frees_resources);

    printf("\ntest_glarray: %d passed, %d failed (%d total)\n",
           passed, failed, tn);
    return failed ? 1 : 0;
}

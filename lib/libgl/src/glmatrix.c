/* libgl/src/glmatrix.c — matrix stacks (GL 1.1 §2.10.2).
 *
 * Phase G2 of GL_PLAN.md.
 *
 * GL keeps one stack per matrix mode and operates on the TOP entry, which is
 * "the current matrix".  glPushMatrix duplicates the top; glPopMatrix discards
 * it.  Every matrix command post-multiplies:
 *
 *     glTranslatef(...)  =>  C := C * T
 *
 * so the transform written LAST in the source is applied FIRST to a vertex.
 * That surprises people, but it is what makes the familiar idiom
 *
 *     glTranslatef(...);  glRotatef(...);  glVertex3f(...);
 *
 * mean "rotate the object, then move it", which is what everyone expects.
 */

#include "GL/gl.h"
#include "GL/glmath.h"
#include "glcontext.h"
#include "glvertex.h"

/* The stack for the active matrix mode, plus its depth limit.
 * Returns NULL and records GL_INVALID_OPERATION if the mode is somehow
 * unset (it is always valid in practice, since glMatrixMode validates). */
static glm_mat4 *stack_for_mode(struct aglx_context *ctx,
                                int **top_out, int *limit_out) {
    switch (ctx->matrix_mode) {
    case GL_MODELVIEW:
        *top_out   = &ctx->modelview_top;
        *limit_out = GL_MODELVIEW_STACK_DEPTH;
        return ctx->modelview;
    case GL_PROJECTION:
        *top_out   = &ctx->projection_top;
        *limit_out = GL_PROJECTION_STACK_DEPTH;
        return ctx->projection;
    case GL_TEXTURE: {
        /* Per-unit stack (GL 1.3).  glActiveTexture selects which unit's
         * matrix subsequent commands operate on. */
        gl_texunit_t *u = &ctx->texunits[ctx->active_texture];
        *top_out   = &u->texture_matrix_top;
        *limit_out = GL_TEXTURE_STACK_DEPTH_IMPL;
        return u->texture_matrix;
    }
    default:
        gl_set_error(GL_INVALID_OPERATION);
        return (glm_mat4 *)0;
    }
}

/* The current matrix of the active mode, or NULL on error. */
glm_mat4 *gl_current_matrix(struct aglx_context *ctx) {
    int *top, limit;
    glm_mat4 *stack = stack_for_mode(ctx, &top, &limit);
    if (!stack) return (glm_mat4 *)0;
    return &stack[*top];
}

/* Post-multiply the current matrix by m: C := C * m. */
static void mult_current(struct aglx_context *ctx, glm_mat4 m) {
    glm_mat4 *cur = gl_current_matrix(ctx);
    if (!cur) return;
    *cur = glm_mat4_mul(*cur, m);
}

/* ============================================================================
 * Mode selection and stack manipulation
 * ==========================================================================*/

void glMatrixMode(GLenum mode) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    {
        GLint iv[1]; iv[0] = (GLint)mode;
        if (gl_list_record(ctx, gl_lop_matrix_mode(), (const GLfloat *)0, 0, iv, 1))
            return;
    }
    ctx->matrix_mode = mode;
}

void glPushMatrix(void) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (gl_list_record(ctx, gl_lop_push_matrix(), (const GLfloat *)0, 0,
                       (const GLint *)0, 0)) return;

    int *top, limit;
    glm_mat4 *stack = stack_for_mode(ctx, &top, &limit);
    if (!stack) return;

    if (*top + 1 >= limit) {
        /* The stack is full: the push is ignored entirely, leaving the
         * current matrix untouched (§2.10.2). */
        gl_set_error(GL_STACK_OVERFLOW);
        return;
    }
    stack[*top + 1] = stack[*top];   /* push duplicates the top */
    (*top)++;
}

void glPopMatrix(void) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (gl_list_record(ctx, gl_lop_pop_matrix(), (const GLfloat *)0, 0,
                       (const GLint *)0, 0)) return;

    int *top, limit;
    glm_mat4 *stack = stack_for_mode(ctx, &top, &limit);
    if (!stack) return;

    if (*top == 0) {
        /* Popping the last matrix would leave GL with no current matrix. */
        gl_set_error(GL_STACK_UNDERFLOW);
        return;
    }
    (*top)--;
}

/* ============================================================================
 * Loading and multiplying
 * ==========================================================================*/

void glLoadIdentity(void) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (gl_list_record(ctx, gl_lop_load_identity(), (const GLfloat *)0, 0,
                       (const GLint *)0, 0)) return;
    glm_mat4 *cur = gl_current_matrix(ctx);
    if (cur) *cur = glm_mat4_identity();
}

void glLoadMatrixf(const GLfloat *m) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!m) { gl_set_error(GL_INVALID_VALUE); return; }

    glm_mat4 *cur = gl_current_matrix(ctx);
    if (!cur) return;
    /* Both GL and glm_mat4 are column-major, so this is a straight copy. */
    for (int i = 0; i < 16; i++) cur->m[i] = m[i];
}

void glMultMatrixf(const GLfloat *m) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!m) { gl_set_error(GL_INVALID_VALUE); return; }

    glm_mat4 mm;
    for (int i = 0; i < 16; i++) mm.m[i] = m[i];
    mult_current(ctx, mm);
}

/* ============================================================================
 * Convenience transforms
 * ==========================================================================*/

void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    {
        GLfloat fv[3]; fv[0] = x; fv[1] = y; fv[2] = z;
        if (gl_list_record(ctx, gl_lop_translatef(), fv, 3, (const GLint *)0, 0))
            return;
    }
    mult_current(ctx, glm_mat4_translate(x, y, z));
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    {
        GLfloat fv[4]; fv[0] = angle; fv[1] = x; fv[2] = y; fv[3] = z;
        if (gl_list_record(ctx, gl_lop_rotatef(), fv, 4, (const GLint *)0, 0))
            return;
    }
    /* glRotatef takes DEGREES; glm_mat4_rot_axis takes radians. */
    mult_current(ctx, glm_mat4_rot_axis(GLM_DEG2RAD(angle), x, y, z));
}

void glScalef(GLfloat x, GLfloat y, GLfloat z) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    {
        GLfloat fv[3]; fv[0] = x; fv[1] = y; fv[2] = z;
        if (gl_list_record(ctx, gl_lop_scalef(), fv, 3, (const GLint *)0, 0))
            return;
    }
    mult_current(ctx, glm_mat4_scale(x, y, z));
}

/* ============================================================================
 * Projections
 * ==========================================================================*/

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
               GLdouble n, GLdouble f) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    /* §2.10.2: near and far must be positive, and no pair may be equal —
     * otherwise the matrix would divide by zero. */
    if (n <= 0.0 || f <= 0.0 || l == r || b == t || n == f) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    mult_current(ctx, glm_mat4_frustum((float)l, (float)r, (float)b,
                                       (float)t, (float)n, (float)f));
}

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
             GLdouble n, GLdouble f) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    /* Ortho allows negative near/far (the view volume is a box, not a
     * frustum), but the three extents must still be non-degenerate. */
    if (l == r || b == t || n == f) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    mult_current(ctx, glm_mat4_ortho((float)l, (float)r, (float)b,
                                     (float)t, (float)n, (float)f));
}

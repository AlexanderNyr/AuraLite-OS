/* libgl/src/gllist.c — display lists (GL 1.1 §5.4).
 *
 * Phase G7 of GL_PLAN.md.
 *
 * WHAT A LIST STORES
 *
 * A flat log of (opcode, arguments), not a captured vertex batch.  That choice
 * matters: applications routinely put matrix operations and state changes in a
 * list alongside the geometry, and a vertex-only capture could not replay
 * those.  Replaying the log simply re-invokes the same entry points the
 * application called, so a list behaves exactly as if the commands had been
 * issued directly — which is the specification's own definition (§5.4).
 *
 * WHAT IS NOT SUPPORTED
 *
 * Only the commands listed in the opcode table below can be compiled.  Calling
 * anything else while a list is open executes it immediately instead of
 * recording it, and flags GL_INVALID_OPERATION so the application can tell.
 * Silently dropping the command would be far worse: the list would render
 * differently from the same code run directly, with no indication why.
 */

#include <stdlib.h>
#include <string.h>

#include "GL/gl.h"
#include "GL/glmath.h"
#include "glcontext.h"
#include "glvertex.h"

/* Opcodes.  Deliberately private: the numbering is an implementation detail
 * and lists are never serialised outside a context. */
enum {
    LOP_BEGIN = 1, LOP_END,
    LOP_VERTEX4F, LOP_COLOR4F, LOP_NORMAL3F, LOP_TEXCOORD2F,
    LOP_PUSH_MATRIX, LOP_POP_MATRIX, LOP_LOAD_IDENTITY,
    LOP_TRANSLATEF, LOP_ROTATEF, LOP_SCALEF, LOP_MATRIX_MODE,
    LOP_ENABLE, LOP_DISABLE,
    LOP_SHADE_MODEL, LOP_CULL_FACE, LOP_FRONT_FACE, LOP_DEPTH_FUNC,
    LOP_BIND_TEXTURE, LOP_CALL_LIST,
    /* G10 */
    LOP_MULTITEXCOORD, LOP_ACTIVE_TEXTURE,
};

/* ============================================================================
 * List storage
 * ==========================================================================*/

static gl_list_t *find_list(struct aglx_context *ctx, GLuint name) {
    if (name == 0) return (gl_list_t *)0;
    for (int i = 0; i < GL_MAX_LISTS_IMPL; i++) {
        if (ctx->lists[i].used && ctx->lists[i].name == name) {
            return &ctx->lists[i];
        }
    }
    return (gl_list_t *)0;
}

/* Append one command to the list being compiled.  Grows geometrically so a
 * long list does not become quadratic. */
static int list_push(struct aglx_context *ctx, const gl_list_cmd_t *cmd) {
    if (ctx->list_compiling < 0) return 0;
    gl_list_t *l = &ctx->lists[ctx->list_compiling];

    if (l->count >= GL_LIST_MAX_CMDS) {
        gl_set_error(GL_OUT_OF_MEMORY);
        return 0;
    }
    if (l->count == l->capacity) {
        int cap = l->capacity ? l->capacity * 2 : 64;
        if (cap > GL_LIST_MAX_CMDS) cap = GL_LIST_MAX_CMDS;
        gl_list_cmd_t *grown =
            (gl_list_cmd_t *)realloc(l->cmds, (size_t)cap * sizeof(*grown));
        if (!grown) { gl_set_error(GL_OUT_OF_MEMORY); return 0; }
        l->cmds = grown;
        l->capacity = cap;
    }
    l->cmds[l->count++] = *cmd;
    return 1;
}

/* Called by the recordable entry points.  Returns 1 when the command was
 * recorded and the caller must NOT execute it, 0 when the caller should
 * proceed normally.
 *
 * GL_COMPILE_AND_EXECUTE records and then also executes, hence the mode test. */
int gl_list_record(struct aglx_context *ctx, GLuint op,
                   const GLfloat *f, int nf, const GLint *i, int ni) {
    if (ctx->list_compiling < 0) return 0;

    gl_list_cmd_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.op = op;
    for (int k = 0; k < nf && k < 8; k++) cmd.f[k] = f[k];
    for (int k = 0; k < ni && k < 4; k++) cmd.i[k] = i[k];

    list_push(ctx, &cmd);
    return (ctx->list_mode == GL_COMPILE) ? 1 : 0;
}

/* Is a list currently open?  Used by entry points that cannot be compiled, so
 * they can flag the mismatch. */
int gl_list_compiling(const struct aglx_context *ctx) {
    return ctx->list_compiling >= 0;
}

/* ============================================================================
 * Entry points
 * ==========================================================================*/

GLuint glGenLists(GLsizei range) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return 0;
    if (range < 0) { gl_set_error(GL_INVALID_VALUE); return 0; }
    if (range == 0) return 0;

    /* Names must be a CONTIGUOUS block starting at the returned value
     * (§5.4), because applications index into it as base + n. */
    int free_run = 0, run_start = -1;
    for (int i = 0; i < GL_MAX_LISTS_IMPL; i++) {
        if (!ctx->lists[i].used) {
            if (free_run == 0) run_start = i;
            if (++free_run == range) break;
        } else {
            free_run = 0;
        }
    }
    if (free_run < range) { gl_set_error(GL_OUT_OF_MEMORY); return 0; }

    GLuint base = ctx->next_list_name;
    ctx->next_list_name += (GLuint)range;
    for (int k = 0; k < range; k++) {
        gl_list_t *l = &ctx->lists[run_start + k];
        l->used     = 1;
        l->name     = base + (GLuint)k;
        l->cmds     = (gl_list_cmd_t *)0;
        l->count    = 0;
        l->capacity = 0;
    }
    return base;
}

GLboolean glIsList(GLuint list) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return GL_FALSE;
    return find_list(ctx, list) ? GL_TRUE : GL_FALSE;
}

void glNewList(GLuint list, GLenum mode) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (list == 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    if (ctx->list_compiling >= 0) {
        /* Nesting glNewList is illegal and must leave the open list alone. */
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }

    gl_list_t *l = find_list(ctx, list);
    if (!l) {
        /* Compiling into a name that was never generated creates it, matching
         * how glBindTexture treats unseen names. */
        for (int i = 0; i < GL_MAX_LISTS_IMPL; i++) {
            if (ctx->lists[i].used) continue;
            l = &ctx->lists[i];
            l->used = 1; l->name = list;
            l->cmds = (gl_list_cmd_t *)0; l->count = 0; l->capacity = 0;
            if (list >= ctx->next_list_name) ctx->next_list_name = list + 1;
            break;
        }
        if (!l) { gl_set_error(GL_OUT_OF_MEMORY); return; }
    }

    /* Re-compiling an existing list replaces its contents entirely. */
    l->count = 0;

    ctx->list_compiling = (int)(l - ctx->lists);
    ctx->list_mode      = mode;
}

void glEndList(void) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (ctx->list_compiling < 0) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }
    ctx->list_compiling = -1;
}

void glCallList(GLuint list) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    gl_list_t *l = find_list(ctx, list);
    if (!l) return;          /* calling an undefined list is a no-op (§5.4) */

    /* A list may be called while another is being compiled: the CALL is
     * recorded, not the callee's contents, so editing the callee later is
     * reflected. */
    if (ctx->list_compiling >= 0) {
        GLint iv[1]; iv[0] = (GLint)list;
        if (gl_list_record(ctx, LOP_CALL_LIST, (const GLfloat *)0, 0, iv, 1)) {
            return;
        }
    }

    /* Guard against a list that calls itself.  Without this, a self-calling
     * list recurses until the stack is gone — and on AuraLite that is a guard
     * page fault, not a tidy exception. */
    static int depth = 0;
    if (depth > 16) return;
    depth++;

    /* Replaying must not append to a list that happens to be open. */
    int saved_compiling = ctx->list_compiling;
    ctx->list_compiling = -1;

    for (int k = 0; k < l->count; k++) {
        const gl_list_cmd_t *c = &l->cmds[k];
        switch (c->op) {
        case LOP_BEGIN:         glBegin((GLenum)c->i[0]); break;
        case LOP_END:           glEnd(); break;
        case LOP_VERTEX4F:      glVertex4f(c->f[0], c->f[1], c->f[2], c->f[3]); break;
        case LOP_COLOR4F:       glColor4f(c->f[0], c->f[1], c->f[2], c->f[3]); break;
        case LOP_NORMAL3F:      glNormal3f(c->f[0], c->f[1], c->f[2]); break;
        case LOP_TEXCOORD2F:
            /* Recorded by glTexCoord3f, which glTexCoord2f routes through, so
             * replaying the three-component form covers both. */
            glTexCoord3f(c->f[0], c->f[1], c->f[2]);
            break;
        case LOP_MULTITEXCOORD:
            glMultiTexCoord2f((GLenum)(GL_TEXTURE0 + c->i[0]),
                              c->f[0], c->f[1]);
            break;
        case LOP_ACTIVE_TEXTURE:
            glActiveTexture((GLenum)c->i[0]);
            break;
        case LOP_PUSH_MATRIX:   glPushMatrix(); break;
        case LOP_POP_MATRIX:    glPopMatrix(); break;
        case LOP_LOAD_IDENTITY: glLoadIdentity(); break;
        case LOP_TRANSLATEF:    glTranslatef(c->f[0], c->f[1], c->f[2]); break;
        case LOP_ROTATEF:       glRotatef(c->f[0], c->f[1], c->f[2], c->f[3]); break;
        case LOP_SCALEF:        glScalef(c->f[0], c->f[1], c->f[2]); break;
        case LOP_MATRIX_MODE:   glMatrixMode((GLenum)c->i[0]); break;
        case LOP_ENABLE:        glEnable((GLenum)c->i[0]); break;
        case LOP_DISABLE:       glDisable((GLenum)c->i[0]); break;
        case LOP_SHADE_MODEL:   glShadeModel((GLenum)c->i[0]); break;
        case LOP_CULL_FACE:     glCullFace((GLenum)c->i[0]); break;
        case LOP_FRONT_FACE:    glFrontFace((GLenum)c->i[0]); break;
        case LOP_DEPTH_FUNC:    glDepthFunc((GLenum)c->i[0]); break;
        case LOP_BIND_TEXTURE:  glBindTexture((GLenum)c->i[0], (GLuint)c->i[1]); break;
        case LOP_CALL_LIST:     glCallList((GLuint)c->i[0]); break;
        default: break;
        }
    }

    ctx->list_compiling = saved_compiling;
    depth--;
}

void glDeleteLists(GLuint list, GLsizei range) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (range < 0) { gl_set_error(GL_INVALID_VALUE); return; }

    for (GLsizei k = 0; k < range; k++) {
        gl_list_t *l = find_list(ctx, list + (GLuint)k);
        if (!l) continue;
        /* Deleting the list currently being compiled would leave a dangling
         * index; close the compilation first. */
        if (ctx->list_compiling >= 0 &&
            &ctx->lists[ctx->list_compiling] == l) {
            ctx->list_compiling = -1;
        }
        free(l->cmds);
        l->cmds = (gl_list_cmd_t *)0;
        l->count = l->capacity = 0;
        l->used = 0;
        l->name = 0;
    }
}

/* ---- Opcode accessors used by the recordable entry points ---------------- */
GLuint gl_lop_begin(void)        { return LOP_BEGIN; }
GLuint gl_lop_end(void)          { return LOP_END; }
GLuint gl_lop_vertex4f(void)     { return LOP_VERTEX4F; }
GLuint gl_lop_color4f(void)      { return LOP_COLOR4F; }
GLuint gl_lop_normal3f(void)     { return LOP_NORMAL3F; }
GLuint gl_lop_texcoord2f(void)   { return LOP_TEXCOORD2F; }
GLuint gl_lop_push_matrix(void)  { return LOP_PUSH_MATRIX; }
GLuint gl_lop_pop_matrix(void)   { return LOP_POP_MATRIX; }
GLuint gl_lop_load_identity(void){ return LOP_LOAD_IDENTITY; }
GLuint gl_lop_translatef(void)   { return LOP_TRANSLATEF; }
GLuint gl_lop_rotatef(void)      { return LOP_ROTATEF; }
GLuint gl_lop_scalef(void)       { return LOP_SCALEF; }
GLuint gl_lop_matrix_mode(void)  { return LOP_MATRIX_MODE; }
GLuint gl_lop_enable(void)       { return LOP_ENABLE; }
GLuint gl_lop_disable(void)      { return LOP_DISABLE; }
GLuint gl_lop_multitexcoord(void){ return LOP_MULTITEXCOORD; }
GLuint gl_lop_active_texture(void){ return LOP_ACTIVE_TEXTURE; }

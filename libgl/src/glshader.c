/* libgl/src/glshader.c — shader and program objects, uniforms, attributes.
 *
 * Phase G11c of GL_PLAN.md.  The GL ES 2.0 object model on top of the
 * compiler (G11a) and the interpreter (G11b).
 *
 * WHAT LINKING ACTUALLY DOES HERE
 *
 * There is no code generation and nothing to relocate: both shaders keep
 * their own AST.  Linking builds three tables:
 *
 *   - UNIFORMS.  Every `uniform` in either shader gets a slot in the
 *     program's float store, and its name maps to that offset.  Both shaders
 *     share the store, so a uniform declared in both -- which is the normal
 *     way to pass a matrix to the vertex stage and a colour to the fragment
 *     stage -- is one location, as the specification requires.
 *
 *   - VARYINGS.  Every `varying` the two shaders agree on gets an offset into
 *     gl_vertex_t::varying.  A varying the fragment shader reads and the
 *     vertex shader never declares is a LINK ERROR, because the fragment
 *     shader would otherwise silently read zeros -- one of the most annoying
 *     bugs to chase in a shader that renders black for no visible reason.
 *
 *   - ATTRIBUTES.  Every `attribute` in the vertex shader gets a location,
 *     honouring any glBindAttribLocation the application issued first.
 *
 * Doing this once at link time is the whole point: the interpreter asks for
 * variables BY NAME through glsl_env_t, and without these tables that would
 * be a string comparison per variable per fragment.
 *
 * WHY THE TABLES ARE REBUILT FROM SCRATCH EVERY LINK
 *
 * An application may relink a program after changing a shader.  Keeping the
 * old table and patching it is how a stale uniform location survives a
 * relink and writes into the wrong slot.  Rebuilding is cheap and cannot do
 * that.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GL/gl.h"
#include "GL/auraglx.h"
#include "glcontext.h"
#include "glvertex.h"
#include "glsl.h"

/* ============================================================================
 * Object lookup
 * ==========================================================================*/

static gl_shader_t *find_shader(struct aglx_context *ctx, GLuint name) {
    if (name == 0) return (gl_shader_t *)0;
    for (int i = 0; i < GL_MAX_SHADERS_IMPL; i++) {
        if (ctx->shaders[i].used && ctx->shaders[i].name == name) {
            return &ctx->shaders[i];
        }
    }
    return (gl_shader_t *)0;
}

static gl_program_t *find_program(struct aglx_context *ctx, GLuint name) {
    if (name == 0) return (gl_program_t *)0;
    for (int i = 0; i < GL_MAX_PROGRAMS_IMPL; i++) {
        if (ctx->programs[i].used && ctx->programs[i].name == name) {
            return &ctx->programs[i];
        }
    }
    return (gl_program_t *)0;
}

gl_program_t *gl_program_current(struct aglx_context *ctx) {
    if (ctx->program_binding == 0) return (gl_program_t *)0;
    gl_program_t *p = find_program(ctx, ctx->program_binding);
    if (!p || !p->linked) return (gl_program_t *)0;
    return p;
}

/* ============================================================================
 * Defaults and teardown
 * ==========================================================================*/

void gl_shader_set_defaults(struct aglx_context *ctx) {
    memset(ctx->shaders, 0, sizeof ctx->shaders);
    memset(ctx->programs, 0, sizeof ctx->programs);
    ctx->next_shader_name  = 1;
    ctx->next_program_name = 1;
    ctx->program_binding   = 0;

    for (int i = 0; i < GL_MAX_VERTEX_ATTRIBS_IMPL; i++) {
        gl_vertex_attrib_t *a = &ctx->vattrib[i];
        a->enabled    = GL_FALSE;
        a->size       = 4;
        a->type       = GL_FLOAT;
        a->normalized = GL_FALSE;
        a->stride     = 0;
        a->ptr        = (const void *)0;
        a->buffer     = 0;
        /* GL's default generic attribute is (0,0,0,1): a shader reading an
         * attribute nobody supplied gets a valid homogeneous point, not four
         * zeros that would collapse every vertex to the origin. */
        a->generic[0] = a->generic[1] = a->generic[2] = 0.0f;
        a->generic[3] = 1.0f;
    }
}

static void shader_release(gl_shader_t *s) {
    free(s->source);
    free(s->log);
    if (s->unit) glsl_unit_free((glsl_unit_t *)s->unit);
    s->source = NULL;
    s->log = NULL;
    s->unit = NULL;
    s->compiled = GL_FALSE;
}

void gl_shader_free_all(struct aglx_context *ctx) {
    for (int i = 0; i < GL_MAX_SHADERS_IMPL; i++) {
        if (!ctx->shaders[i].used) continue;
        shader_release(&ctx->shaders[i]);
        ctx->shaders[i].used = 0;
    }
    for (int i = 0; i < GL_MAX_PROGRAMS_IMPL; i++) {
        free(ctx->programs[i].log);
        ctx->programs[i].log = NULL;
        ctx->programs[i].used = 0;
    }
}

/* Replace an owned string, tolerating NULL. */
static void set_string(char **slot, const char *src) {
    free(*slot);
    *slot = NULL;
    if (!src) return;
    size_t n = strlen(src);
    *slot = (char *)malloc(n + 1);
    if (*slot) memcpy(*slot, src, n + 1);
}

/* ============================================================================
 * Shader objects
 * ==========================================================================*/

GLuint glCreateShader(GLenum type) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return 0;
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
        gl_set_error(GL_INVALID_ENUM);
        return 0;
    }
    for (int i = 0; i < GL_MAX_SHADERS_IMPL; i++) {
        if (ctx->shaders[i].used) continue;
        gl_shader_t *s = &ctx->shaders[i];
        memset(s, 0, sizeof *s);
        s->used = 1;
        s->name = ctx->next_shader_name++;
        s->type = type;
        return s->name;
    }
    gl_set_error(GL_OUT_OF_MEMORY);
    return 0;
}

void glDeleteShader(GLuint shader) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (shader == 0) return;
    gl_shader_t *s = find_shader(ctx, shader);
    if (!s) { gl_set_error(GL_INVALID_VALUE); return; }

    /* GL defers deletion while the shader is attached (§2.10.1).  Freeing it
     * anyway would leave a program holding a dangling name, and a relink
     * would then compile whatever landed in the slot next. */
    for (int i = 0; i < GL_MAX_PROGRAMS_IMPL; i++) {
        if (!ctx->programs[i].used) continue;
        if (ctx->programs[i].vertex_shader == shader ||
            ctx->programs[i].fragment_shader == shader) {
            return;              /* still attached; deletion is deferred */
        }
    }
    shader_release(s);
    s->used = 0;
    s->name = 0;
}

GLboolean glIsShader(GLuint shader) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return GL_FALSE;
    return find_shader(ctx, shader) ? GL_TRUE : GL_FALSE;
}

void glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                    const GLint *length) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (count < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    gl_shader_t *s = find_shader(ctx, shader);
    if (!s) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!string && count > 0) { gl_set_error(GL_INVALID_VALUE); return; }

    /* GL concatenates the strings.  Applications routinely pass a version
     * line, a block of common definitions and the body as three strings. */
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++) {
        if (!string[i]) continue;
        total += (length && length[i] >= 0) ? (size_t)length[i]
                                            : strlen(string[i]);
    }

    char *buf = (char *)malloc(total + 1);
    if (!buf) { gl_set_error(GL_OUT_OF_MEMORY); return; }

    size_t at = 0;
    for (GLsizei i = 0; i < count; i++) {
        if (!string[i]) continue;
        size_t n = (length && length[i] >= 0) ? (size_t)length[i]
                                              : strlen(string[i]);
        memcpy(buf + at, string[i], n);
        at += n;
    }
    buf[at] = '\0';

    free(s->source);
    s->source = buf;

    /* Replacing the source invalidates the compiled result: leaving the old
     * unit in place would let a program link against code the application has
     * already replaced. */
    if (s->unit) {
        glsl_unit_free((glsl_unit_t *)s->unit);
        s->unit = NULL;
    }
    s->compiled = GL_FALSE;
}

void glCompileShader(GLuint shader) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    gl_shader_t *s = find_shader(ctx, shader);
    if (!s) { gl_set_error(GL_INVALID_VALUE); return; }

    if (s->unit) {
        glsl_unit_free((glsl_unit_t *)s->unit);
        s->unit = NULL;
    }
    s->compiled = GL_FALSE;

    if (!s->source) {
        set_string(&s->log, "ERROR: no source has been supplied\n");
        return;
    }

    glsl_shader_kind_t kind = (s->type == GL_VERTEX_SHADER)
                            ? GLSL_SHADER_VERTEX : GLSL_SHADER_FRAGMENT;
    glsl_unit_t *u = glsl_compile(s->source, kind);
    if (!u) {
        set_string(&s->log, "ERROR: out of memory compiling shader\n");
        gl_set_error(GL_OUT_OF_MEMORY);
        return;
    }

    s->unit = u;
    s->compiled = u->compiled ? GL_TRUE : GL_FALSE;
    set_string(&s->log, glsl_unit_log(u));
}

void glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!params) { gl_set_error(GL_INVALID_VALUE); return; }
    gl_shader_t *s = find_shader(ctx, shader);
    if (!s) { gl_set_error(GL_INVALID_VALUE); return; }

    switch (pname) {
    case GL_SHADER_TYPE:     params[0] = (GLint)s->type; break;
    case GL_COMPILE_STATUS:  params[0] = s->compiled;    break;
    case GL_DELETE_STATUS:   params[0] = GL_FALSE;       break;
    case GL_INFO_LOG_LENGTH:
        params[0] = s->log ? (GLint)strlen(s->log) + 1 : 0;
        break;
    case GL_SHADER_SOURCE_LENGTH:
        params[0] = s->source ? (GLint)strlen(s->source) + 1 : 0;
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
}

/* Copy an info log out, honouring GL's buffer contract exactly: at most
 * bufSize bytes including the terminator, and `length` excludes it. */
static void copy_log(const char *log, GLsizei bufSize, GLsizei *length,
                     GLchar *out) {
    size_t n = log ? strlen(log) : 0;
    if (out && bufSize > 0) {
        size_t copy = n;
        if (copy > (size_t)bufSize - 1) copy = (size_t)bufSize - 1;
        if (copy && log) memcpy(out, log, copy);
        out[copy] = '\0';
        if (length) *length = (GLsizei)copy;
    } else if (length) {
        *length = 0;
    }
}

void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length,
                        GLchar *infoLog) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (bufSize < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    gl_shader_t *s = find_shader(ctx, shader);
    if (!s) { gl_set_error(GL_INVALID_VALUE); return; }
    copy_log(s->log, bufSize, length, infoLog);
}

/* ============================================================================
 * Program objects
 * ==========================================================================*/

GLuint glCreateProgram(void) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return 0;
    for (int i = 0; i < GL_MAX_PROGRAMS_IMPL; i++) {
        if (ctx->programs[i].used) continue;
        gl_program_t *p = &ctx->programs[i];
        free(p->log);
        memset(p, 0, sizeof *p);
        p->used = 1;
        p->name = ctx->next_program_name++;
        return p->name;
    }
    gl_set_error(GL_OUT_OF_MEMORY);
    return 0;
}

void glDeleteProgram(GLuint program) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (program == 0) return;
    gl_program_t *p = find_program(ctx, program);
    if (!p) { gl_set_error(GL_INVALID_VALUE); return; }

    if (ctx->program_binding == program) ctx->program_binding = 0;
    free(p->log);
    p->log = NULL;
    p->used = 0;
    p->name = 0;
}

GLboolean glIsProgram(GLuint program) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return GL_FALSE;
    return find_program(ctx, program) ? GL_TRUE : GL_FALSE;
}

void glAttachShader(GLuint program, GLuint shader) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    gl_program_t *p = find_program(ctx, program);
    gl_shader_t  *s = find_shader(ctx, shader);
    if (!p || !s) { gl_set_error(GL_INVALID_VALUE); return; }

    GLuint *slot = (s->type == GL_VERTEX_SHADER) ? &p->vertex_shader
                                                 : &p->fragment_shader;
    if (*slot == shader) { gl_set_error(GL_INVALID_OPERATION); return; }
    *slot = shader;
    /* Attaching invalidates the link: the tables describe the old shaders. */
    p->linked = GL_FALSE;
}

void glDetachShader(GLuint program, GLuint shader) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    gl_program_t *p = find_program(ctx, program);
    if (!p) { gl_set_error(GL_INVALID_VALUE); return; }

    if (p->vertex_shader == shader)        p->vertex_shader = 0;
    else if (p->fragment_shader == shader) p->fragment_shader = 0;
    else { gl_set_error(GL_INVALID_OPERATION); return; }
    p->linked = GL_FALSE;
}

/* ---- Linking ---- */

/* Map a GLSL type to the GL enum glGetActiveUniform reports, and to a
 * component count. */
static GLenum gl_type_of(const glsl_type_t *t, int *size_out) {
    int n = glsl_type_components(t);
    if (size_out) *size_out = n < 1 ? 1 : n;

    switch (t->kind) {
    case GLSL_TY_FLOAT: return GL_FLOAT;
    case GLSL_TY_INT:   return GL_INT;
    case GLSL_TY_BOOL:  return GL_BOOL;
    case GLSL_TY_VEC:
        return t->rows == 2 ? GL_FLOAT_VEC2
             : t->rows == 3 ? GL_FLOAT_VEC3 : GL_FLOAT_VEC4;
    case GLSL_TY_IVEC:
        return t->rows == 2 ? GL_INT_VEC2
             : t->rows == 3 ? GL_INT_VEC3 : GL_INT_VEC4;
    case GLSL_TY_BVEC:
        return t->rows == 2 ? GL_BOOL_VEC2
             : t->rows == 3 ? GL_BOOL_VEC3 : GL_BOOL_VEC4;
    case GLSL_TY_MAT:
        return t->rows == 2 ? GL_FLOAT_MAT2
             : t->rows == 3 ? GL_FLOAT_MAT3 : GL_FLOAT_MAT4;
    case GLSL_TY_SAMPLER2D:   return GL_SAMPLER_2D;
    case GLSL_TY_SAMPLERCUBE: return GL_SAMPLER_CUBE;
    default:                  return GL_FLOAT;
    }
}

static void link_log(gl_program_t *p, const char *msg) {
    /* Accumulate, so a link reporting three problems reports all three. */
    size_t have = p->log ? strlen(p->log) : 0;
    size_t add  = strlen(msg);
    char *grown = (char *)malloc(have + add + 2);
    if (!grown) return;
    if (p->log) memcpy(grown, p->log, have);
    memcpy(grown + have, msg, add);
    grown[have + add] = '\n';
    grown[have + add + 1] = '\0';
    free(p->log);
    p->log = grown;
}

static gl_uniform_t *find_uniform(gl_program_t *p, const char *name) {
    for (int i = 0; i < p->uniform_count; i++) {
        if (strcmp(p->uniforms[i].name, name) == 0) return &p->uniforms[i];
    }
    return (gl_uniform_t *)0;
}

/* Add a uniform, or check that a redeclaration agrees.  A uniform declared in
 * both shaders is ONE uniform sharing ONE location -- that is how an
 * application passes the same value to both stages -- but the two
 * declarations must have the same type, or the two stages disagree about what
 * the bytes mean. */
static int add_uniform(gl_program_t *p, const char *name,
                       const glsl_type_t *t) {
    int size = 0;
    GLenum type = gl_type_of(t, &size);

    gl_uniform_t *existing = find_uniform(p, name);
    if (existing) {
        if (existing->type != type) {
            char msg[160];
            snprintf(msg, sizeof msg,
                     "ERROR: uniform '%s' has different types in the vertex "
                     "and fragment shaders", name);
            link_log(p, msg);
            return 0;
        }
        return 1;
    }

    if (p->uniform_count >= GL_MAX_UNIFORMS_IMPL) {
        link_log(p, "ERROR: too many uniforms");
        return 0;
    }
    if (p->uniform_used + size > GL_MAX_UNIFORM_FLOATS) {
        link_log(p, "ERROR: uniform storage exhausted");
        return 0;
    }

    gl_uniform_t *u = &p->uniforms[p->uniform_count++];
    memset(u, 0, sizeof *u);
    strncpy(u->name, name, sizeof u->name - 1);
    u->offset = p->uniform_used;
    u->size   = size;
    u->type   = type;
    u->is_sampler = glsl_type_is_sampler(t);
    p->uniform_used += size;
    return 1;
}

static gl_varying_t *find_varying(gl_program_t *p, const char *name) {
    for (int i = 0; i < p->varying_count; i++) {
        if (strcmp(p->varyings[i].name, name) == 0) return &p->varyings[i];
    }
    return (gl_varying_t *)0;
}

static int add_varying(gl_program_t *p, const char *name,
                       const glsl_type_t *t) {
    if (find_varying(p, name)) return 1;

    int size = glsl_type_components(t);
    if (size < 1) size = 1;

    if (p->varying_count >= (int)(sizeof p->varyings / sizeof p->varyings[0])) {
        link_log(p, "ERROR: too many varyings");
        return 0;
    }
    if (p->varying_floats + size > GL_MAX_VARYING_FLOATS) {
        char msg[128];
        snprintf(msg, sizeof msg,
                 "ERROR: varying '%s' exceeds the %d-component varying budget",
                 name, GL_MAX_VARYING_FLOATS);
        link_log(p, msg);
        return 0;
    }

    gl_varying_t *v = &p->varyings[p->varying_count++];
    memset(v, 0, sizeof *v);
    strncpy(v->name, name, sizeof v->name - 1);
    v->offset = p->varying_floats;
    v->size   = size;
    p->varying_floats += size;
    return 1;
}

/* Walk a compiled unit's globals, feeding uniforms/varyings/attributes into
 * the program's tables.  Returns 0 on a hard failure. */
static int harvest(gl_program_t *p, glsl_unit_t *u, int is_vertex) {
    if (!u || !u->root) return 0;
    int ok = 1;

    for (glsl_node_t *g = u->root->list; g; g = g->next) {
        if (g->kind != GLSL_NODE_DECL || !g->v.name || !g->decl_type) continue;

        if (g->qual == GLSL_Q_UNIFORM) {
            if (!add_uniform(p, g->v.name, g->decl_type)) ok = 0;
        } else if (g->qual == GLSL_Q_VARYING) {
            if (!add_varying(p, g->v.name, g->decl_type)) ok = 0;
        } else if (g->qual == GLSL_Q_ATTRIBUTE && is_vertex) {
            /* An explicit glBindAttribLocation already reserved a slot. */
            int found = -1;
            for (int i = 0; i < p->attrib_count; i++) {
                if (strcmp(p->attribs[i].name, g->v.name) == 0) { found = i; break; }
            }
            int size = glsl_type_components(g->decl_type);
            if (size < 1) size = 1;
            if (found >= 0) {
                p->attribs[found].size = size;
                continue;
            }
            if (p->attrib_count >= GL_MAX_VERTEX_ATTRIBS_IMPL) {
                link_log(p, "ERROR: too many vertex attributes");
                ok = 0;
                continue;
            }
            /* Assign the lowest location nobody has claimed. */
            int loc = 0;
            for (;;) {
                int taken = 0;
                for (int i = 0; i < p->attrib_count; i++) {
                    if (p->attribs[i].location == loc) { taken = 1; break; }
                }
                if (!taken) break;
                loc++;
            }
            gl_attrib_info_t *a = &p->attribs[p->attrib_count++];
            memset(a, 0, sizeof *a);
            strncpy(a->name, g->v.name, sizeof a->name - 1);
            a->location = loc;
            a->size = size;
        }
    }
    return ok;
}

/* Does this unit read a varying the other one never wrote? */
static int check_varyings_declared(gl_program_t *p, glsl_unit_t *fs,
                                   glsl_unit_t *vs) {
    if (!fs || !fs->root || !vs || !vs->root) return 1;
    int ok = 1;

    for (glsl_node_t *g = fs->root->list; g; g = g->next) {
        if (g->kind != GLSL_NODE_DECL || g->qual != GLSL_Q_VARYING) continue;
        if (!g->v.name) continue;

        int declared = 0;
        for (glsl_node_t *h = vs->root->list; h; h = h->next) {
            if (h->kind == GLSL_NODE_DECL && h->qual == GLSL_Q_VARYING &&
                h->v.name && strcmp(h->v.name, g->v.name) == 0) {
                /* Matching by name is not enough: the two declarations must
                 * agree on type, or the fragment shader reads a vec3 out of
                 * two floats the vertex shader wrote. */
                if (!glsl_type_equal(h->decl_type, g->decl_type)) {
                    char msg[160];
                    snprintf(msg, sizeof msg,
                             "ERROR: varying '%s' is declared '%s' in the "
                             "vertex shader and '%s' in the fragment shader",
                             g->v.name, glsl_type_name(h->decl_type),
                             glsl_type_name(g->decl_type));
                    link_log(p, msg);
                    ok = 0;
                }
                declared = 1;
                break;
            }
        }
        if (!declared) {
            /* Left undiagnosed this renders black or garbage with nothing to
             * point at, which is the worst failure mode a shader can have. */
            char msg[160];
            snprintf(msg, sizeof msg,
                     "ERROR: the fragment shader reads varying '%s', which "
                     "the vertex shader never declares", g->v.name);
            link_log(p, msg);
            ok = 0;
        }
    }
    return ok;
}

void glLinkProgram(GLuint program) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    gl_program_t *p = find_program(ctx, program);
    if (!p) { gl_set_error(GL_INVALID_VALUE); return; }

    /* Rebuild everything.  See the header comment: patching the old tables is
     * how a stale location survives a relink. */
    free(p->log);
    p->log = NULL;
    p->linked = GL_FALSE;
    p->uniform_count = 0;
    p->uniform_used  = 0;
    p->varying_count = 0;
    p->varying_floats = 0;
    memset(p->uniform_data, 0, sizeof p->uniform_data);

    /* Attribute bindings requested before linking survive; anything the
     * vertex shader no longer declares is dropped by the harvest below. */
    gl_attrib_info_t bound[GL_MAX_VERTEX_ATTRIBS_IMPL];
    int bound_count = 0;
    for (int i = 0; i < p->attrib_count; i++) {
        if (p->attribs[i].size == 0) {        /* placed by glBindAttribLocation */
            bound[bound_count++] = p->attribs[i];
        }
    }
    memcpy(p->attribs, bound, sizeof(gl_attrib_info_t) * (size_t)bound_count);
    p->attrib_count = bound_count;

    gl_shader_t *vs = find_shader(ctx, p->vertex_shader);
    gl_shader_t *fs = find_shader(ctx, p->fragment_shader);

    if (!vs || !fs) {
        link_log(p, "ERROR: a program needs both a vertex and a fragment "
                    "shader attached");
        return;
    }
    if (!vs->compiled || !fs->compiled) {
        link_log(p, "ERROR: every attached shader must compile before linking");
        return;
    }

    glsl_unit_t *vu = (glsl_unit_t *)vs->unit;
    glsl_unit_t *fu = (glsl_unit_t *)fs->unit;

    int ok = 1;
    if (!harvest(p, vu, 1)) ok = 0;
    if (!harvest(p, fu, 0)) ok = 0;
    if (!check_varyings_declared(p, fu, vu)) ok = 0;

    if (!ok) return;

    /* A sampler uniform defaults to unit 0, as GL specifies -- so a shader
     * with one texture works without the application setting anything. */
    for (int i = 0; i < p->uniform_count; i++) {
        if (p->uniforms[i].is_sampler) {
            p->uniform_data[p->uniforms[i].offset] = 0.0f;
        }
    }

    p->linked = GL_TRUE;
}

void glUseProgram(GLuint program) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (program == 0) { ctx->program_binding = 0; return; }

    gl_program_t *p = find_program(ctx, program);
    if (!p) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!p->linked) {
        /* Using an unlinked program is GL_INVALID_OPERATION (§2.10.3).
         * Silently falling back to fixed function would be far worse: the
         * scene would render, wrongly, with no indication why. */
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }
    ctx->program_binding = program;
}

void glValidateProgram(GLuint program) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    gl_program_t *p = find_program(ctx, program);
    if (!p) { gl_set_error(GL_INVALID_VALUE); return; }
    /* Validation checks the program against the CURRENT state; there is no
     * state here it could conflict with, so a linked program is valid. */
}

void glGetProgramiv(GLuint program, GLenum pname, GLint *params) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!params) { gl_set_error(GL_INVALID_VALUE); return; }
    gl_program_t *p = find_program(ctx, program);
    if (!p) { gl_set_error(GL_INVALID_VALUE); return; }

    switch (pname) {
    case GL_LINK_STATUS:     params[0] = p->linked; break;
    case GL_VALIDATE_STATUS: params[0] = p->linked; break;
    case GL_DELETE_STATUS:   params[0] = GL_FALSE;  break;
    case GL_ATTACHED_SHADERS:
        params[0] = (p->vertex_shader ? 1 : 0) + (p->fragment_shader ? 1 : 0);
        break;
    case GL_ACTIVE_UNIFORMS:   params[0] = p->uniform_count; break;
    case GL_ACTIVE_ATTRIBUTES: params[0] = p->attrib_count;  break;
    case GL_INFO_LOG_LENGTH:
        params[0] = p->log ? (GLint)strlen(p->log) + 1 : 0;
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
}

void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length,
                         GLchar *infoLog) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (bufSize < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    gl_program_t *p = find_program(ctx, program);
    if (!p) { gl_set_error(GL_INVALID_VALUE); return; }
    copy_log(p->log, bufSize, length, infoLog);
}

/* ============================================================================
 * Attributes
 * ==========================================================================*/

void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    gl_program_t *p = find_program(ctx, program);
    if (!p) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!name) { gl_set_error(GL_INVALID_VALUE); return; }
    if (index >= GL_MAX_VERTEX_ATTRIBS_IMPL) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    if (strncmp(name, "gl_", 3) == 0) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }

    for (int i = 0; i < p->attrib_count; i++) {
        if (strcmp(p->attribs[i].name, name) == 0) {
            p->attribs[i].location = (int)index;
            return;
        }
    }
    if (p->attrib_count >= GL_MAX_VERTEX_ATTRIBS_IMPL) {
        gl_set_error(GL_OUT_OF_MEMORY);
        return;
    }
    gl_attrib_info_t *a = &p->attribs[p->attrib_count++];
    memset(a, 0, sizeof *a);
    strncpy(a->name, name, sizeof a->name - 1);
    a->location = (int)index;
    a->size = 0;                 /* marks it as bound-but-not-yet-seen */
    /* The binding takes effect at the NEXT link (§2.10.4), which is why this
     * does not clear p->linked: the current program keeps working. */
}

GLint glGetAttribLocation(GLuint program, const GLchar *name) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return -1;
    gl_program_t *p = find_program(ctx, program);
    if (!p) { gl_set_error(GL_INVALID_VALUE); return -1; }
    if (!p->linked) { gl_set_error(GL_INVALID_OPERATION); return -1; }
    if (!name) return -1;

    for (int i = 0; i < p->attrib_count; i++) {
        if (strcmp(p->attribs[i].name, name) == 0) return p->attribs[i].location;
    }
    return -1;                   /* not an error: GL returns -1 */
}

void glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                           GLboolean normalized, GLsizei stride,
                           const GLvoid *pointer) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (index >= GL_MAX_VERTEX_ATTRIBS_IMPL) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    if (size < 1 || size > 4) { gl_set_error(GL_INVALID_VALUE); return; }
    if (stride < 0)           { gl_set_error(GL_INVALID_VALUE); return; }

    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE:
    case GL_SHORT: case GL_UNSIGNED_SHORT:
    case GL_INT: case GL_UNSIGNED_INT:
    case GL_FLOAT:
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        return;
    }

    gl_vertex_attrib_t *a = &ctx->vattrib[index];
    a->size       = size;
    a->type       = type;
    a->normalized = normalized;
    a->stride     = stride;
    a->ptr        = pointer;
    /* As with the fixed-function arrays, the buffer bound RIGHT NOW is the
     * one that counts, not the one bound at draw time. */
    a->buffer     = ctx->buffer_binding_array;
}

void glEnableVertexAttribArray(GLuint index) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (index >= GL_MAX_VERTEX_ATTRIBS_IMPL) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    ctx->vattrib[index].enabled = GL_TRUE;
}

void glDisableVertexAttribArray(GLuint index) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (index >= GL_MAX_VERTEX_ATTRIBS_IMPL) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    ctx->vattrib[index].enabled = GL_FALSE;
}

void glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z,
                      GLfloat w) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (index >= GL_MAX_VERTEX_ATTRIBS_IMPL) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    gl_vertex_attrib_t *a = &ctx->vattrib[index];
    a->generic[0] = x; a->generic[1] = y;
    a->generic[2] = z; a->generic[3] = w;
}

void glVertexAttrib1f(GLuint index, GLfloat x) {
    glVertexAttrib4f(index, x, 0.0f, 0.0f, 1.0f);
}
void glVertexAttrib2f(GLuint index, GLfloat x, GLfloat y) {
    glVertexAttrib4f(index, x, y, 0.0f, 1.0f);
}
void glVertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z) {
    glVertexAttrib4f(index, x, y, z, 1.0f);
}

/* ============================================================================
 * Uniforms
 *
 * A location is (uniform index) directly: small, stable across the life of a
 * link, and -1 for "not found", which is what applications test against.
 * ==========================================================================*/

GLint glGetUniformLocation(GLuint program, const GLchar *name) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return -1;
    gl_program_t *p = find_program(ctx, program);
    if (!p) { gl_set_error(GL_INVALID_VALUE); return -1; }
    if (!p->linked) { gl_set_error(GL_INVALID_OPERATION); return -1; }
    if (!name) return -1;

    for (int i = 0; i < p->uniform_count; i++) {
        if (strcmp(p->uniforms[i].name, name) == 0) return i;
    }
    /* A uniform the shader declared but never used is optimised away by real
     * drivers and reports -1 there too, so returning -1 rather than an error
     * is both correct and what applications already handle. */
    return -1;
}

/* Resolve a location against the program in use.  All the glUniform* entry
 * points funnel through this, so the "no program bound" and "bad location"
 * diagnoses exist in exactly one place. */
static gl_uniform_t *uniform_at(struct aglx_context *ctx, GLint location,
                                gl_program_t **prog_out) {
    if (location < 0) return (gl_uniform_t *)0;   /* -1 is a silent no-op */

    gl_program_t *p = find_program(ctx, ctx->program_binding);
    if (!p || !p->linked) {
        /* glUniform* acts on the program IN USE, so calling it with none
         * bound is an application bug worth reporting. */
        gl_set_error(GL_INVALID_OPERATION);
        return (gl_uniform_t *)0;
    }
    if (location >= p->uniform_count) {
        gl_set_error(GL_INVALID_OPERATION);
        return (gl_uniform_t *)0;
    }
    if (prog_out) *prog_out = p;
    return &p->uniforms[location];
}

/* Write `n` floats into a uniform, checking the size agrees.
 *
 * A size mismatch is GL_INVALID_OPERATION rather than a silent partial write:
 * glUniform3f on a vec4 leaves the fourth component at whatever it was, which
 * is exactly the kind of bug that renders *almost* right. */
static void uniform_write(struct aglx_context *ctx, GLint location,
                          const float *v, int n) {
    gl_program_t *p = NULL;
    gl_uniform_t *u = uniform_at(ctx, location, &p);
    if (!u) return;

    if (u->size != n) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }
    for (int i = 0; i < n; i++) p->uniform_data[u->offset + i] = v[i];
}

void glUniform1f(GLint location, GLfloat v0) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    uniform_write(ctx, location, &v0, 1);
}

void glUniform2f(GLint location, GLfloat v0, GLfloat v1) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    float v[2] = { v0, v1 };
    uniform_write(ctx, location, v, 2);
}

void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    float v[3] = { v0, v1, v2 };
    uniform_write(ctx, location, v, 3);
}

void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2,
                 GLfloat v3) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    float v[4] = { v0, v1, v2, v3 };
    uniform_write(ctx, location, v, 4);
}

void glUniform1i(GLint location, GLint v0) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    /* This is how a sampler is pointed at a texture unit, and it is also how
     * an int uniform is set; both store one float. */
    float f = (float)v0;
    uniform_write(ctx, location, &f, 1);
}

void glUniform1fv(GLint location, GLsizei count, const GLfloat *value) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!value || count < 1) { gl_set_error(GL_INVALID_VALUE); return; }
    uniform_write(ctx, location, value, 1);
}

void glUniform2fv(GLint location, GLsizei count, const GLfloat *value) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!value || count < 1) { gl_set_error(GL_INVALID_VALUE); return; }
    uniform_write(ctx, location, value, 2);
}

void glUniform3fv(GLint location, GLsizei count, const GLfloat *value) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!value || count < 1) { gl_set_error(GL_INVALID_VALUE); return; }
    uniform_write(ctx, location, value, 3);
}

void glUniform4fv(GLint location, GLsizei count, const GLfloat *value) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!value || count < 1) { gl_set_error(GL_INVALID_VALUE); return; }
    uniform_write(ctx, location, value, 4);
}

/* Matrix uniforms.  `transpose` must be GL_FALSE in GL ES 2.0, but desktop
 * code passes GL_TRUE and expects it to work, so it is honoured. */
static void uniform_matrix(struct aglx_context *ctx, GLint location,
                           GLboolean transpose, const GLfloat *value, int n) {
    if (!value) { gl_set_error(GL_INVALID_VALUE); return; }

    float tmp[16];
    if (transpose) {
        for (int c = 0; c < n; c++) {
            for (int r = 0; r < n; r++) tmp[c * n + r] = value[r * n + c];
        }
        value = tmp;
    }
    uniform_write(ctx, location, value, n * n);
}

void glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *value) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (count < 1) { gl_set_error(GL_INVALID_VALUE); return; }
    uniform_matrix(ctx, location, transpose, value, 2);
}

void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *value) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (count < 1) { gl_set_error(GL_INVALID_VALUE); return; }
    uniform_matrix(ctx, location, transpose, value, 3);
}

void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
                        const GLfloat *value) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (count < 1) { gl_set_error(GL_INVALID_VALUE); return; }
    uniform_matrix(ctx, location, transpose, value, 4);
}

/* ============================================================================
 * Lookup helpers used by the pipeline (glshaderpipe.c)
 * ==========================================================================*/

const gl_uniform_t *gl_program_find_uniform(const gl_program_t *p,
                                            const char *name) {
    for (int i = 0; i < p->uniform_count; i++) {
        if (strcmp(p->uniforms[i].name, name) == 0) return &p->uniforms[i];
    }
    return (const gl_uniform_t *)0;
}

const gl_varying_t *gl_program_find_varying(const gl_program_t *p,
                                            const char *name) {
    for (int i = 0; i < p->varying_count; i++) {
        if (strcmp(p->varyings[i].name, name) == 0) return &p->varyings[i];
    }
    return (const gl_varying_t *)0;
}

const gl_attrib_info_t *gl_program_find_attrib(const gl_program_t *p,
                                               const char *name) {
    for (int i = 0; i < p->attrib_count; i++) {
        if (strcmp(p->attribs[i].name, name) == 0) return &p->attribs[i];
    }
    return (const gl_attrib_info_t *)0;
}

void *gl_program_vertex_unit(struct aglx_context *ctx, const gl_program_t *p) {
    gl_shader_t *s = find_shader(ctx, p->vertex_shader);
    return s ? s->unit : NULL;
}

void *gl_program_fragment_unit(struct aglx_context *ctx,
                               const gl_program_t *p) {
    gl_shader_t *s = find_shader(ctx, p->fragment_shader);
    return s ? s->unit : NULL;
}

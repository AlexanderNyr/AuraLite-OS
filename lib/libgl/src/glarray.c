/* libgl/src/glarray.c — vertex arrays and buffer objects.
 *
 * Phase G7 of GL_PLAN.md.  GL 1.1 vertex arrays plus the GL 1.5 buffer-object
 * subset.
 *
 * WHY THIS EXISTS
 *
 * Immediate mode costs a function call per attribute per vertex.  A 10 000
 * triangle model means 30 000 glVertex calls plus as many glColor and
 * glNormal, every frame.  Vertex arrays hand the whole batch over once and let
 * the implementation walk it.
 *
 * MEASURED PERFORMANCE, AND WHY IT IS NOT FASTER HERE
 *
 * On this implementation glDrawArrays is NOT measurably faster than the
 * equivalent glBegin/glEnd loop: 10 000 triangles cost ~4.5 ms either way, and
 * ~3.2 ms even when every triangle is degenerate so nothing is rasterised.
 *
 * That is because the array path deliberately submits through the same
 * immediate-mode entry points, and the cost is dominated by the per-vertex
 * transform (two 4x4 matrix multiplies, clipping, the viewport map), not by
 * call dispatch.  Removing a function call per vertex saves nothing next to
 * that.
 *
 * The alternative — a separate bulk path that inlines the transform — would
 * duplicate the pipeline and give two code paths that can disagree about
 * lighting, clipping or attribute latching.  For a software rasterizer whose
 * bottleneck is arithmetic, the correctness risk outweighs the gain.  Arrays
 * are provided here for API completeness and for the applications that expect
 * them, not as an optimisation; a genuine speed-up needs a faster transform
 * stage (SIMD, or the hardware backend of phase G9).
 *
 * THE POINTER/OFFSET OVERLOAD
 *
 * The single nastiest detail in this API: glVertexPointer's last argument is a
 * client pointer when no buffer is bound, and a BYTE OFFSET into the bound
 * buffer when one is.  Worse, the binding that matters is the one in force
 * when the *pointer* call is made, not when the draw call happens — so the
 * binding has to be captured per array, which is what gl_array_t::buffer does.
 * Getting this wrong produces either garbage geometry or a wild pointer
 * dereference.
 */

#include <stdlib.h>
#include <string.h>

#include "GL/gl.h"
#include "GL/glmath.h"
#include "glcontext.h"
#include "glvertex.h"

/* Immediate-mode entry points are reused to submit array elements: the vertex
 * pipeline, lighting and clipping all already hang off them, so routing
 * through them keeps one code path instead of two that can disagree. */
void gl_imm_reset(void);

/* ============================================================================
 * Defaults
 * ==========================================================================*/

static void array_defaults(gl_array_t *a, GLint size, GLenum type) {
    a->enabled = GL_FALSE;
    a->size    = size;
    a->type    = type;
    a->stride  = 0;
    a->ptr     = (const void *)0;
    a->buffer  = 0;
}

void gl_array_set_defaults(struct aglx_context *ctx) {
    array_defaults(&ctx->array_vertex,   4, GL_FLOAT);
    array_defaults(&ctx->array_color,    4, GL_FLOAT);
    array_defaults(&ctx->array_normal,   3, GL_FLOAT);
    for (int u = 0; u < GL_MAX_TEXTURE_UNITS_IMPL; u++) {
        array_defaults(&ctx->array_texcoord[u], 4, GL_FLOAT);
    }

    for (int i = 0; i < GL_MAX_BUFFERS_IMPL; i++) {
        ctx->buffers[i].name  = 0;
        ctx->buffers[i].used  = 0;
        ctx->buffers[i].data  = (void *)0;
        ctx->buffers[i].size  = 0;
        ctx->buffers[i].usage = GL_STATIC_DRAW;
    }
    ctx->next_buffer_name        = 1;
    ctx->buffer_binding_array    = 0;
    ctx->buffer_binding_element  = 0;

    for (int i = 0; i < GL_MAX_LISTS_IMPL; i++) {
        ctx->lists[i].name     = 0;
        ctx->lists[i].used     = 0;
        ctx->lists[i].cmds     = (gl_list_cmd_t *)0;
        ctx->lists[i].count    = 0;
        ctx->lists[i].capacity = 0;
    }
    ctx->next_list_name = 1;
    ctx->list_compiling = -1;
    ctx->list_mode      = GL_COMPILE;
}

void gl_array_free_all(struct aglx_context *ctx) {
    for (int i = 0; i < GL_MAX_BUFFERS_IMPL; i++) {
        free(ctx->buffers[i].data);
        ctx->buffers[i].data = (void *)0;
        ctx->buffers[i].used = 0;
    }
    for (int i = 0; i < GL_MAX_LISTS_IMPL; i++) {
        free(ctx->lists[i].cmds);
        ctx->lists[i].cmds = (gl_list_cmd_t *)0;
        ctx->lists[i].used = 0;
    }
}

/* ============================================================================
 * Buffer objects
 * ==========================================================================*/

static gl_buffer_t *find_buffer(struct aglx_context *ctx, GLuint name) {
    if (name == 0) return (gl_buffer_t *)0;
    for (int i = 0; i < GL_MAX_BUFFERS_IMPL; i++) {
        if (ctx->buffers[i].used && ctx->buffers[i].name == name) {
            return &ctx->buffers[i];
        }
    }
    return (gl_buffer_t *)0;
}

static GLuint *binding_for_target(struct aglx_context *ctx, GLenum target) {
    if (target == GL_ARRAY_BUFFER)         return &ctx->buffer_binding_array;
    if (target == GL_ELEMENT_ARRAY_BUFFER) return &ctx->buffer_binding_element;
    return (GLuint *)0;
}

void glGenBuffers(GLsizei n, GLuint *buffers) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!buffers || n == 0) return;

    GLsizei made = 0;
    for (int i = 0; i < GL_MAX_BUFFERS_IMPL && made < n; i++) {
        if (ctx->buffers[i].used) continue;
        gl_buffer_t *b = &ctx->buffers[i];
        b->used  = 1;
        b->name  = ctx->next_buffer_name++;
        b->data  = (void *)0;
        b->size  = 0;
        b->usage = GL_STATIC_DRAW;
        buffers[made++] = b->name;
    }
    if (made < n) {
        for (GLsizei k = made; k < n; k++) buffers[k] = 0;
        gl_set_error(GL_OUT_OF_MEMORY);
    }
}

void glDeleteBuffers(GLsizei n, const GLuint *buffers) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (n < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!buffers) return;

    for (GLsizei k = 0; k < n; k++) {
        gl_buffer_t *b = find_buffer(ctx, buffers[k]);
        if (!b) continue;                  /* unknown names are ignored */
        free(b->data);
        b->data = (void *)0;
        b->used = 0;
        b->name = 0;
        if (ctx->buffer_binding_array   == buffers[k]) ctx->buffer_binding_array   = 0;
        if (ctx->buffer_binding_element == buffers[k]) ctx->buffer_binding_element = 0;

        /* An array still referencing the deleted buffer would dereference an
         * offset against freed storage on the next draw.  Disarm it. */
        gl_array_t *arrays[3 + GL_MAX_TEXTURE_UNITS_IMPL];
        arrays[0] = &ctx->array_vertex;
        arrays[1] = &ctx->array_color;
        arrays[2] = &ctx->array_normal;
        for (int u = 0; u < GL_MAX_TEXTURE_UNITS_IMPL; u++) {
            arrays[3 + u] = &ctx->array_texcoord[u];
        }
        for (int a = 0; a < 3 + GL_MAX_TEXTURE_UNITS_IMPL; a++) {
            if (arrays[a]->buffer == buffers[k]) {
                arrays[a]->buffer = 0;
                arrays[a]->ptr    = (const void *)0;
            }
        }
    }
}

GLboolean glIsBuffer(GLuint buffer) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return GL_FALSE;
    return find_buffer(ctx, buffer) ? GL_TRUE : GL_FALSE;
}

void glBindBuffer(GLenum target, GLuint buffer) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    GLuint *binding = binding_for_target(ctx, target);
    if (!binding) { gl_set_error(GL_INVALID_ENUM); return; }

    if (buffer == 0) { *binding = 0; return; }

    if (!find_buffer(ctx, buffer)) {
        /* As with textures, binding an unseen name creates the object. */
        for (int i = 0; i < GL_MAX_BUFFERS_IMPL; i++) {
            if (ctx->buffers[i].used) continue;
            gl_buffer_t *b = &ctx->buffers[i];
            b->used = 1; b->name = buffer;
            b->data = (void *)0; b->size = 0; b->usage = GL_STATIC_DRAW;
            if (buffer >= ctx->next_buffer_name) ctx->next_buffer_name = buffer + 1;
            *binding = buffer;
            return;
        }
        gl_set_error(GL_OUT_OF_MEMORY);
        return;
    }
    *binding = buffer;
}

void glBufferData(GLenum target, GLsizeiptr size, const GLvoid *data,
                  GLenum usage) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    GLuint *binding = binding_for_target(ctx, target);
    if (!binding) { gl_set_error(GL_INVALID_ENUM); return; }
    if (size < 0) { gl_set_error(GL_INVALID_VALUE); return; }

    switch (usage) {
    case GL_STREAM_DRAW: case GL_STATIC_DRAW: case GL_DYNAMIC_DRAW: break;
    default: gl_set_error(GL_INVALID_ENUM); return;
    }

    gl_buffer_t *b = find_buffer(ctx, *binding);
    if (!b) { gl_set_error(GL_INVALID_OPERATION); return; }

    free(b->data);
    b->data  = (void *)0;
    b->size  = 0;
    b->usage = usage;
    if (size == 0) return;

    b->data = malloc((size_t)size);
    if (!b->data) { gl_set_error(GL_OUT_OF_MEMORY); return; }
    b->size = size;

    /* A NULL data pointer reserves storage without initialising it (§2.9).
     * Zeroing beats handing back heap garbage. */
    if (data) memcpy(b->data, data, (size_t)size);
    else      memset(b->data, 0, (size_t)size);
}

void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                     const GLvoid *data) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    GLuint *binding = binding_for_target(ctx, target);
    if (!binding) { gl_set_error(GL_INVALID_ENUM); return; }
    if (offset < 0 || size < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!data) { gl_set_error(GL_INVALID_VALUE); return; }

    gl_buffer_t *b = find_buffer(ctx, *binding);
    if (!b || !b->data) { gl_set_error(GL_INVALID_OPERATION); return; }
    if (offset + size > b->size) { gl_set_error(GL_INVALID_VALUE); return; }

    memcpy((unsigned char *)b->data + offset, data, (size_t)size);
}

/* ============================================================================
 * Client array state
 * ==========================================================================*/

static gl_array_t *array_slot(struct aglx_context *ctx, GLenum which) {
    switch (which) {
    case GL_VERTEX_ARRAY:        return &ctx->array_vertex;
    case GL_COLOR_ARRAY:         return &ctx->array_color;
    case GL_NORMAL_ARRAY:        return &ctx->array_normal;
    /* The texture-coordinate array is selected by glClientActiveTexture, not
     * by glActiveTexture: the client-side and server-side selectors are
     * independent in GL 1.3, and mixing them up silently feeds unit 1 the
     * coordinates meant for unit 0. */
    case GL_TEXTURE_COORD_ARRAY:
        return &ctx->array_texcoord[ctx->client_active_texture];
    default:                     return (gl_array_t *)0;
    }
}

void glEnableClientState(GLenum array) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    gl_array_t *a = array_slot(ctx, array);
    if (!a) { gl_set_error(GL_INVALID_ENUM); return; }
    a->enabled = GL_TRUE;
}

void glDisableClientState(GLenum array) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    gl_array_t *a = array_slot(ctx, array);
    if (!a) { gl_set_error(GL_INVALID_ENUM); return; }
    a->enabled = GL_FALSE;
}

/* Is `type` one this implementation can read? */
static int type_is_supported(GLenum type) {
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE:
    case GL_SHORT: case GL_UNSIGNED_SHORT:
    case GL_INT: case GL_UNSIGNED_INT:
    case GL_FLOAT: case GL_DOUBLE:
        return 1;
    default:
        return 0;
    }
}

static int type_size(GLenum type) {
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE:   return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_INT: case GL_UNSIGNED_INT:
    case GL_FLOAT:                         return 4;
    case GL_DOUBLE:                        return 8;
    default:                               return 0;
    }
}

static void set_pointer(struct aglx_context *ctx, gl_array_t *a,
                        GLint size, GLenum type, GLsizei stride,
                        const GLvoid *ptr) {
    if (stride < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (!type_is_supported(type)) { gl_set_error(GL_INVALID_ENUM); return; }

    a->size   = size;
    a->type   = type;
    a->stride = stride;
    a->ptr    = ptr;
    /* Capture the binding NOW: see the header comment on the pointer/offset
     * overload. */
    a->buffer = ctx->buffer_binding_array;
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (size < 2 || size > 4) { gl_set_error(GL_INVALID_VALUE); return; }
    set_pointer(ctx, &ctx->array_vertex, size, type, stride, ptr);
}

void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (size < 3 || size > 4) { gl_set_error(GL_INVALID_VALUE); return; }
    set_pointer(ctx, &ctx->array_color, size, type, stride, ptr);
}

void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    /* Normals are always 3 components (§2.8). */
    set_pointer(ctx, &ctx->array_normal, 3, type, stride, ptr);
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (size < 1 || size > 4) { gl_set_error(GL_INVALID_VALUE); return; }
    set_pointer(ctx, &ctx->array_texcoord[ctx->client_active_texture],
                size, type, stride, ptr);
}

/* ============================================================================
 * Reading array elements
 * ==========================================================================*/

/* Base address of an array: either the client pointer, or the bound buffer's
 * storage plus the offset the pointer argument encoded.  Returns NULL when the
 * array cannot be read, which the caller must treat as "skip". */
static const unsigned char *array_base(struct aglx_context *ctx,
                                       const gl_array_t *a,
                                       GLsizeiptr *avail_out) {
    if (a->buffer != 0) {
        gl_buffer_t *b = find_buffer(ctx, a->buffer);
        if (!b || !b->data) return (const unsigned char *)0;
        GLsizeiptr off = (GLsizeiptr)(uintptr_t)a->ptr;
        if (off < 0 || off > b->size) return (const unsigned char *)0;
        if (avail_out) *avail_out = b->size - off;
        return (const unsigned char *)b->data + off;
    }
    if (!a->ptr) return (const unsigned char *)0;
    /* Client memory: the application owns the bounds, GL cannot check them. */
    if (avail_out) *avail_out = -1;
    return (const unsigned char *)a->ptr;
}

/* Read up to 4 components of element `index` into `out`, normalising integer
 * types the way GL does for colours (§2.13). */
static int read_element(struct aglx_context *ctx, const gl_array_t *a,
                        GLint index, GLfloat out[4], int normalise_ints) {
    GLsizeiptr avail = -1;
    const unsigned char *base = array_base(ctx, a, &avail);
    if (!base) return 0;

    int esz = type_size(a->type);
    if (esz == 0) return 0;
    int stride = a->stride ? a->stride : (esz * a->size);

    GLsizeiptr byte_off = (GLsizeiptr)index * stride;
    if (avail >= 0 && byte_off + (GLsizeiptr)(esz * a->size) > avail) {
        /* Reading past the end of a bound buffer would be a wild read; GL
         * leaves this undefined, but returning zero is the safe choice for an
         * OS that must not fault on application error. */
        return 0;
    }

    const unsigned char *p = base + byte_off;
    for (int c = 0; c < 4; c++) out[c] = (c == 3) ? 1.0f : 0.0f;

    for (int c = 0; c < a->size && c < 4; c++) {
        const unsigned char *q = p + (size_t)c * esz;
        switch (a->type) {
        case GL_FLOAT: {
            float v; memcpy(&v, q, sizeof v); out[c] = v; break;
        }
        case GL_DOUBLE: {
            double v; memcpy(&v, q, sizeof v); out[c] = (GLfloat)v; break;
        }
        case GL_BYTE: {
            signed char v; memcpy(&v, q, sizeof v);
            out[c] = normalise_ints ? (GLfloat)v / 127.0f : (GLfloat)v;
            break;
        }
        case GL_UNSIGNED_BYTE: {
            unsigned char v = *q;
            out[c] = normalise_ints ? (GLfloat)v / 255.0f : (GLfloat)v;
            break;
        }
        case GL_SHORT: {
            short v; memcpy(&v, q, sizeof v);
            out[c] = normalise_ints ? (GLfloat)v / 32767.0f : (GLfloat)v;
            break;
        }
        case GL_UNSIGNED_SHORT: {
            unsigned short v; memcpy(&v, q, sizeof v);
            out[c] = normalise_ints ? (GLfloat)v / 65535.0f : (GLfloat)v;
            break;
        }
        case GL_INT: {
            int v; memcpy(&v, q, sizeof v);
            out[c] = normalise_ints ? (GLfloat)v / 2147483647.0f : (GLfloat)v;
            break;
        }
        case GL_UNSIGNED_INT: {
            unsigned int v; memcpy(&v, q, sizeof v);
            out[c] = normalise_ints ? (GLfloat)v / 4294967295.0f : (GLfloat)v;
            break;
        }
        default:
            return 0;
        }
    }
    return 1;
}

/* Submit one array element through the immediate-mode entry points.
 *
 * Attribute order matters: colour, normal and texture coordinate must be set
 * BEFORE the vertex, because glVertex is what latches the current attributes
 * and pushes the vertex into primitive assembly. */
void glArrayElement(GLint i) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (i < 0) { gl_set_error(GL_INVALID_VALUE); return; }

    /* ---- The shader path (G11c) ----
     *
     * With a program bound, the vertex shader replaces the whole
     * colour/normal/texcoord/position sequence below: its inputs are the
     * GENERIC attribute arrays, not the fixed-function ones, and its output
     * is a clip-space position plus varyings.  Routing it into the same
     * assembler keeps one implementation of strips, fans and quads. */
    if (gl_shader_active(ctx)) {
        gl_vertex_t sv;
        if (gl_shader_run_vertex(ctx, i, &sv)) {
            gl_imm_submit_vertex(ctx, &sv);
        }
        return;
    }

    GLfloat v[4];

    if (ctx->array_color.enabled &&
        read_element(ctx, &ctx->array_color, i, v, 1)) {
        glColor4f(v[0], v[1], v[2], ctx->array_color.size == 4 ? v[3] : 1.0f);
    }
    if (ctx->array_normal.enabled &&
        read_element(ctx, &ctx->array_normal, i, v, 1)) {
        glNormal3f(v[0], v[1], v[2]);
    }
    for (int u = 0; u < GL_MAX_TEXTURE_UNITS_IMPL; u++) {
        gl_array_t *a = &ctx->array_texcoord[u];
        if (!a->enabled) continue;
        if (!read_element(ctx, a, i, v, 0)) continue;
        if (u == 0) {
            /* Unit 0 goes through glTexCoord3f so a three-component array
             * feeds a 3D texture's r coordinate. */
            glTexCoord3f(v[0], v[1], a->size >= 3 ? v[2] : 0.0f);
        } else {
            glMultiTexCoord2f((GLenum)(GL_TEXTURE0 + u), v[0], v[1]);
        }
    }
    if (ctx->array_vertex.enabled &&
        read_element(ctx, &ctx->array_vertex, i, v, 0)) {
        glVertex4f(v[0], v[1], v[2], ctx->array_vertex.size == 4 ? v[3] : 1.0f);
    }
}

/* ============================================================================
 * Draw calls
 * ==========================================================================*/

static int mode_is_valid(GLenum mode) {
    switch (mode) {
    case GL_POINTS: case GL_LINES: case GL_LINE_STRIP: case GL_LINE_LOOP:
    case GL_TRIANGLES: case GL_TRIANGLE_STRIP: case GL_TRIANGLE_FAN:
    case GL_QUADS: case GL_QUAD_STRIP: case GL_POLYGON:
        return 1;
    default:
        return 0;
    }
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!mode_is_valid(mode)) { gl_set_error(GL_INVALID_ENUM); return; }
    if (count < 0 || first < 0) { gl_set_error(GL_INVALID_VALUE); return; }
    if (count == 0) return;

    /* Without a vertex array there is nothing to draw; the other arrays are
     * optional (§2.8).
     *
     * A shader program supplies its position from a generic attribute
     * instead, so the fixed-function vertex array is not required -- and
     * demanding it would make every ES 2.0 application draw nothing. */
    if (!gl_shader_active(ctx) && !ctx->array_vertex.enabled) return;

    gl_imm_begin_internal(mode);
    for (GLsizei k = 0; k < count; k++) glArrayElement(first + k);
    gl_imm_end_internal();
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type,
                    const GLvoid *indices) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!mode_is_valid(mode)) { gl_set_error(GL_INVALID_ENUM); return; }
    if (count < 0) { gl_set_error(GL_INVALID_VALUE); return; }

    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT &&
        type != GL_UNSIGNED_INT) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    if (count == 0) return;
    if (!gl_shader_active(ctx) && !ctx->array_vertex.enabled) return;

    /* Indices come from the element buffer when one is bound, in which case
     * `indices` is a byte offset — the same overload as the vertex arrays. */
    const unsigned char *idx_base;
    GLsizeiptr idx_avail = -1;
    if (ctx->buffer_binding_element != 0) {
        gl_buffer_t *b = find_buffer(ctx, ctx->buffer_binding_element);
        if (!b || !b->data) { gl_set_error(GL_INVALID_OPERATION); return; }
        GLsizeiptr off = (GLsizeiptr)(uintptr_t)indices;
        if (off < 0 || off > b->size) { gl_set_error(GL_INVALID_VALUE); return; }
        idx_base  = (const unsigned char *)b->data + off;
        idx_avail = b->size - off;
    } else {
        if (!indices) { gl_set_error(GL_INVALID_VALUE); return; }
        idx_base = (const unsigned char *)indices;
    }

    int isz = (type == GL_UNSIGNED_BYTE) ? 1
            : (type == GL_UNSIGNED_SHORT) ? 2 : 4;
    if (idx_avail >= 0 && (GLsizeiptr)count * isz > idx_avail) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }

    gl_imm_begin_internal(mode);
    for (GLsizei k = 0; k < count; k++) {
        const unsigned char *q = idx_base + (size_t)k * isz;
        GLint index;
        if (type == GL_UNSIGNED_BYTE) {
            index = (GLint)*q;
        } else if (type == GL_UNSIGNED_SHORT) {
            unsigned short v; memcpy(&v, q, sizeof v); index = (GLint)v;
        } else {
            unsigned int v; memcpy(&v, q, sizeof v); index = (GLint)v;
        }
        glArrayElement(index);
    }
    gl_imm_end_internal();
}

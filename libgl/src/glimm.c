/* libgl/src/glimm.c — immediate mode: glBegin/glVertex/glColor/glEnd.
 *
 * Phase G2 of GL_PLAN.md.
 *
 * Immediate mode is the classic GL 1.1 way to submit geometry.  Vertices are
 * accumulated between glBegin() and glEnd(), then assembled into primitives
 * according to the mode.
 *
 * WHEN PRIMITIVES ARE EMITTED
 *
 * Rather than buffering the whole batch and assembling at glEnd(), primitives
 * are emitted as soon as enough vertices have arrived.  That keeps memory use
 * constant no matter how long a GL_TRIANGLE_STRIP or GL_LINE_STRIP runs — a
 * glBegin(GL_TRIANGLE_STRIP) block with 100000 vertices uses the same few
 * bytes as one with three.  Only GL_LINE_LOOP and GL_POLYGON need state that
 * survives to glEnd(), and both need just the first vertex.
 *
 * The transform stage runs per vertex at specification time, which is also
 * what makes the current MODELVIEW/PROJECTION matrices at that instant the
 * ones that apply — the behaviour applications rely on when they change
 * matrices mid-batch.
 */

#include "GL/gl.h"
#include "GL/glmath.h"
#include "glcontext.h"
#include "glvertex.h"

/* The current matrix of the active mode (defined in glmatrix.c). */
glm_mat4 *gl_current_matrix(struct aglx_context *ctx);

/* ============================================================================
 * Transform stage (§2.10)
 * ==========================================================================*/

void gl_transform_vertex(struct aglx_context *ctx,
                         GLfloat x, GLfloat y, GLfloat z, GLfloat w,
                         gl_vertex_t *out) {
    glm_vec4 obj = glm_vec4_make(x, y, z, w);

    /* object -> eye -> clip.  The pipeline stops here: from G4 the clipper
     * runs next and needs the pre-divide clip coordinates (see glclip.c). */
    glm_vec4 eye  = glm_mat4_transform4(ctx->modelview[ctx->modelview_top], obj);
    glm_vec4 clip = glm_mat4_transform4(ctx->projection[ctx->projection_top], eye);

    out->clip  = clip;
    /* Eye position is kept because lighting is evaluated in eye space (G5). */
    out->eye   = glm_vec3_make(eye.x, eye.y, eye.z);
    out->valid = 1;
    out->inv_w = 0.0f;
    out->win   = glm_vec3_make(0.0f, 0.0f, 0.0f);
}

/* ============================================================================
 * Immediate-mode state
 *
 * Held outside the context struct for now because it is transient per-batch
 * data rather than persistent GL state.  It moves into the context when libgl
 * gains multi-threading.
 * ==========================================================================*/

static struct {
    int         active;         /* inside glBegin/glEnd? */
    GLenum      mode;
    int         count;          /* vertices seen in this batch */

    /* Rolling window of recent vertices.  Three is enough for every mode:
     * triangles need three, quads are emitted as two triangles from four but
     * only ever need the previous three plus the new one. */
    gl_vertex_t v[4];

    /* GL_LINE_LOOP / GL_POLYGON need the first vertex at glEnd(). */
    gl_vertex_t first;

    /* GL_TRIANGLE_STRIP alternates winding; tracked so the emitted triangles
     * all have consistent orientation for the culling stage in G3. */
    int         strip_parity;
} imm;

/* Current per-vertex attributes, latched when glVertex is called (§2.7). */
static gl_color_t cur_color = { 1.0f, 1.0f, 1.0f, 1.0f };
static glm_vec3   cur_normal;
/* Current texture coordinate per unit (G10; G6 had a single s,t pair). */
static GLfloat    cur_s[GL_MAX_TEXTURE_UNITS_IMPL];
static GLfloat    cur_t[GL_MAX_TEXTURE_UNITS_IMPL];
static GLfloat    cur_r[GL_MAX_TEXTURE_UNITS_IMPL];
static int        cur_normal_init = 0;

/* ============================================================================
 * Primitive assembly
 * ==========================================================================*/

/* The assembler produces CLIP-space primitives; these route them through the
 * frustum clipper, which projects the survivors and hands window-space
 * vertices to the rasterizer.  Before G4 the transform stage projected
 * directly and primitives crossing the near plane were dropped whole. */
static void emit_triangle(struct aglx_context *ctx,
                          const gl_vertex_t *a, const gl_vertex_t *b,
                          const gl_vertex_t *c) {
    gl_clip_and_emit_triangle(ctx, a, b, c, gl_raster_triangle);
}

static void emit_line(struct aglx_context *ctx,
                      const gl_vertex_t *a, const gl_vertex_t *b) {
    gl_clip_and_emit_line(ctx, a, b, gl_raster_line);
}

static void emit_point(struct aglx_context *ctx, const gl_vertex_t *v) {
    gl_clip_and_emit_point(ctx, v, gl_raster_point);
}

/* Feed one fully transformed vertex into the assembler. */
/* Feed a vertex the SHADER pipeline produced straight into assembly.
 *
 * The shader path skips glVertex4f entirely -- there is no current colour,
 * normal or texture coordinate to latch, and no matrix to multiply by -- but
 * it must reach exactly the same primitive assembler, or strips, fans and
 * quads would need a second implementation that could disagree about winding.
 */
void gl_imm_submit_vertex(struct aglx_context *ctx, const gl_vertex_t *v);

static void assemble(struct aglx_context *ctx, const gl_vertex_t *nv);

void gl_imm_submit_vertex(struct aglx_context *ctx, const gl_vertex_t *v) {
    if (!imm.active) { gl_set_error(GL_INVALID_OPERATION); return; }
    assemble(ctx, v);
}

static void assemble(struct aglx_context *ctx, const gl_vertex_t *nv) {
    int n = imm.count;

    switch (imm.mode) {

    case GL_POINTS:
        emit_point(ctx, nv);
        break;

    case GL_LINES:
        /* Independent segments: emit on every second vertex. */
        imm.v[n & 1] = *nv;
        if ((n & 1) == 1) emit_line(ctx, &imm.v[0], &imm.v[1]);
        break;

    case GL_LINE_STRIP:
        if (n > 0) emit_line(ctx, &imm.v[0], nv);
        imm.v[0] = *nv;
        break;

    case GL_LINE_LOOP:
        if (n == 0) imm.first = *nv;
        else        emit_line(ctx, &imm.v[0], nv);
        imm.v[0] = *nv;
        /* The closing segment is emitted by glEnd(). */
        break;

    case GL_TRIANGLES:
        imm.v[n % 3] = *nv;
        if (n % 3 == 2) emit_triangle(ctx, &imm.v[0], &imm.v[1], &imm.v[2]);
        break;

    case GL_TRIANGLE_STRIP:
        if (n < 2) {
            imm.v[n] = *nv;
        } else {
            /* Every new vertex forms a triangle with the previous two.  The
             * winding alternates, so odd triangles swap two vertices to keep
             * a consistent front face (§2.6.1). */
            if (imm.strip_parity == 0) emit_triangle(ctx, &imm.v[0], &imm.v[1], nv);
            else                       emit_triangle(ctx, &imm.v[1], &imm.v[0], nv);
            imm.strip_parity ^= 1;
            imm.v[0] = imm.v[1];
            imm.v[1] = *nv;
        }
        break;

    case GL_TRIANGLE_FAN:
        if (n == 0)      imm.first = *nv;   /* the hub */
        else if (n == 1) imm.v[0]  = *nv;
        else {
            emit_triangle(ctx, &imm.first, &imm.v[0], nv);
            imm.v[0] = *nv;
        }
        break;

    case GL_QUADS:
        /* Four vertices make a quad, split into two triangles. */
        imm.v[n % 4] = *nv;
        if (n % 4 == 3) {
            emit_triangle(ctx, &imm.v[0], &imm.v[1], &imm.v[2]);
            emit_triangle(ctx, &imm.v[0], &imm.v[2], &imm.v[3]);
        }
        break;

    case GL_QUAD_STRIP:
        /* Vertices arrive in pairs; each new pair closes a quad with the
         * previous pair.  Note the vertex order: v0,v1,v3,v2 — a quad strip
         * is specified in a zig-zag, unlike GL_QUADS. */
        if (n < 2) {
            imm.v[n] = *nv;
        } else {
            imm.v[2 + (n & 1)] = *nv;
            if ((n & 1) == 1) {
                emit_triangle(ctx, &imm.v[0], &imm.v[1], &imm.v[3]);
                emit_triangle(ctx, &imm.v[0], &imm.v[3], &imm.v[2]);
                imm.v[0] = imm.v[2];
                imm.v[1] = imm.v[3];
            }
        }
        break;

    case GL_POLYGON:
        /* Convex polygon as a triangle fan from the first vertex. */
        if (n == 0)      imm.first = *nv;
        else if (n == 1) imm.v[0]  = *nv;
        else {
            emit_triangle(ctx, &imm.first, &imm.v[0], nv);
            imm.v[0] = *nv;
        }
        break;

    default:
        break;
    }

    imm.count++;
}

/* ============================================================================
 * Entry points
 * ==========================================================================*/

void glBegin(GLenum mode) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (imm.active) {
        /* Nested glBegin is illegal (§2.6.1) and must not disturb the batch
         * already in progress. */
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }

    switch (mode) {
    case GL_POINTS: case GL_LINES: case GL_LINE_STRIP: case GL_LINE_LOOP:
    case GL_TRIANGLES: case GL_TRIANGLE_STRIP: case GL_TRIANGLE_FAN:
    case GL_QUADS: case GL_QUAD_STRIP: case GL_POLYGON:
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        return;
    }

    /* Display lists record the command instead of executing it (§5.4). */
    {
        GLint iv[1]; iv[0] = (GLint)mode;
        if (gl_list_record(ctx, gl_lop_begin(), (const GLfloat *)0, 0, iv, 1))
            return;
    }

    /* Drawing into an incomplete framebuffer object is
     * GL_INVALID_FRAMEBUFFER_OPERATION (§4.4.4).  Refusing here rather than
     * per-primitive means one check per batch instead of one per triangle,
     * and the batch is discarded as a whole -- which is what the application
     * wants, since half a mesh drawn nowhere helps nobody. */
    if (!gl_fbo_target_ok(ctx)) {
        gl_set_error(GL_INVALID_FRAMEBUFFER_OPERATION);
        return;
    }

    imm.active       = 1;
    imm.mode         = mode;
    imm.count        = 0;
    imm.strip_parity = 0;
}

void glEnd(void) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (gl_list_record(ctx, gl_lop_end(), (const GLfloat *)0, 0,
                       (const GLint *)0, 0)) {
        return;
    }

    if (!imm.active) {
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }

    /* Close the loop: the only primitive whose last edge is implicit. */
    if (imm.mode == GL_LINE_LOOP && imm.count >= 2) {
        emit_line(ctx, &imm.v[0], &imm.first);
    }

    imm.active = 0;
    imm.count  = 0;
}

void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    {
        GLfloat fv[4]; fv[0] = x; fv[1] = y; fv[2] = z; fv[3] = w;
        if (gl_list_record(ctx, gl_lop_vertex4f(), fv, 4, (const GLint *)0, 0))
            return;
    }

    if (!imm.active) {
        /* glVertex outside glBegin/glEnd is a no-op with an error, rather
         * than undefined behaviour (§2.6.1). */
        gl_set_error(GL_INVALID_OPERATION);
        return;
    }

    gl_vertex_t v;
    gl_transform_vertex(ctx, x, y, z, w, &v);

    /* Latch the attributes current at this instant (§2.7). */
    glm_vec3 obj_normal = cur_normal_init ? cur_normal
                                          : glm_vec3_make(0.0f, 0.0f, 1.0f);

    /* Normals are transformed by the inverse-transpose of MODELVIEW, not by
     * MODELVIEW itself: under non-uniform scaling the plain matrix would shear
     * them away from the surface (§2.10.3). */
    glm_mat4 nm = glm_mat4_normal(ctx->modelview[ctx->modelview_top]);
    v.normal = glm_mat4_transform_dir(nm, obj_normal);

    /* GL_NORMALIZE rescales to unit length after transformation, which matters
     * when the MODELVIEW contains a scale: an unnormalised normal would make
     * the diffuse term too bright or too dark. */
    if (ctx->normalize) v.normal = glm_vec3_normalize(v.normal);

    for (int u = 0; u < GL_MAX_TEXTURE_UNITS_IMPL; u++) {
        v.s[u] = cur_s[u];
        v.t[u] = cur_t[u];
        v.r[u] = cur_r[u];
    }

    if (ctx->lighting) {
        /* Lighting is per-vertex in GL 1.1: the lit colour is computed here
         * and then Gouraud-interpolated by the rasterizer. */
        v.color = gl_light_vertex(ctx, v.eye, v.normal, cur_color, 0);
    } else {
        v.color = cur_color;
    }

    assemble(ctx, &v);
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { glVertex4f(x, y, z, 1.0f); }
void glVertex2f(GLfloat x, GLfloat y)            { glVertex4f(x, y, 0.0f, 1.0f); }

void glVertex3fv(const GLfloat *v) {
    if (!v) { gl_set_error(GL_INVALID_VALUE); return; }
    glVertex4f(v[0], v[1], v[2], 1.0f);
}

void glVertex2fv(const GLfloat *v) {
    if (!v) { gl_set_error(GL_INVALID_VALUE); return; }
    glVertex4f(v[0], v[1], 0.0f, 1.0f);
}

void glVertex4fv(const GLfloat *v) {
    if (!v) { gl_set_error(GL_INVALID_VALUE); return; }
    glVertex4f(v[0], v[1], v[2], v[3]);
}

/* ---- Colour ----
 *
 * Note there is no glBegin check here: unlike glVertex, colour may be set at
 * any time and simply becomes the current colour (§2.7).
 */
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    struct aglx_context *ctx = gl_current_ctx;
    if (ctx) {
        GLfloat fv[4]; fv[0] = r; fv[1] = g; fv[2] = b; fv[3] = a;
        if (gl_list_record(ctx, gl_lop_color4f(), fv, 4, (const GLint *)0, 0))
            return;
    }
    cur_color.r = r; cur_color.g = g; cur_color.b = b; cur_color.a = a;
}

void glColor3f(GLfloat r, GLfloat g, GLfloat b) { glColor4f(r, g, b, 1.0f); }

void glColor3ub(GLubyte r, GLubyte g, GLubyte b) {
    /* Integer colours map [0,255] onto [0,1] (§2.13). */
    glColor4f((GLfloat)r / 255.0f, (GLfloat)g / 255.0f,
              (GLfloat)b / 255.0f, 1.0f);
}

void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
    glColor4f((GLfloat)r / 255.0f, (GLfloat)g / 255.0f,
              (GLfloat)b / 255.0f, (GLfloat)a / 255.0f);
}

void glColor3fv(const GLfloat *v) {
    if (!v) { gl_set_error(GL_INVALID_VALUE); return; }
    glColor4f(v[0], v[1], v[2], 1.0f);
}

void glColor4fv(const GLfloat *v) {
    if (!v) { gl_set_error(GL_INVALID_VALUE); return; }
    glColor4f(v[0], v[1], v[2], v[3]);
}

/* ---- Normals and texture coordinates ----
 * Stored now, consumed by lighting in G5 and texturing in G6.
 */
void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) {
    struct aglx_context *ctx = gl_current_ctx;
    if (ctx) {
        GLfloat fv[3]; fv[0] = nx; fv[1] = ny; fv[2] = nz;
        if (gl_list_record(ctx, gl_lop_normal3f(), fv, 3, (const GLint *)0, 0))
            return;
    }
    cur_normal = glm_vec3_make(nx, ny, nz);
    cur_normal_init = 1;
}

void glNormal3fv(const GLfloat *v) {
    if (!v) { gl_set_error(GL_INVALID_VALUE); return; }
    glNormal3f(v[0], v[1], v[2]);
}

/* glTexCoord* always writes unit 0.  Only glMultiTexCoord* can reach the
 * others -- glActiveTexture does NOT redirect glTexCoord, which is a detail
 * applications get wrong far more often than implementations do. */
void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r) {
    struct aglx_context *ctx = gl_current_ctx;
    if (ctx) {
        GLfloat fv[3]; fv[0] = s; fv[1] = t; fv[2] = r;
        if (gl_list_record(ctx, gl_lop_texcoord2f(), fv, 3, (const GLint *)0, 0))
            return;
    }
    cur_s[0] = s; cur_t[0] = t; cur_r[0] = r;
}

void glTexCoord2f(GLfloat s, GLfloat t) {
    /* The third coordinate defaults to 0 (§2.7), which is also what a 3D
     * texture sampled by a 2D-coordinate application will see. */
    glTexCoord3f(s, t, 0.0f);
}

void glTexCoord2fv(const GLfloat *v) {
    if (!v) { gl_set_error(GL_INVALID_VALUE); return; }
    glTexCoord2f(v[0], v[1]);
}

void glMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t) {
    struct aglx_context *ctx = gl_current_ctx;
    if (target < GL_TEXTURE0 ||
        target >= GL_TEXTURE0 + GL_MAX_TEXTURE_UNITS_IMPL) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    int unit = (int)(target - GL_TEXTURE0);
    if (ctx) {
        GLfloat fv[3]; fv[0] = s; fv[1] = t; fv[2] = 0.0f;
        GLint iv[1]; iv[0] = unit;
        if (gl_list_record(ctx, gl_lop_multitexcoord(), fv, 3, iv, 1))
            return;
    }
    cur_s[unit] = s; cur_t[unit] = t; cur_r[unit] = 0.0f;
}

/* ---- Internal: reset immediate-mode state ----
 * Used by the unit tests so one test cannot leak a half-open glBegin into the
 * next.  Not part of the public GL API.
 */
void gl_imm_reset(void) {
    imm.active = 0;
    imm.count  = 0;
    imm.strip_parity = 0;
    cur_color.r = cur_color.g = cur_color.b = cur_color.a = 1.0f;
    cur_normal_init = 0;
    for (int u = 0; u < GL_MAX_TEXTURE_UNITS_IMPL; u++) {
        cur_s[u] = cur_t[u] = cur_r[u] = 0.0f;
    }
}

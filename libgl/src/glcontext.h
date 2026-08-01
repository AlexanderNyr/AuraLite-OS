/* libgl/src/glcontext.h — internal GL context definition.
 *
 * PRIVATE to libgl.  Applications see only the opaque aglx_context_t from
 * GL/auraglx.h; everything here is free to change between phases.
 *
 * The context owns all mutable GL state.  Keeping it in one struct (rather
 * than in scattered globals) is what will allow multiple contexts and,
 * later, per-thread current contexts without reworking every entry point.
 */
#ifndef AURALITE_GL_CONTEXT_H
#define AURALITE_GL_CONTEXT_H

#include <stdint.h>
#include "GL/gl.h"
#include "GL/glmath.h"

/* ---- Limits (GL 1.1 requires at least 32 / 2; we are more generous) ---- */
#define GL_MODELVIEW_STACK_DEPTH   32
#define GL_PROJECTION_STACK_DEPTH  8
#define GL_TEXTURE_STACK_DEPTH     4

/* glPushAttrib stack depth.  GL 1.1 requires at least 16. */
#define GL_ATTRIB_STACK_DEPTH      16

/* GL 1.1 requires at least 8 lights (§2.14.1). */
#define GL_MAX_LIGHTS_IMPL         8

/* Texture objects tracked per context.  GL has no fixed limit; this is an
 * implementation choice sized for the demos plus room to spare. */
#define GL_MAX_TEXTURES_IMPL       64

/* Buffer objects and display lists tracked per context. */
#define GL_MAX_BUFFERS_IMPL        64
#define GL_MAX_LISTS_IMPL          256

/* Commands recorded in one display list.  A list is a flat command log, not a
 * re-executable program, so this bounds the geometry a single list can hold. */
#define GL_LIST_MAX_CMDS           16384

/* A colour in the form the framebuffer stores: 0x00RRGGBB. */
typedef uint32_t gl_pixel_t;

/* Floating-point RGBA, the form GL works in internally. */
typedef struct {
    GLfloat r, g, b, a;
} gl_color_t;

/* ---- Texture object (§3.8) ----
 *
 * Texels are stored already unpacked to 32-bit RGBA in the framebuffer's
 * channel order, whatever format the application supplied.  Converting once at
 * glTexImage2D time keeps the inner sampling loop free of per-texel format
 * branches, at the cost of memory for GL_LUMINANCE images.  For a software
 * rasterizer that is the right trade: sampling happens millions of times per
 * frame, upload happens once.
 */
typedef struct {
    GLuint     name;             /* 0 means the slot is free                */
    int        used;
    uint32_t  *texels;           /* width*height, 0xAARRGGBB, or NULL       */
    GLsizei    width, height;
    GLenum     min_filter, mag_filter;
    GLenum     wrap_s, wrap_t;
} gl_texture_t;

/* ---- Vertex array pointer (§2.8) ----
 *
 * `ptr` is either a real client pointer or, when a buffer is bound at the time
 * glVertexPointer() and friends are called, a BYTE OFFSET into that buffer.
 * GL overloads the same argument for both, which is why `buffer` has to be
 * captured here: the binding in force at specification time is the one that
 * counts, not the binding in force at draw time.
 */
typedef struct {
    GLboolean    enabled;
    GLint        size;           /* components per element: 2,3,4          */
    GLenum       type;           /* GL_FLOAT, GL_UNSIGNED_BYTE, ...        */
    GLsizei      stride;         /* bytes between elements; 0 = tightly packed */
    const void  *ptr;
    GLuint       buffer;         /* 0 = client memory                      */
} gl_array_t;

/* ---- Buffer object (GL 1.5 subset) ---- */
typedef struct {
    GLuint     name;
    int        used;
    void      *data;
    GLsizeiptr size;
    GLenum     usage;
} gl_buffer_t;

/* ---- Display list (§5.4) ----
 *
 * Recorded as a flat log of (opcode, arguments) rather than as a captured
 * vertex batch, so a list can contain state changes and matrix operations as
 * well as geometry — which is what applications actually put in them.
 */
typedef struct {
    GLuint   op;
    GLfloat  f[8];
    GLint    i[4];
} gl_list_cmd_t;

typedef struct {
    GLuint          name;
    int             used;
    gl_list_cmd_t  *cmds;
    int             count;
    int             capacity;
} gl_list_t;

/* ---- Lighting (§2.14) ----
 *
 * One light source.  GL stores the position in EYE coordinates: glLightfv()
 * transforms whatever the application supplies by the MODELVIEW matrix current
 * at that moment, which is what makes "set the light before the camera
 * transform" behave differently from "set it after".
 *
 * A positional light has w = 1 and attenuates with distance; a directional
 * light has w = 0, in which case `position` is a direction TOWARDS the light
 * and no attenuation applies.
 */
typedef struct {
    GLboolean enabled;
    glm_vec4  position;          /* eye coordinates; w selects the light type */
    gl_color_t ambient, diffuse, specular;
    /* Attenuation: 1 / (kc + kl*d + kq*d^2), positional lights only. */
    GLfloat   constant_att, linear_att, quadratic_att;
    /* Spotlight.  cutoff == 180 means "not a spotlight" (the GL default). */
    glm_vec3  spot_direction;    /* eye coordinates */
    GLfloat   spot_exponent, spot_cutoff;
} gl_light_t;

/* Material properties for one face (§2.14.2). */
typedef struct {
    gl_color_t ambient, diffuse, specular, emission;
    GLfloat    shininess;        /* specular exponent, [0,128] */
} gl_material_t;

struct aglx_context {
    /* ---- Render targets ---- */
    int         width;          /* buffer width  in pixels */
    int         height;         /* buffer height in pixels */
    gl_pixel_t *color;          /* width*height, packed XRGB8888, never NULL */
    float      *depth;          /* width*height, or NULL when AGLX_DEPTH unset */

    /* ---- Window binding ---- */
    int         wid;            /* AuraGUI window id */
    uint32_t    flags;          /* AGLX_* creation flags */

    /* ---- Clear state (§4.2.3) ---- */
    gl_color_t  clear_color;
    GLfloat     clear_depth;    /* clamped to [0,1] */

    /* ---- Viewport (§2.11) ----
     * Stored exactly as glViewport() received it; the transform to window
     * coordinates happens per-vertex in the pipeline. */
    GLint       viewport_x, viewport_y;
    GLsizei     viewport_w, viewport_h;

    /* ---- Error state (§2.5) ----
     * GL keeps the FIRST error until it is read, so later errors never mask
     * the original cause.  glGetError() clears it. */
    GLenum      error;

    /* ---- Matrix state (§2.10.2) — populated in phase G2 ---- */
    GLenum      matrix_mode;
    glm_mat4    modelview[GL_MODELVIEW_STACK_DEPTH];
    int         modelview_top;
    glm_mat4    projection[GL_PROJECTION_STACK_DEPTH];
    int         projection_top;

    /* ---- Per-fragment state — populated in phase G3 ---- */
    GLboolean   depth_test;
    GLboolean   depth_mask;
    GLenum      depth_func;
    GLboolean   cull_face;
    GLenum      cull_mode;
    GLenum      front_face;
    GLenum      shade_model;
    GLenum      polygon_mode;   /* GL_FILL (default), GL_LINE, GL_POINT */
    GLboolean   scissor_test;
    GLint       scissor_x, scissor_y;
    GLsizei     scissor_w, scissor_h;

    /* ---- Lighting (§2.14), phase G5 ---- */
    GLboolean     lighting;
    gl_light_t    lights[GL_MAX_LIGHTS_IMPL];
    gl_material_t material_front, material_back;
    gl_color_t    light_model_ambient;
    GLboolean     light_model_two_side;
    GLboolean     normalize;
    GLboolean     color_material;
    GLenum        color_material_face, color_material_mode;

    /* ---- Texturing (§3.8), phase G6 ---- */
    GLboolean     texture_2d;              /* GL_TEXTURE_2D enabled?        */
    gl_texture_t  textures[GL_MAX_TEXTURES_IMPL];
    GLuint        texture_binding;         /* currently bound name, 0 = none */
    GLuint        next_texture_name;       /* monotonic name allocator       */
    GLenum        tex_env_mode;            /* MODULATE / REPLACE / DECAL / BLEND */
    gl_color_t    tex_env_color;

    /* ---- Blending (§4.1.7) ---- */
    GLboolean     blend;
    GLenum        blend_src, blend_dst;

    /* ---- Alpha test (§4.1.4) ---- */
    GLboolean     alpha_test;
    GLenum        alpha_func;
    GLfloat       alpha_ref;

    /* ---- Fog (§3.10) ---- */
    GLboolean     fog;
    GLenum        fog_mode;
    GLfloat       fog_density, fog_start, fog_end;
    gl_color_t    fog_color;

    /* ---- Vertex arrays and buffer objects (§2.8), phase G7 ---- */
    gl_array_t    array_vertex, array_color, array_normal, array_texcoord;
    gl_buffer_t   buffers[GL_MAX_BUFFERS_IMPL];
    GLuint        next_buffer_name;
    GLuint        buffer_binding_array;    /* GL_ARRAY_BUFFER         */
    GLuint        buffer_binding_element;  /* GL_ELEMENT_ARRAY_BUFFER */

    /* ---- Display lists (§5.4) ---- */
    gl_list_t     lists[GL_MAX_LISTS_IMPL];
    GLuint        next_list_name;
    int           list_compiling;          /* index into lists[], or -1 */
    GLenum        list_mode;               /* COMPILE or COMPILE_AND_EXECUTE */

    /* ---- glPushAttrib / glPopAttrib (§6.1.2) ----
     * Each entry stores a full copy of the attribute groups this
     * implementation tracks, plus the mask it was pushed with so the pop
     * restores exactly the groups that were saved. */
    struct gl_attrib_entry {
        GLbitfield mask;
        gl_color_t clear_color;
        GLfloat    clear_depth;
        GLint      viewport_x, viewport_y;
        GLsizei    viewport_w, viewport_h;
        GLboolean  depth_test, depth_mask;
        GLenum     depth_func;
        GLboolean  cull_face;
        GLenum     cull_mode, front_face, shade_model, polygon_mode;
        GLboolean  scissor_test;
        GLint      scissor_x, scissor_y;
        GLsizei    scissor_w, scissor_h;
    } attrib_stack[GL_ATTRIB_STACK_DEPTH];
    int attrib_top;          /* number of entries currently pushed */
};

/* ---- Internal helpers shared across libgl translation units ---- */

/* The context GL entry points act on, or NULL.  Defined in auraglx.c. */
extern struct aglx_context *gl_current_ctx;

/* Record an error, honouring the "first error wins" rule.  Safe with a NULL
 * context (the error is then dropped, since there is nowhere to store it). */
void gl_set_error(GLenum error);

/* Convenience used by nearly every entry point: fetch the current context or
 * flag GL_INVALID_OPERATION and return NULL.  Calling GL without a current
 * context is an application bug, and this makes it visible via glGetError()
 * instead of silently doing nothing. */
struct aglx_context *gl_ctx_or_error(void);

/* Clamp a float to [0,1] — GLclampf semantics (§2.1.2). */
static inline GLfloat gl_clampf(GLfloat v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Pack normalised RGBA into the framebuffer's 0x00RRGGBB layout.
 * Rounding uses +0.5 so that 1.0f maps to 255 rather than 254. */
static inline gl_pixel_t gl_pack_color(gl_color_t c) {
    uint32_t r = (uint32_t)(gl_clampf(c.r) * 255.0f + 0.5f);
    uint32_t g = (uint32_t)(gl_clampf(c.g) * 255.0f + 0.5f);
    uint32_t b = (uint32_t)(gl_clampf(c.b) * 255.0f + 0.5f);
    return (r << 16) | (g << 8) | b;
}

#endif /* AURALITE_GL_CONTEXT_H */

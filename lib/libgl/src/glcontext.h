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
/* Per-unit texture-matrix stack.  Named _IMPL so it does not collide with
 * the GL query token GL_TEXTURE_STACK_DEPTH (0x0BA5) in gl.h.  Depth 2 is
 * GL2 L3's budget: enough for a push/pop around a slide, not a nest. */
#define GL_TEXTURE_STACK_DEPTH_IMPL  2

/* glPushAttrib stack depth.  GL 1.1 requires at least 16. */
#define GL_ATTRIB_STACK_DEPTH      16

/* GL 1.1 requires at least 8 lights (§2.14.1). */
#define GL_MAX_LIGHTS_IMPL         8

/* Texture objects tracked per context.  GL has no fixed limit; this is an
 * implementation choice sized for the demos plus room to spare. */
#define GL_MAX_TEXTURES_IMPL       64

/* ---- Phase G10 limits ----
 *
 * Mipmap levels.  13 covers everything up to 4096x4096 (level 12 is 1x1),
 * which is already beyond what a software rasterizer on this OS will draw.
 */
#define GL_MAX_MIPMAP_LEVELS       13

/* Texture units for multitexturing (GL 1.3 §3.8.10).  Four is GL2 L3's
 * budget: the G10 minimum of two plus room for a lightmap and a detail
 * layer, without the 8-unit blow-up that put a 130 KB context on the C
 * stack.  Every extra unit is a sampler call per fragment. */
#define GL_MAX_TEXTURE_UNITS_IMPL  4

/* Cube map faces, in the order the GL_TEXTURE_CUBE_MAP_* enums are numbered:
 * +X, -X, +Y, -Y, +Z, -Z. */
#define GL_CUBE_FACES              6

/* ---- Phase G11c limits (the shader pipeline) ----
 *
 * GLSL ES 1.0 requires at least 8 varying vec4s, 8 vertex attributes, 128
 * vertex uniform vec4s and 16 fragment ones.  These are the minimums, which
 * is what portable shaders are written against; raising them costs memory in
 * every vertex (varyings) and in every context (uniforms) for capability
 * nothing portable uses.
 */
#define GL_MAX_VARYING_VEC4S       8
#define GL_MAX_VARYING_FLOATS      (GL_MAX_VARYING_VEC4S * 4)
#define GL_MAX_VERTEX_ATTRIBS_IMPL 8
#define GL_MAX_UNIFORMS_IMPL       64   /* named uniforms per program      */
#define GL_MAX_UNIFORM_FLOATS      1024 /* total storage per program       */
#define GL_MAX_SHADERS_IMPL        16
#define GL_MAX_PROGRAMS_IMPL       8

/* ---- Phase G12 limits ----
 *
 * Framebuffer and renderbuffer objects tracked per context.  Sized like the
 * texture and buffer tables: generous for the demos, bounded so the context
 * stays a fixed-size allocation.
 */
#define GL_MAX_FRAMEBUFFERS_IMPL   32
#define GL_MAX_RENDERBUFFERS_IMPL  32

/* Colour attachment points per framebuffer.  One is all the fixed-function
 * pipeline can write: there are no draw buffers and no gl_FragData, so a
 * second attachment would have nothing to put in it.  The constant exists so
 * that the day a shader path lands (G11) the loops are already written. */
#define GL_MAX_COLOR_ATTACHMENTS_IMPL 1

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
/* One stored image: a single mipmap level of a single cube face, or the whole
 * volume of a 3D texture.  `depth` is 1 for 2D and cube-map images. */
typedef struct {
    uint32_t  *texels;           /* width*height*depth, 0xAARRGGBB, or NULL */
    GLsizei    width, height, depth;
} gl_teximage_t;

/* ---- Texture object, extended in G10 ----
 *
 * A single object type covers 2D, 3D and cube-map textures rather than three
 * parallel types.  A cube map is six face chains; 2D and 3D use face 0 only.
 * That costs five unused pointers per non-cube texture (40 bytes) and saves
 * the sampler from having to know which of three structs it was handed.
 *
 * `levels` counts the mipmap levels actually populated on face 0, starting at
 * level 0.  A chain is COMPLETE when it runs all the way down to 1x1; the
 * mipmap filters silently fall back to the level-0 image on an incomplete
 * chain, which is what the specification requires (§3.8.10) and what stops a
 * half-uploaded texture from rendering as garbage.
 */
typedef struct {
    GLuint         name;         /* 0 means the slot is free                */
    int            used;
    GLenum         target;       /* GL_TEXTURE_2D / _3D / _CUBE_MAP, 0 = untyped */
    gl_teximage_t  img[GL_CUBE_FACES][GL_MAX_MIPMAP_LEVELS];
    int            levels;       /* populated levels on face 0 (>=1 when sized) */
    GLenum         min_filter, mag_filter;
    GLenum         wrap_s, wrap_t, wrap_r;
    gl_color_t     border_color; /* used by GL_CLAMP_TO_BORDER              */
    GLint          base_level, max_level;
} gl_texture_t;

/* ---- Texture unit (GL 1.3 §3.8.10) ----
 *
 * Each unit has its own enables, its own bindings for each target and its own
 * environment.  The units are combined in order: unit 0's result becomes the
 * incoming fragment colour for unit 1, exactly as the fixed-function pipeline
 * specifies.
 */
typedef struct {
    GLboolean  enabled_2d, enabled_3d, enabled_cube;
    GLuint     binding_2d, binding_3d, binding_cube;
    GLenum     env_mode;         /* MODULATE / REPLACE / DECAL / BLEND / COMBINE */
    gl_color_t env_color;
    /* GL 1.3 COMBINE (GL2 L3).  Defaults match §3.8.13 so COMBINE+MODULATE
     * is pixel-identical to the GL 1.1 MODULATE path (D3). */
    GLenum     combine_rgb, combine_alpha;
    GLenum     src_rgb[3], src_alpha[3];
    GLenum     operand_rgb[3], operand_alpha[3];
    GLfloat    rgb_scale, alpha_scale;
    /* Per-unit texture matrix (GL 1.3).  Identity default, so existing UVs
     * are unchanged until an application asks (D3). */
    glm_mat4   texture_matrix[GL_TEXTURE_STACK_DEPTH_IMPL];
    int        texture_matrix_top;
} gl_texunit_t;

/* ---- Renderbuffer object (§4.4.2) ----
 *
 * An off-screen surface that is not a texture.  Depth attachments are the
 * common use: rendering colour into a texture still needs somewhere to put
 * depth, and a renderbuffer is that somewhere without pretending to be
 * samplable.
 *
 * Colour renderbuffers store packed gl_pixel_t, depth ones store float,
 * stencil ones store uint8_t, and exactly one of the three pointers is
 * non-NULL.  Keeping them as separate members rather than a union makes the
 * "which is it" question answerable by looking, which matters in the
 * attachment-completeness checks.
 */
typedef struct {
    GLuint      name;
    int         used;
    GLenum      format;          /* GL_RGBA8, GL_DEPTH_COMPONENT24, ...     */
    GLsizei     width, height;
    gl_pixel_t *color;           /* non-NULL for a colour renderbuffer      */
    float      *depth;           /* non-NULL for a depth renderbuffer       */
    uint8_t    *stencil;         /* non-NULL for a stencil renderbuffer     */
} gl_renderbuffer_t;

/* ---- Framebuffer object (§4.4) ----
 *
 * An FBO owns no storage of its own: it is a set of REFERENCES to images that
 * live in textures or renderbuffers.  That is the whole point of the object,
 * and it is why deleting an attached texture has to be handled explicitly --
 * the FBO would otherwise keep a dangling reference.
 *
 * An attachment is described by (kind, name, and for a texture the target,
 * face and level).  Resolving it to an actual pixel pointer happens at bind
 * time, in gl_fbo_apply(), not here: a texture can be re-uploaded at a new
 * size while attached, and the resolution must see the current state.
 */
typedef enum {
    GL_ATTACH_NONE = 0,
    GL_ATTACH_TEXTURE,
    GL_ATTACH_RENDERBUFFER
} gl_attach_kind_t;

typedef struct {
    gl_attach_kind_t kind;
    GLuint           name;       /* texture or renderbuffer name            */
    GLenum           textarget;  /* GL_TEXTURE_2D or a cube face target     */
    GLint            level;      /* mipmap level, textures only             */
} gl_attachment_t;

typedef struct {
    GLuint          name;
    int             used;
    gl_attachment_t color[GL_MAX_COLOR_ATTACHMENTS_IMPL];
    gl_attachment_t depth;
    gl_attachment_t stencil;
} gl_framebuffer_t;

/* ---- Shader and program objects (GL ES 2.0, phase G11c) ----
 *
 * A shader owns its source and, once compiled, its glsl_unit_t.  A program
 * references up to two shaders and owns the LINKED state: the uniform store,
 * and the tables mapping attribute and varying names to slots.
 *
 * Those tables are built at link time and not before, which is what makes
 * glGetUniformLocation() meaningful: a location is an index into this
 * program's store, not a name lookup repeated per draw call.  The interpreter
 * still asks by name through glsl_env_t, so the table is what turns that back
 * into an index once per variable per frame rather than per fragment.
 */
typedef struct {
    GLuint   name;
    int      used;
    GLenum   type;               /* GL_VERTEX_SHADER / GL_FRAGMENT_SHADER  */
    char    *source;             /* owned copy, or NULL                    */
    void    *unit;               /* glsl_unit_t*, opaque here              */
    GLboolean compiled;
    char    *log;                /* owned copy of the info log             */
} gl_shader_t;

/* One named uniform in a linked program. */
typedef struct {
    char    name[64];
    int     offset;              /* into the program's float store         */
    int     size;                /* components                             */
    GLenum  type;                /* GL_FLOAT, GL_FLOAT_VEC3, GL_SAMPLER_2D */
    int     is_sampler;
} gl_uniform_t;

/* One varying, matched between the two shaders at link time. */
typedef struct {
    char    name[64];
    int     offset;              /* into gl_vertex_t::varying              */
    int     size;
} gl_varying_t;

/* One generic vertex attribute the vertex shader declared. */
typedef struct {
    char    name[64];
    int     location;            /* the index glVertexAttribPointer uses   */
    int     size;
} gl_attrib_info_t;

typedef struct {
    GLuint   name;
    int      used;
    GLuint   vertex_shader;      /* attached names, 0 when none            */
    GLuint   fragment_shader;
    GLboolean linked;
    /* GL2 phase L5: 1 when the linked fragment shader contains anything that
     * makes shade-then-depth the only correct order (a `discard` today; a
     * `gl_FragDepth` store once the language grows one).  Set by every
     * glLinkProgram from the fragment AST (glsl_fragment_may_kill_early_z).
     * Sits in the alignment padding after `linked`, so the context does not
     * grow. */
    int      may_kill_early_z;
    char    *log;

    /* Linked state.  Rebuilt from scratch by every glLinkProgram, so a
     * relink cannot leave a stale location behind. */
    gl_uniform_t  uniforms[GL_MAX_UNIFORMS_IMPL];
    int           uniform_count;
    float         uniform_data[GL_MAX_UNIFORM_FLOATS];
    int           uniform_used;

    gl_varying_t  varyings[GL_MAX_VARYING_VEC4S * 4];
    int           varying_count;
    int           varying_floats;

    gl_attrib_info_t attribs[GL_MAX_VERTEX_ATTRIBS_IMPL];
    int           attrib_count;
} gl_program_t;

/* A generic vertex attribute array (glVertexAttribPointer).  Deliberately a
 * separate table from the fixed-function arrays: a program addresses them by
 * NUMBER and the fixed-function pipeline by role, and conflating the two is
 * how an implementation ends up feeding a shader the colour array because it
 * happened to be at index 3. */
typedef struct {
    GLboolean    enabled;
    GLint        size;
    GLenum       type;
    GLboolean    normalized;
    GLsizei      stride;
    const void  *ptr;
    GLuint       buffer;
    GLfloat      generic[4];     /* the value when the array is disabled   */
} gl_vertex_attrib_t;

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
    /* ---- Render target: where the rasterizer writes ----
     *
     * These are the ONLY things the rasterizer knows about.  Binding a
     * framebuffer object redirects them at the attached images; binding
     * framebuffer 0 restores the window buffers saved in win_* below.  That
     * is what makes phase G12 small: not one line of glraster.c had to change
     * to gain render-to-texture.
     */
    int         width;          /* buffer width  in pixels */
    int         height;         /* buffer height in pixels */
    gl_pixel_t *color;          /* width*height, packed XRGB8888, never NULL */
    float      *depth;          /* width*height, or NULL when there is none  */
    uint8_t    *stencil;        /* width*height, or NULL when there is none  */

    /* ---- Row order of the current target ----
     *
     * GL window coordinates have a BOTTOM-left origin, and the two kinds of
     * target disagree about where row 0 lives:
     *
     *   - the window's framebuffer stores row 0 at the TOP, so writing a GL
     *     pixel requires flipping y;
     *   - a texture stores row 0 at the BOTTOM (see gltexture.c), which is
     *     already GL's own convention, so no flip applies.
     *
     * Getting this wrong renders correctly into the window and upside-down
     * into a texture -- which is exactly what happened first, and is the one
     * genuine subtlety in phase G12.  gl_fb_row() consults this flag. */
    int         target_flip_y;  /* 1 = row 0 is the top (the window)         */

    /* The window-system buffers, owned by the context and always allocated.
     * `color`/`depth` above point HERE whenever framebuffer 0 is bound, and
     * elsewhere when an FBO is.  aglxSwapBuffers() always presents win_color,
     * so presenting while an FBO is bound shows the window's content rather
     * than whatever off-screen surface happens to be current -- which is what
     * the specification means by framebuffer 0 being the window. */
    int         win_width, win_height;
    gl_pixel_t *win_color;
    float      *win_depth;
    uint8_t    *win_stencil;

    /* ---- Window binding ---- */
    int         wid;            /* AuraGUI window id */
    uint32_t    flags;          /* AGLX_* creation flags */

    /* ---- Clear state (§4.2.3) ---- */
    gl_color_t  clear_color;
    GLfloat     clear_depth;    /* clamped to [0,1] */
    GLint       clear_stencil;  /* masked to 8 bits when used */

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

    /* ---- Stencil buffer (§4.1.4), GL2 L1 ---- */
    GLboolean   stencil_test;
    GLenum      stencil_func;
    GLint       stencil_ref;
    GLuint      stencil_valuemask;
    GLenum      stencil_fail, stencil_zfail, stencil_zpass;
    GLuint      stencil_writemask;

    /* ---- Lighting (§2.14), phase G5 ---- */
    GLboolean     lighting;
    gl_light_t    lights[GL_MAX_LIGHTS_IMPL];
    gl_material_t material_front, material_back;
    gl_color_t    light_model_ambient;
    GLboolean     light_model_two_side;
    GLboolean     normalize;
    GLboolean     color_material;
    GLenum        color_material_face, color_material_mode;

    /* ---- Texturing (§3.8), phase G6, extended to GL 1.3 in G10 ----
     *
     * Texture OBJECTS are shared by the whole context; the per-unit state
     * (enables, bindings, environment) lives in `texunits`.  `active_texture`
     * selects which unit the glTexImage/glTexParameter/glTexEnv calls act on,
     * and `client_active_texture` selects which unit glTexCoordPointer acts
     * on — GL keeps those two selectors separate, and conflating them is a
     * classic multitexturing bug. */
    gl_texture_t  textures[GL_MAX_TEXTURES_IMPL];
    GLuint        next_texture_name;       /* monotonic name allocator       */
    gl_texunit_t  texunits[GL_MAX_TEXTURE_UNITS_IMPL];
    int           active_texture;          /* 0..GL_MAX_TEXTURE_UNITS_IMPL-1 */
    int           client_active_texture;

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
    gl_array_t    array_vertex, array_color, array_normal;
    /* One texture-coordinate array per unit (GL 1.3): glClientActiveTexture
     * selects which one glTexCoordPointer and glEnableClientState affect. */
    gl_array_t    array_texcoord[GL_MAX_TEXTURE_UNITS_IMPL];
    gl_buffer_t   buffers[GL_MAX_BUFFERS_IMPL];
    GLuint        next_buffer_name;
    GLuint        buffer_binding_array;    /* GL_ARRAY_BUFFER         */
    GLuint        buffer_binding_element;  /* GL_ELEMENT_ARRAY_BUFFER */

    /* ---- Display lists (§5.4) ---- */
    gl_list_t     lists[GL_MAX_LISTS_IMPL];
    GLuint        next_list_name;
    int           list_compiling;          /* index into lists[], or -1 */
    GLenum        list_mode;               /* COMPILE or COMPILE_AND_EXECUTE */

    /* ---- Shaders and programs (phase G11c) ---- */
    gl_shader_t   shaders[GL_MAX_SHADERS_IMPL];
    gl_program_t  programs[GL_MAX_PROGRAMS_IMPL];
    GLuint        next_shader_name;
    GLuint        next_program_name;
    GLuint        program_binding;         /* 0 = fixed function          */
    gl_vertex_attrib_t vattrib[GL_MAX_VERTEX_ATTRIBS_IMPL];

    /* ---- Framebuffer objects (§4.4), phase G12 ---- */
    gl_framebuffer_t  framebuffers[GL_MAX_FRAMEBUFFERS_IMPL];
    gl_renderbuffer_t renderbuffers[GL_MAX_RENDERBUFFERS_IMPL];
    GLuint            next_framebuffer_name;
    GLuint            next_renderbuffer_name;
    GLuint            framebuffer_binding;   /* draw FBO; 0 = the window    */
    GLuint            read_framebuffer_binding; /* read FBO; GL_FRAMEBUFFER sets both */
    GLuint            renderbuffer_binding;

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

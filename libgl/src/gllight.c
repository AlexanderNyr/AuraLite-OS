/* libgl/src/gllight.c — lighting and materials (GL 1.1 §2.14).
 *
 * Phase G5 of GL_PLAN.md.
 *
 * WHERE LIGHTING HAPPENS
 *
 * Per VERTEX, in EYE space, at the moment glVertex is called — not per
 * fragment.  That is what GL 1.1 specifies: the lit colour is computed for
 * each vertex and then interpolated across the primitive by the rasterizer
 * (Gouraud shading).  Per-pixel lighting only arrives with programmable
 * shaders, which is phase G11 territory.
 *
 * Eye space is the right space for this because the viewer is at the origin
 * there, so the eye vector is simply -position, and light positions can be
 * stored once instead of being re-transformed per vertex.
 *
 * THE LIGHTING EQUATION (§2.14.1)
 *
 *   colour = emission
 *          + ambient_scene * ambient_material
 *          + sum over enabled lights of
 *              attenuation * spotlight * (
 *                  ambient_light  * ambient_material
 *                + max(N.L, 0)    * diffuse_light  * diffuse_material
 *                + f * (N.H)^shininess * specular_light * specular_material )
 *
 * where f is 1 when N.L > 0 and 0 otherwise — without it, a surface facing
 * away from the light would still show a specular highlight.
 *
 * H is the half-vector, normalize(L + eye_direction), which is what makes this
 * Blinn-Phong rather than Phong.  GL 1.1 specifies exactly this form.
 */

#include <math.h>

#include "GL/gl.h"
#include "GL/glmath.h"
#include "glcontext.h"
#include "glvertex.h"

/* ============================================================================
 * Defaults (§2.14, tables 2.7 and 2.8)
 * ==========================================================================*/

void gl_lighting_set_defaults(struct aglx_context *ctx) {
    ctx->lighting             = GL_FALSE;
    ctx->light_model_two_side = GL_FALSE;
    ctx->normalize            = GL_FALSE;
    ctx->color_material       = GL_FALSE;
    ctx->color_material_face  = GL_FRONT_AND_BACK;
    ctx->color_material_mode  = GL_AMBIENT_AND_DIFFUSE;

    /* Scene ambient is a dim grey, not black, so a lit scene with no lights
     * enabled is still faintly visible rather than pitch dark. */
    ctx->light_model_ambient.r = 0.2f;
    ctx->light_model_ambient.g = 0.2f;
    ctx->light_model_ambient.b = 0.2f;
    ctx->light_model_ambient.a = 1.0f;

    for (int i = 0; i < GL_MAX_LIGHTS_IMPL; i++) {
        gl_light_t *l = &ctx->lights[i];
        l->enabled = GL_FALSE;

        /* Default position is (0,0,1,0): a directional light shining from the
         * viewer towards -z. */
        l->position = glm_vec4_make(0.0f, 0.0f, 1.0f, 0.0f);

        l->ambient.r = l->ambient.g = l->ambient.b = 0.0f;
        l->ambient.a = 1.0f;

        /* GL_LIGHT0 defaults to white diffuse/specular; all other lights
         * default to black, so enabling GL_LIGHT1 alone changes nothing until
         * its colours are set.  This asymmetry is in the specification. */
        GLfloat d = (i == 0) ? 1.0f : 0.0f;
        l->diffuse.r  = l->diffuse.g  = l->diffuse.b  = d;
        l->diffuse.a  = 1.0f;
        l->specular.r = l->specular.g = l->specular.b = d;
        l->specular.a = 1.0f;

        l->constant_att  = 1.0f;
        l->linear_att    = 0.0f;
        l->quadratic_att = 0.0f;

        l->spot_direction = glm_vec3_make(0.0f, 0.0f, -1.0f);
        l->spot_exponent  = 0.0f;
        l->spot_cutoff    = 180.0f;     /* 180 == not a spotlight */
    }

    gl_material_t *m[2] = { &ctx->material_front, &ctx->material_back };
    for (int k = 0; k < 2; k++) {
        m[k]->ambient.r = m[k]->ambient.g = m[k]->ambient.b = 0.2f;
        m[k]->ambient.a = 1.0f;
        m[k]->diffuse.r = m[k]->diffuse.g = m[k]->diffuse.b = 0.8f;
        m[k]->diffuse.a = 1.0f;
        m[k]->specular.r = m[k]->specular.g = m[k]->specular.b = 0.0f;
        m[k]->specular.a = 1.0f;
        m[k]->emission.r = m[k]->emission.g = m[k]->emission.b = 0.0f;
        m[k]->emission.a = 1.0f;
        m[k]->shininess = 0.0f;
    }
}

/* ============================================================================
 * The lighting computation
 * ==========================================================================*/

static gl_color_t color_mul(gl_color_t a, gl_color_t b) {
    gl_color_t r;
    r.r = a.r * b.r; r.g = a.g * b.g; r.b = a.b * b.b; r.a = a.a * b.a;
    return r;
}

static void color_add_scaled(gl_color_t *acc, gl_color_t c, GLfloat s) {
    acc->r += c.r * s;
    acc->g += c.g * s;
    acc->b += c.b * s;
}

/* Light one vertex.  `eye_pos` and `normal` are in eye coordinates; the normal
 * is assumed already transformed but not necessarily unit length. */
gl_color_t gl_light_vertex(struct aglx_context *ctx,
                           glm_vec3 eye_pos, glm_vec3 normal,
                           gl_color_t vertex_color, int back_face) {
    const gl_material_t *mat = back_face ? &ctx->material_back
                                         : &ctx->material_front;

    /* GL_COLOR_MATERIAL makes glColor drive a material component, which is how
     * applications get per-vertex colours while lighting is on (§2.14.3). */
    gl_color_t amb_mat = mat->ambient;
    gl_color_t dif_mat = mat->diffuse;
    gl_color_t spc_mat = mat->specular;
    gl_color_t emi_mat = mat->emission;

    if (ctx->color_material) {
        switch (ctx->color_material_mode) {
        case GL_AMBIENT:              amb_mat = vertex_color; break;
        case GL_DIFFUSE:              dif_mat = vertex_color; break;
        case GL_SPECULAR:             spc_mat = vertex_color; break;
        case GL_EMISSION:             emi_mat = vertex_color; break;
        case GL_AMBIENT_AND_DIFFUSE:
        default:
            amb_mat = vertex_color;
            dif_mat = vertex_color;
            break;
        }
    }

    /* A back face is lit with its normal reversed, otherwise every back face
     * would be dark regardless of where the light is (§2.14.1). */
    glm_vec3 N = normal;
    if (back_face) N = glm_vec3_scale(N, -1.0f);

    /* NOT normalised here.  GL only rescales the normal when GL_NORMALIZE (or
     * GL_RESCALE_NORMAL) is enabled, and that is applied earlier, in the
     * vertex stage.  Normalising unconditionally here would make GL_NORMALIZE
     * a no-op and silently hide the "scaled modelview makes lighting wrong"
     * behaviour that applications must opt out of themselves — which is
     * precisely the bug this comment exists to prevent reintroducing. */

    /* In eye space the viewer sits at the origin, so the direction towards it
     * is simply -position. */
    glm_vec3 V = glm_vec3_normalize(glm_vec3_scale(eye_pos, -1.0f));

    gl_color_t out;
    out.r = emi_mat.r + ctx->light_model_ambient.r * amb_mat.r;
    out.g = emi_mat.g + ctx->light_model_ambient.g * amb_mat.g;
    out.b = emi_mat.b + ctx->light_model_ambient.b * amb_mat.b;
    /* Alpha comes from the diffuse material alone (§2.14.1). */
    out.a = dif_mat.a;

    for (int i = 0; i < GL_MAX_LIGHTS_IMPL; i++) {
        const gl_light_t *l = &ctx->lights[i];
        if (!l->enabled) continue;

        glm_vec3 L;
        GLfloat attenuation = 1.0f;

        if (l->position.w == 0.0f) {
            /* Directional: position IS the direction towards the light, and
             * distance is infinite so there is no attenuation. */
            L = glm_vec3_normalize(glm_vec3_make(l->position.x,
                                                 l->position.y,
                                                 l->position.z));
        } else {
            glm_vec3 lp = glm_vec3_make(l->position.x / l->position.w,
                                        l->position.y / l->position.w,
                                        l->position.z / l->position.w);
            glm_vec3 d = glm_vec3_sub(lp, eye_pos);
            GLfloat dist = glm_vec3_length(d);
            L = (dist > 1e-20f) ? glm_vec3_scale(d, 1.0f / dist) : d;

            GLfloat denom = l->constant_att
                          + l->linear_att * dist
                          + l->quadratic_att * dist * dist;
            attenuation = (denom > 1e-20f) ? (1.0f / denom) : 1.0f;
        }

        /* Spotlight cone (§2.14.1).  cutoff == 180 disables the test. */
        if (l->spot_cutoff != 180.0f) {
            glm_vec3 sd = glm_vec3_normalize(l->spot_direction);
            /* Angle between the spot axis and the direction to the fragment,
             * which is -L. */
            GLfloat cos_angle = glm_vec3_dot(glm_vec3_scale(L, -1.0f), sd);
            GLfloat cos_cutoff = cosf(GLM_DEG2RAD(l->spot_cutoff));
            if (cos_angle < cos_cutoff || cos_angle <= 0.0f) {
                continue;                    /* outside the cone */
            }
            attenuation *= powf(cos_angle, l->spot_exponent);
        }

        if (attenuation <= 0.0f) continue;

        /* Ambient term is unaffected by geometry. */
        gl_color_t amb = color_mul(l->ambient, amb_mat);
        color_add_scaled(&out, amb, attenuation);

        GLfloat ndotl = glm_vec3_dot(N, L);
        if (ndotl > 0.0f) {
            gl_color_t dif = color_mul(l->diffuse, dif_mat);
            color_add_scaled(&out, dif, attenuation * ndotl);

            /* Specular, guarded by ndotl > 0: a surface facing away from the
             * light must not show a highlight. */
            if (mat->shininess > 0.0f) {
                glm_vec3 H = glm_vec3_normalize(glm_vec3_add(L, V));
                GLfloat ndoth = glm_vec3_dot(N, H);
                if (ndoth > 0.0f) {
                    GLfloat spec = powf(ndoth, mat->shininess);
                    gl_color_t sc = color_mul(l->specular, spc_mat);
                    color_add_scaled(&out, sc, attenuation * spec);
                }
            }
        }
    }

    /* Lit colours are clamped to [0,1] before being used (§2.14.1). */
    out.r = gl_clampf(out.r);
    out.g = gl_clampf(out.g);
    out.b = gl_clampf(out.b);
    out.a = gl_clampf(out.a);
    return out;
}

/* ============================================================================
 * Entry points
 * ==========================================================================*/

/* Resolve a GL_LIGHTi enum to its slot, or NULL when out of range. */
static gl_light_t *light_slot(struct aglx_context *ctx, GLenum light) {
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + GL_MAX_LIGHTS_IMPL) {
        gl_set_error(GL_INVALID_ENUM);
        return (gl_light_t *)0;
    }
    return &ctx->lights[light - GL_LIGHT0];
}

void glLightfv(GLenum light, GLenum pname, const GLfloat *params) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!params) { gl_set_error(GL_INVALID_VALUE); return; }

    gl_light_t *l = light_slot(ctx, light);
    if (!l) return;

    switch (pname) {
    case GL_POSITION: {
        /* The position is transformed by the CURRENT MODELVIEW matrix and
         * stored in eye coordinates (§2.14.1).  This is why setting a light
         * before or after the camera transform gives different results, and
         * why applications must re-issue glLightfv when the camera moves if
         * they want the light fixed in world space. */
        glm_vec4 p = glm_vec4_make(params[0], params[1], params[2], params[3]);
        l->position = glm_mat4_transform4(ctx->modelview[ctx->modelview_top], p);
        break;
    }
    case GL_AMBIENT:
        l->ambient.r = params[0]; l->ambient.g = params[1];
        l->ambient.b = params[2]; l->ambient.a = params[3];
        break;
    case GL_DIFFUSE:
        l->diffuse.r = params[0]; l->diffuse.g = params[1];
        l->diffuse.b = params[2]; l->diffuse.a = params[3];
        break;
    case GL_SPECULAR:
        l->specular.r = params[0]; l->specular.g = params[1];
        l->specular.b = params[2]; l->specular.a = params[3];
        break;
    case GL_SPOT_DIRECTION: {
        /* Directions transform by the MODELVIEW too, but as a direction: the
         * translation must not apply. */
        glm_vec3 d = glm_vec3_make(params[0], params[1], params[2]);
        l->spot_direction =
            glm_mat4_transform_dir(ctx->modelview[ctx->modelview_top], d);
        break;
    }
    case GL_SPOT_EXPONENT:
        if (params[0] < 0.0f || params[0] > 128.0f) {
            gl_set_error(GL_INVALID_VALUE); return;
        }
        l->spot_exponent = params[0];
        break;
    case GL_SPOT_CUTOFF:
        if (!((params[0] >= 0.0f && params[0] <= 90.0f) || params[0] == 180.0f)) {
            gl_set_error(GL_INVALID_VALUE); return;
        }
        l->spot_cutoff = params[0];
        break;
    case GL_CONSTANT_ATTENUATION:
        if (params[0] < 0.0f) { gl_set_error(GL_INVALID_VALUE); return; }
        l->constant_att = params[0];
        break;
    case GL_LINEAR_ATTENUATION:
        if (params[0] < 0.0f) { gl_set_error(GL_INVALID_VALUE); return; }
        l->linear_att = params[0];
        break;
    case GL_QUADRATIC_ATTENUATION:
        if (params[0] < 0.0f) { gl_set_error(GL_INVALID_VALUE); return; }
        l->quadratic_att = params[0];
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
}

void glLightf(GLenum light, GLenum pname, GLfloat param) {
    /* Only the scalar parameters are valid here (§2.14.1). */
    switch (pname) {
    case GL_SPOT_EXPONENT: case GL_SPOT_CUTOFF:
    case GL_CONSTANT_ATTENUATION: case GL_LINEAR_ATTENUATION:
    case GL_QUADRATIC_ATTENUATION: {
        GLfloat v[4] = { param, 0.0f, 0.0f, 0.0f };
        glLightfv(light, pname, v);
        break;
    }
    default:
        (void)gl_ctx_or_error();
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
}

void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!params) { gl_set_error(GL_INVALID_VALUE); return; }

    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }

    gl_material_t *targets[2];
    int n = 0;
    if (face == GL_FRONT || face == GL_FRONT_AND_BACK)
        targets[n++] = &ctx->material_front;
    if (face == GL_BACK || face == GL_FRONT_AND_BACK)
        targets[n++] = &ctx->material_back;

    for (int i = 0; i < n; i++) {
        gl_material_t *m = targets[i];
        switch (pname) {
        case GL_AMBIENT:
            m->ambient.r = params[0]; m->ambient.g = params[1];
            m->ambient.b = params[2]; m->ambient.a = params[3];
            break;
        case GL_DIFFUSE:
            m->diffuse.r = params[0]; m->diffuse.g = params[1];
            m->diffuse.b = params[2]; m->diffuse.a = params[3];
            break;
        case GL_SPECULAR:
            m->specular.r = params[0]; m->specular.g = params[1];
            m->specular.b = params[2]; m->specular.a = params[3];
            break;
        case GL_EMISSION:
            m->emission.r = params[0]; m->emission.g = params[1];
            m->emission.b = params[2]; m->emission.a = params[3];
            break;
        case GL_AMBIENT_AND_DIFFUSE:
            m->ambient.r = m->diffuse.r = params[0];
            m->ambient.g = m->diffuse.g = params[1];
            m->ambient.b = m->diffuse.b = params[2];
            m->ambient.a = m->diffuse.a = params[3];
            break;
        case GL_SHININESS:
            if (params[0] < 0.0f || params[0] > 128.0f) {
                gl_set_error(GL_INVALID_VALUE);
                return;
            }
            m->shininess = params[0];
            break;
        default:
            gl_set_error(GL_INVALID_ENUM);
            return;
        }
    }
}

void glMaterialf(GLenum face, GLenum pname, GLfloat param) {
    if (pname != GL_SHININESS) {
        (void)gl_ctx_or_error();
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    GLfloat v[4] = { param, 0.0f, 0.0f, 0.0f };
    glMaterialfv(face, pname, v);
}

void glLightModelfv(GLenum pname, const GLfloat *params) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;
    if (!params) { gl_set_error(GL_INVALID_VALUE); return; }

    switch (pname) {
    case GL_LIGHT_MODEL_AMBIENT:
        ctx->light_model_ambient.r = params[0];
        ctx->light_model_ambient.g = params[1];
        ctx->light_model_ambient.b = params[2];
        ctx->light_model_ambient.a = params[3];
        break;
    case GL_LIGHT_MODEL_TWO_SIDE:
        ctx->light_model_two_side = (params[0] != 0.0f) ? GL_TRUE : GL_FALSE;
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        break;
    }
}

void glLightModeli(GLenum pname, GLint param) {
    GLfloat v[4] = { (GLfloat)param, 0.0f, 0.0f, 0.0f };
    glLightModelfv(pname, v);
}

void glColorMaterial(GLenum face, GLenum mode) {
    struct aglx_context *ctx = gl_ctx_or_error();
    if (!ctx) return;

    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    switch (mode) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR:
    case GL_EMISSION: case GL_AMBIENT_AND_DIFFUSE:
        break;
    default:
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    ctx->color_material_face = face;
    ctx->color_material_mode = mode;
}

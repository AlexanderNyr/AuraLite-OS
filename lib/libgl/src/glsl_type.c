/* libgl/src/glsl_type.c — the GLSL ES 1.0 type system.
 *
 * Phase G11a of GL_PLAN.md.
 *
 * WHY THE TYPES ARE SINGLETONS
 *
 * Everything except structs and arrays is one of a fixed set of 17 types, so
 * they live in a static table and are handed out by pointer.  Type equality is
 * then pointer equality in the overwhelmingly common case, and -- more
 * usefully -- there is exactly one `vec3` in the program, so a bug that
 * produces a subtly different one is impossible.
 *
 * Structs and arrays are arena-allocated per declaration, so glsl_type_equal()
 * still compares structurally.
 */

#include <stdio.h>
#include <string.h>

#include "glsl.h"

/* ============================================================================
 * The singleton table
 * ==========================================================================*/

#define T(k, r) { k, r, 0, NULL, {{NULL,NULL}}, 0 }

static const glsl_type_t ty_error       = T(GLSL_TY_ERROR, 0);
static const glsl_type_t ty_void        = T(GLSL_TY_VOID, 0);
static const glsl_type_t ty_bool        = T(GLSL_TY_BOOL, 1);
static const glsl_type_t ty_int         = T(GLSL_TY_INT, 1);
static const glsl_type_t ty_float       = T(GLSL_TY_FLOAT, 1);
static const glsl_type_t ty_vec2        = T(GLSL_TY_VEC, 2);
static const glsl_type_t ty_vec3        = T(GLSL_TY_VEC, 3);
static const glsl_type_t ty_vec4        = T(GLSL_TY_VEC, 4);
static const glsl_type_t ty_ivec2       = T(GLSL_TY_IVEC, 2);
static const glsl_type_t ty_ivec3       = T(GLSL_TY_IVEC, 3);
static const glsl_type_t ty_ivec4       = T(GLSL_TY_IVEC, 4);
static const glsl_type_t ty_bvec2       = T(GLSL_TY_BVEC, 2);
static const glsl_type_t ty_bvec3       = T(GLSL_TY_BVEC, 3);
static const glsl_type_t ty_bvec4       = T(GLSL_TY_BVEC, 4);
static const glsl_type_t ty_mat2        = T(GLSL_TY_MAT, 2);
static const glsl_type_t ty_mat3        = T(GLSL_TY_MAT, 3);
static const glsl_type_t ty_mat4        = T(GLSL_TY_MAT, 4);
static const glsl_type_t ty_sampler2d   = T(GLSL_TY_SAMPLER2D, 1);
static const glsl_type_t ty_samplercube = T(GLSL_TY_SAMPLERCUBE, 1);

#undef T

const glsl_type_t *glsl_type_error(void) { return &ty_error; }

const glsl_type_t *glsl_type_basic(glsl_ty_kind_t kind, int rows) {
    switch (kind) {
    case GLSL_TY_VOID:  return &ty_void;
    case GLSL_TY_BOOL:  return &ty_bool;
    case GLSL_TY_INT:   return &ty_int;
    case GLSL_TY_FLOAT: return &ty_float;
    case GLSL_TY_VEC:
        if (rows == 2) return &ty_vec2;
        if (rows == 3) return &ty_vec3;
        if (rows == 4) return &ty_vec4;
        return &ty_error;
    case GLSL_TY_IVEC:
        if (rows == 2) return &ty_ivec2;
        if (rows == 3) return &ty_ivec3;
        if (rows == 4) return &ty_ivec4;
        return &ty_error;
    case GLSL_TY_BVEC:
        if (rows == 2) return &ty_bvec2;
        if (rows == 3) return &ty_bvec3;
        if (rows == 4) return &ty_bvec4;
        return &ty_error;
    case GLSL_TY_MAT:
        if (rows == 2) return &ty_mat2;
        if (rows == 3) return &ty_mat3;
        if (rows == 4) return &ty_mat4;
        return &ty_error;
    case GLSL_TY_SAMPLER2D:   return &ty_sampler2d;
    case GLSL_TY_SAMPLERCUBE: return &ty_samplercube;
    default:                  return &ty_error;
    }
}

/* ============================================================================
 * Predicates
 * ==========================================================================*/

int glsl_type_is_float_based(const glsl_type_t *t) {
    return t && (t->kind == GLSL_TY_FLOAT || t->kind == GLSL_TY_VEC ||
                 t->kind == GLSL_TY_MAT);
}

int glsl_type_is_int_based(const glsl_type_t *t) {
    return t && (t->kind == GLSL_TY_INT || t->kind == GLSL_TY_IVEC);
}

int glsl_type_is_bool_based(const glsl_type_t *t) {
    return t && (t->kind == GLSL_TY_BOOL || t->kind == GLSL_TY_BVEC);
}

int glsl_type_is_numeric(const glsl_type_t *t) {
    return glsl_type_is_float_based(t) || glsl_type_is_int_based(t);
}

int glsl_type_is_sampler(const glsl_type_t *t) {
    return t && (t->kind == GLSL_TY_SAMPLER2D || t->kind == GLSL_TY_SAMPLERCUBE);
}

int glsl_type_components(const glsl_type_t *t) {
    if (!t) return 0;

    /* An array's component count is its element's, times its length.  Missing
     * this makes `float a[3]` occupy one slot, so a[1] and a[2] alias a[0] --
     * which is exactly the bug the array tests caught, and it is silent:
     * every write lands somewhere legal, just on top of the previous one. */
    if (t->array_len > 0) {
        glsl_type_t elem = *t;
        elem.array_len = 0;
        return glsl_type_components(&elem) * t->array_len;
    }

    switch (t->kind) {
    case GLSL_TY_BOOL: case GLSL_TY_INT: case GLSL_TY_FLOAT:
        return 1;
    case GLSL_TY_VEC: case GLSL_TY_IVEC: case GLSL_TY_BVEC:
        return t->rows;
    case GLSL_TY_MAT:
        return t->rows * t->rows;
    case GLSL_TY_STRUCT: {
        int n = 0;
        for (int i = 0; i < t->field_count; i++) {
            n += glsl_type_components(t->fields[i].type);
        }
        return n;
    }
    default:
        return 0;
    }
}

const glsl_type_t *glsl_type_element(const glsl_type_t *t) {
    if (!t) return glsl_type_error();
    switch (t->kind) {
    case GLSL_TY_VEC:  case GLSL_TY_MAT: return glsl_type_basic(GLSL_TY_FLOAT, 1);
    case GLSL_TY_IVEC: return glsl_type_basic(GLSL_TY_INT, 1);
    case GLSL_TY_BVEC: return glsl_type_basic(GLSL_TY_BOOL, 1);
    default:           return t;
    }
}

int glsl_type_equal(const glsl_type_t *a, const glsl_type_t *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;

    /* An error type unifies with anything.  This is what stops one mistake
     * from producing a diagnostic at every node above it. */
    if (a->kind == GLSL_TY_ERROR || b->kind == GLSL_TY_ERROR) return 1;

    if (a->kind != b->kind) return 0;
    if (a->rows != b->rows) return 0;
    if (a->array_len != b->array_len) return 0;

    if (a->kind == GLSL_TY_STRUCT) {
        /* GLSL structs are NAME-equivalent, not structurally equivalent
         * (§4.1.8): two structs with identical fields but different names are
         * different types, and assigning one to the other is an error. */
        if (!a->struct_name || !b->struct_name) return 0;
        return strcmp(a->struct_name, b->struct_name) == 0;
    }
    return 1;
}

/* ============================================================================
 * Naming, for diagnostics
 * ==========================================================================*/

const char *glsl_type_name(const glsl_type_t *t) {
    static char buf[96];

    if (!t) return "<null>";

    const char *base;
    switch (t->kind) {
    case GLSL_TY_ERROR:  return "<error>";
    case GLSL_TY_VOID:   base = "void";  break;
    case GLSL_TY_BOOL:   base = "bool";  break;
    case GLSL_TY_INT:    base = "int";   break;
    case GLSL_TY_FLOAT:  base = "float"; break;
    case GLSL_TY_VEC:
        base = (t->rows == 2) ? "vec2" : (t->rows == 3) ? "vec3" : "vec4";
        break;
    case GLSL_TY_IVEC:
        base = (t->rows == 2) ? "ivec2" : (t->rows == 3) ? "ivec3" : "ivec4";
        break;
    case GLSL_TY_BVEC:
        base = (t->rows == 2) ? "bvec2" : (t->rows == 3) ? "bvec3" : "bvec4";
        break;
    case GLSL_TY_MAT:
        base = (t->rows == 2) ? "mat2" : (t->rows == 3) ? "mat3" : "mat4";
        break;
    case GLSL_TY_SAMPLER2D:   base = "sampler2D";   break;
    case GLSL_TY_SAMPLERCUBE: base = "samplerCube"; break;
    case GLSL_TY_STRUCT:
        base = t->struct_name ? t->struct_name : "struct";
        break;
    default: base = "<unknown>"; break;
    }

    if (t->array_len > 0) {
        snprintf(buf, sizeof buf, "%s[%d]", base, t->array_len);
        return buf;
    }
    return base;
}

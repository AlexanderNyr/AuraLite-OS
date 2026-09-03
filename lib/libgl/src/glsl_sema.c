/* libgl/src/glsl_sema.c — GLSL ES 1.0 type checker and scope resolution.
 *
 * Phase G11a of GL_PLAN.md.
 *
 * WHAT THIS STAGE DECIDES
 *
 * Every expression node comes out with a type, an is_const flag and an
 * is_lvalue flag.  Phase G11b will execute the tree, and it must be able to do
 * so without re-deriving any of that -- an interpreter that has to work out
 * whether `v.xy` is assignable at run time is an interpreter that will get it
 * wrong somewhere.
 *
 * THE RULES THAT ARE NOT OBVIOUS
 *
 * 1. There are NO implicit conversions in GLSL ES 1.0.  `float f = 1;` is an
 *    error, not a widening.  This surprises everyone coming from C, so the
 *    diagnostic says so explicitly rather than just "type mismatch".
 *
 * 2. Scalar-vector arithmetic is component-wise and legal (`v * 2.0`), but
 *    the scalar must have the vector's element type: `v * 2` is an error
 *    because 2 is an int.
 *
 * 3. `mat * vec` is a linear transform, not a component-wise multiply, and it
 *    is the one binary operator whose result type is neither operand's.
 *
 * 4. Swizzles are ordered sets over one of three disjoint alphabets -- xyzw,
 *    rgba, stpq -- and mixing alphabets is an error.  A swizzle with a
 *    repeated component (`v.xx`) is readable but not assignable.
 *
 * 5. `==` and `!=` work on whole vectors and return a single bool; the
 *    component-wise versions are the built-ins equal()/notEqual().  Relational
 *    operators (`<`) are scalars only.
 *
 * BUILT-INS ARE DECLARED, NOT SPECIAL-CASED
 *
 * gl_Position, gl_FragColor, texture2D and friends are entered into the global
 * scope as ordinary symbols before checking starts.  A built-in then resolves
 * through the same path as a user name, so shadowing, wrong-argument-count and
 * wrong-type diagnostics all come out identical -- and G11b can look them up
 * the same way too.
 */

#include <string.h>

#include "glsl.h"

/* ============================================================================
 * Symbols and scopes
 * ==========================================================================*/

typedef enum {
    SYM_VAR = 0,
    SYM_FUNC
} sym_kind_t;

typedef struct {
    sym_kind_t         kind;
    const char        *name;
    const glsl_type_t *type;         /* variable type, or function return  */
    glsl_qualifier_t   qual;
    int                scope;
    int                is_builtin;
    int                readonly;     /* const, uniform, attribute, varying-in */

    /* Function payload. */
    int                param_count;
    const glsl_type_t *param_types[GLSL_MAX_ARGS];
    glsl_qualifier_t   param_quals[GLSL_MAX_ARGS];
    int                is_generic;   /* built-in accepting any float type  */
    int                defined;      /* a body was seen, not just a prototype */
} symbol_t;

typedef struct {
    glsl_unit_t *u;
    symbol_t     syms[GLSL_MAX_SYMBOLS];
    int          sym_count;
    int          scope;

    /* Context for statement checks. */
    const glsl_type_t *return_type;
    int                loop_depth;
    int                in_function;
    int                found_main;
    int                writes_position;
} sema_t;

static void scope_push(sema_t *s) { s->scope++; }

static void scope_pop(sema_t *s) {
    while (s->sym_count > 0 && s->syms[s->sym_count - 1].scope >= s->scope) {
        s->sym_count--;
    }
    s->scope--;
}

/* Innermost declaration of `name`, or NULL.  Searching backwards is what makes
 * an inner scope shadow an outer one. */
static symbol_t *lookup(sema_t *s, const char *name) {
    if (!name) return NULL;
    for (int i = s->sym_count - 1; i >= 0; i--) {
        if (strcmp(s->syms[i].name, name) == 0) return &s->syms[i];
    }
    return NULL;
}

/* Was `name` already declared in the CURRENT scope?  Redeclaring in an inner
 * scope is legal shadowing; redeclaring in the same one is an error. */
static symbol_t *lookup_local(sema_t *s, const char *name) {
    if (!name) return NULL;
    for (int i = s->sym_count - 1; i >= 0; i--) {
        if (s->syms[i].scope < s->scope) break;
        if (strcmp(s->syms[i].name, name) == 0) return &s->syms[i];
    }
    return NULL;
}

static symbol_t *declare(sema_t *s, const char *name) {
    if (!name) return NULL;
    if (s->sym_count >= GLSL_MAX_SYMBOLS) return NULL;
    symbol_t *sym = &s->syms[s->sym_count++];
    memset(sym, 0, sizeof *sym);
    sym->name  = name;
    sym->scope = s->scope;
    return sym;
}

/* ============================================================================
 * Built-in declarations
 * ==========================================================================*/

static const glsl_type_t *T_int(void)   { return glsl_type_basic(GLSL_TY_INT, 1); }
static const glsl_type_t *T_bool(void)  { return glsl_type_basic(GLSL_TY_BOOL, 1); }
static const glsl_type_t *T_float(void) { return glsl_type_basic(GLSL_TY_FLOAT, 1); }
static const glsl_type_t *T_vec(int n)  { return glsl_type_basic(GLSL_TY_VEC, n); }
static const glsl_type_t *T_bvec(int n) { return glsl_type_basic(GLSL_TY_BVEC, n); }
static const glsl_type_t *T_mat(int n)  { return glsl_type_basic(GLSL_TY_MAT, n); }

static void add_var(sema_t *s, const char *name, const glsl_type_t *t,
                    glsl_qualifier_t q, int readonly) {
    symbol_t *sym = declare(s, name);
    if (!sym) return;
    sym->kind = SYM_VAR;
    sym->type = t;
    sym->qual = q;
    sym->is_builtin = 1;
    sym->readonly = readonly;
}

static void add_func(sema_t *s, const char *name, const glsl_type_t *ret,
                     int nargs, const glsl_type_t *a0, const glsl_type_t *a1,
                     const glsl_type_t *a2, int generic) {
    symbol_t *sym = declare(s, name);
    if (!sym) return;
    sym->kind = SYM_FUNC;
    sym->type = ret;
    sym->is_builtin = 1;
    sym->is_generic = generic;
    sym->defined = 1;
    sym->param_count = nargs;
    if (nargs > 0) sym->param_types[0] = a0;
    if (nargs > 1) sym->param_types[1] = a1;
    if (nargs > 2) sym->param_types[2] = a2;
    for (int i = 0; i < nargs; i++) sym->param_quals[i] = GLSL_Q_PARAM_IN;
}

/* Populate the global scope.
 *
 * `generic` marks the large family of functions that accept float, vec2, vec3
 * or vec4 and return the same type -- sin, abs, mix and so on.  GLSL calls
 * these genType functions.  Modelling them with a flag rather than four
 * overloads each keeps the symbol table at 60 entries instead of 240, and the
 * call checker handles the rule in one place. */
static void install_builtins(sema_t *s) {
    /* ---- Variables ---- */
    if (s->u->kind == GLSL_SHADER_VERTEX) {
        add_var(s, "gl_Position",   T_vec(4), GLSL_Q_NONE, 0);
        add_var(s, "gl_PointSize",  T_float(), GLSL_Q_NONE, 0);
    } else {
        add_var(s, "gl_FragColor",  T_vec(4), GLSL_Q_NONE, 0);
        add_var(s, "gl_FragCoord",  T_vec(4), GLSL_Q_NONE, 1);
        add_var(s, "gl_PointCoord", T_vec(2), GLSL_Q_NONE, 1);
        add_var(s, "gl_FrontFacing", T_bool(), GLSL_Q_NONE, 1);
    }

    /* ---- Angle, exponential and common functions (genType) ---- */
    static const char *gen1[] = {
        "radians", "degrees", "sin", "cos", "tan", "asin", "acos", "atan",
        "exp", "log", "exp2", "log2", "sqrt", "inversesqrt",
        "abs", "sign", "floor", "ceil", "fract", "normalize",
        NULL
    };
    for (int i = 0; gen1[i]; i++) {
        add_func(s, gen1[i], T_float(), 1, T_float(), NULL, NULL, 1);
    }

    static const char *gen2[] = { "pow", "mod", "min", "max", "step", NULL };
    for (int i = 0; gen2[i]; i++) {
        add_func(s, gen2[i], T_float(), 2, T_float(), T_float(), NULL, 1);
    }

    static const char *gen3[] = { "clamp", "mix", "smoothstep", NULL };
    for (int i = 0; gen3[i]; i++) {
        add_func(s, gen3[i], T_float(), 3, T_float(), T_float(), T_float(), 1);
    }

    /* ---- Geometric functions: these collapse a vector to a scalar, so they
     * are generic in their ARGUMENT but not in their result. ---- */
    add_func(s, "length",   T_float(), 1, T_float(), NULL, NULL, 2);
    add_func(s, "distance", T_float(), 2, T_float(), T_float(), NULL, 2);
    add_func(s, "dot",      T_float(), 2, T_float(), T_float(), NULL, 2);
    /* cross is vec3-only, the one geometric function that is not generic. */
    add_func(s, "cross",    T_vec(3), 2, T_vec(3), T_vec(3), NULL, 0);
    add_func(s, "faceforward", T_float(), 3, T_float(), T_float(), T_float(), 1);
    add_func(s, "reflect",  T_float(), 2, T_float(), T_float(), NULL, 1);
    add_func(s, "refract",  T_float(), 3, T_float(), T_float(), T_float(), 3);

    /* ---- Matrix functions ---- */
    add_func(s, "matrixCompMult", T_mat(2), 2, T_mat(2), T_mat(2), NULL, 4);

    /* ---- Vector relational: vecN -> bvecN ---- */
    static const char *rel[] = {
        "lessThan", "lessThanEqual", "greaterThan", "greaterThanEqual",
        "equal", "notEqual", NULL
    };
    for (int i = 0; rel[i]; i++) {
        add_func(s, rel[i], T_bvec(2), 2, T_float(), T_float(), NULL, 5);
    }
    add_func(s, "any", T_bool(), 1, T_bvec(2), NULL, NULL, 6);
    add_func(s, "all", T_bool(), 1, T_bvec(2), NULL, NULL, 6);
    add_func(s, "not", T_bvec(2), 1, T_bvec(2), NULL, NULL, 7);

    /* ---- Texture lookup ---- */
    add_func(s, "texture2D", T_vec(4), 2,
             glsl_type_basic(GLSL_TY_SAMPLER2D, 1), T_vec(2), NULL, 0);
    add_func(s, "texture2DProj", T_vec(4), 2,
             glsl_type_basic(GLSL_TY_SAMPLER2D, 1), T_vec(4), NULL, 0);
    add_func(s, "texture2DLod", T_vec(4), 3,
             glsl_type_basic(GLSL_TY_SAMPLER2D, 1), T_vec(2), T_float(), 0);
    add_func(s, "textureCube", T_vec(4), 2,
             glsl_type_basic(GLSL_TY_SAMPLERCUBE, 1), T_vec(3), NULL, 0);
    add_func(s, "textureCubeLod", T_vec(4), 3,
             glsl_type_basic(GLSL_TY_SAMPLERCUBE, 1), T_vec(3), T_float(), 0);
}

/* ============================================================================
 * Expression checking
 * ==========================================================================*/

static const glsl_type_t *check_expr(sema_t *s, glsl_node_t *n);

static const glsl_type_t *fail(glsl_node_t *n) {
    if (n) { n->type = glsl_type_error(); n->is_const = 0; n->is_lvalue = 0; }
    return glsl_type_error();
}

static int is_err(const glsl_type_t *t) {
    return !t || t->kind == GLSL_TY_ERROR;
}

/* ---- Swizzles ----
 *
 * Returns the component count, or 0 when `field` is not a valid swizzle.
 * `max_comp` bounds the source vector; `assignable` reports whether every
 * component is distinct, which is what makes `v.xy = ...` legal and
 * `v.xx = ...` not. */
static int swizzle_len(const char *field, int max_comp, int *assignable,
                       int *out_index) {
    static const char *sets[3] = { "xyzw", "rgba", "stpq" };
    size_t n = strlen(field);
    if (n == 0 || n > 4) return 0;

    int set = -1;
    for (int k = 0; k < 3; k++) {
        if (strchr(sets[k], field[0])) { set = k; break; }
    }
    if (set < 0) return 0;

    int seen[4] = { 0, 0, 0, 0 };
    *assignable = 1;
    for (size_t i = 0; i < n; i++) {
        const char *at = strchr(sets[set], field[i]);
        /* Mixing xyzw with rgba in one swizzle is an error (§5.5). */
        if (!at) return 0;
        int idx = (int)(at - sets[set]);
        if (idx >= max_comp) return -1;      /* out of range for this vector */
        if (seen[idx]) *assignable = 0;
        seen[idx] = 1;
        if (out_index) out_index[i] = idx;
    }
    return (int)n;
}

/* ---- Constructors ----
 *
 * GLSL's constructor rules are permissive in a specific way: the arguments are
 * flattened into a stream of components, and as many as the target needs are
 * taken.  Two special cases sit on top:
 *   - a single scalar fills every component (vec3(1.0), and for matrices the
 *     diagonal);
 *   - a matrix constructed from a matrix copies the overlapping part.
 * Everything else is "enough components, no more than needed".
 */
static const glsl_type_t *check_constructor(sema_t *s, glsl_node_t *n,
                                            const glsl_type_t *target) {
    int want = glsl_type_components(target);
    int have = 0;
    int all_const = 1;
    int arg_count = 0;
    int from_matrix = 0;

    /* A STRUCT target takes one argument per field, by type, and a field may
     * itself be a struct -- Outer(Inner(3.0), 4.0) is legal.  So structs are
     * checked before the component-flattening rules below, which exist for
     * vectors and matrices and reject struct arguments outright. */
    if (target->kind == GLSL_TY_STRUCT) {
        for (glsl_node_t *a = n->list; a; a = a->next) {
            const glsl_type_t *at = check_expr(s, a);
            if (is_err(at)) return fail(n);
            if (!a->is_const) all_const = 0;
            arg_count++;
        }
        if (arg_count != target->field_count) {
            glsl_error(s->u, n->line,
                       "constructor for struct '%s' expects %d argument(s), "
                       "got %d",
                       glsl_type_name(target), target->field_count, arg_count);
            return fail(n);
        }
        int i = 0;
        for (glsl_node_t *a = n->list; a; a = a->next, i++) {
            if (!glsl_type_equal(a->type, target->fields[i].type)) {
                glsl_error(s->u, n->line,
                           "field %d of '%s' expects '%s', got '%s'",
                           i + 1, glsl_type_name(target),
                           glsl_type_name(target->fields[i].type),
                           glsl_type_name(a->type));
                return fail(n);
            }
        }
        n->type = target;
        n->is_const = all_const;
        return target;
    }

    for (glsl_node_t *a = n->list; a; a = a->next) {
        const glsl_type_t *at = check_expr(s, a);
        arg_count++;
        if (is_err(at)) return fail(n);
        if (!a->is_const) all_const = 0;

        if (glsl_type_is_sampler(at)) {
            glsl_error(s->u, n->line,
                       "sampler types cannot be used in a constructor");
            return fail(n);
        }
        if (at->kind == GLSL_TY_STRUCT || at->array_len > 0) {
            glsl_error(s->u, n->line,
                       "cannot construct '%s' from '%s'",
                       glsl_type_name(target), glsl_type_name(at));
            return fail(n);
        }
        if (at->kind == GLSL_TY_MAT) from_matrix = 1;
        have += glsl_type_components(at);
    }

    if (arg_count == 0) {
        glsl_error(s->u, n->line, "constructor for '%s' needs arguments",
                   glsl_type_name(target));
        return fail(n);
    }

    /* One scalar fills everything. */
    if (arg_count == 1 && have == 1) {
        n->type = target;
        n->is_const = all_const;
        return target;
    }

    /* mat from mat: any size pair is allowed. */
    if (target->kind == GLSL_TY_MAT && arg_count == 1 && from_matrix) {
        n->type = target;
        n->is_const = all_const;
        return target;
    }

    if (have < want) {
        glsl_error(s->u, n->line,
                   "constructor for '%s' needs %d components, got %d",
                   glsl_type_name(target), want, have);
        return fail(n);
    }

    /* Too many components is an error too (§5.4.2): silently dropping them
     * hides a real mistake, usually a wrong vector size somewhere upstream.
     * The exception is the single-argument forms handled above. */
    if (have > want && arg_count > 1) {
        /* GLSL permits the LAST argument to overflow, e.g. vec2(v3) is an
         * error but vec3(v2, v2) supplies four for three, which drivers
         * accept.  Being strict here would reject shaders that work
         * elsewhere, so only a whole redundant argument is diagnosed. */
        int without_last = 0;
        glsl_node_t *a = n->list;
        for (int i = 0; i < arg_count - 1; i++, a = a->next) {
            without_last += glsl_type_components(a->type);
        }
        if (without_last >= want) {
            glsl_error(s->u, n->line,
                       "constructor for '%s' has an unused argument",
                       glsl_type_name(target));
            return fail(n);
        }
    }

    n->type = target;
    n->is_const = all_const;
    return target;
}

/* ---- Calls to the generic built-ins ----
 *
 * `generic` values, matching add_func():
 *   1  genType f(genType...)          -- sin, mix, clamp
 *   2  float f(genType...)            -- length, dot, distance
 *   3  genType refract(genType, genType, float)
 *   4  matN matrixCompMult(matN, matN)
 *   5  bvecN f(vecN, vecN)            -- lessThan and friends
 *   6  bool f(bvecN)                  -- any, all
 *   7  bvecN not(bvecN)
 */
static const glsl_type_t *check_builtin_call(sema_t *s, glsl_node_t *n,
                                             symbol_t *fn, int argc,
                                             const glsl_type_t **argt) {
    if (argc != fn->param_count) {
        glsl_error(s->u, n->line, "'%s' expects %d argument(s), got %d",
                   fn->name, fn->param_count, argc);
        return fail(n);
    }

    if (!fn->is_generic) {
        for (int i = 0; i < argc; i++) {
            if (!glsl_type_equal(argt[i], fn->param_types[i])) {
                glsl_error(s->u, n->line,
                           "argument %d of '%s' expects '%s', got '%s'",
                           i + 1, fn->name,
                           glsl_type_name(fn->param_types[i]),
                           glsl_type_name(argt[i]));
                return fail(n);
            }
        }
        n->type = fn->type;
        return fn->type;
    }

    /* All the generic families require float-based arguments except the bool
     * ones, so check that first and give a single clear message. */
    int want_bool = (fn->is_generic == 6 || fn->is_generic == 7);

    for (int i = 0; i < argc; i++) {
        if (want_bool) {
            /* any(), all() and not() operate on a bvec.  A plain bool has
             * nothing to reduce, and accepting it would hide the common
             * mistake of forgetting lessThan() and comparing scalars. */
            if (argt[i]->kind != GLSL_TY_BVEC) {
                glsl_error(s->u, n->line,
                           "argument %d of '%s' must be a bvec, got '%s'",
                           i + 1, fn->name, glsl_type_name(argt[i]));
                return fail(n);
            }
        } else if (fn->is_generic == 5) {
            if (argt[i]->kind != GLSL_TY_VEC && argt[i]->kind != GLSL_TY_IVEC) {
                glsl_error(s->u, n->line,
                           "argument %d of '%s' must be a vector, got '%s'",
                           i + 1, fn->name, glsl_type_name(argt[i]));
                return fail(n);
            }
        } else if (fn->is_generic == 4) {
            if (argt[i]->kind != GLSL_TY_MAT) {
                glsl_error(s->u, n->line,
                           "argument %d of '%s' must be a matrix, got '%s'",
                           i + 1, fn->name, glsl_type_name(argt[i]));
                return fail(n);
            }
        } else {
            if (argt[i]->kind != GLSL_TY_FLOAT && argt[i]->kind != GLSL_TY_VEC) {
                glsl_error(s->u, n->line,
                           "argument %d of '%s' must be float or a float "
                           "vector, got '%s'",
                           i + 1, fn->name, glsl_type_name(argt[i]));
                return fail(n);
            }
        }
    }

    /* The shape rule: every generic argument must agree, except that some
     * families allow a trailing scalar (mix's third argument, refract's eta,
     * clamp's bounds). */
    const glsl_type_t *shape = argt[0];
    int scalar_tail_ok = (strcmp(fn->name, "mix") == 0 ||
                          strcmp(fn->name, "clamp") == 0 ||
                          strcmp(fn->name, "min") == 0 ||
                          strcmp(fn->name, "max") == 0 ||
                          strcmp(fn->name, "mod") == 0 ||
                          strcmp(fn->name, "step") == 0 ||
                          strcmp(fn->name, "smoothstep") == 0);

    for (int i = 1; i < argc; i++) {
        if (glsl_type_equal(argt[i], shape)) continue;
        if (fn->is_generic == 3 && i == 2) continue;   /* refract's eta */
        if (scalar_tail_ok && glsl_type_components(argt[i]) == 1) continue;
        /* step(edge, x) and smoothstep take their scalars FIRST. */
        if (scalar_tail_ok && glsl_type_components(shape) == 1) {
            shape = argt[i];
            continue;
        }
        glsl_error(s->u, n->line,
                   "arguments of '%s' must be the same type: '%s' vs '%s'",
                   fn->name, glsl_type_name(shape), glsl_type_name(argt[i]));
        return fail(n);
    }

    const glsl_type_t *result;
    switch (fn->is_generic) {
    case 2:  result = T_float(); break;                       /* length, dot */
    case 5:  result = T_bvec(shape->rows ? shape->rows : 2); break;
    case 6:  result = T_bool(); break;
    case 7:  result = shape; break;
    default: result = shape; break;
    }
    n->type = result;
    return result;
}

static const glsl_type_t *check_call(sema_t *s, glsl_node_t *n) {
    /* A constructor was tagged by the parser. */
    if (n->decl_type) return check_constructor(s, n, n->decl_type);

    if (!n->v.name) {
        glsl_error(s->u, n->line, "call of a non-function expression");
        return fail(n);
    }

    symbol_t *sym = lookup(s, n->v.name);

    /* A struct constructor: the name resolves to a type, not a symbol.  The
     * parser could not know, because struct names live in its own table. */
    if (!sym && n->a && n->a->kind == GLSL_NODE_IDENT) {
        /* Fall through to the not-declared diagnostic below. */
    }

    if (!sym) {
        glsl_error(s->u, n->line, "'%s' is not declared", n->v.name);
        return fail(n);
    }
    if (sym->kind != SYM_FUNC) {
        glsl_error(s->u, n->line, "'%s' is not a function", n->v.name);
        return fail(n);
    }

    const glsl_type_t *argt[GLSL_MAX_ARGS];
    int argc = 0;
    for (glsl_node_t *a = n->list; a; a = a->next) {
        const glsl_type_t *t = check_expr(s, a);
        if (argc < GLSL_MAX_ARGS) argt[argc] = t;
        argc++;
        if (is_err(t)) return fail(n);
    }
    if (argc > GLSL_MAX_ARGS) {
        glsl_error(s->u, n->line, "too many arguments to '%s'", n->v.name);
        return fail(n);
    }

    if (sym->is_builtin) return check_builtin_call(s, n, sym, argc, argt);

    if (argc != sym->param_count) {
        glsl_error(s->u, n->line, "'%s' expects %d argument(s), got %d",
                   sym->name, sym->param_count, argc);
        return fail(n);
    }

    int i = 0;
    for (glsl_node_t *a = n->list; a; a = a->next, i++) {
        if (!glsl_type_equal(argt[i], sym->param_types[i])) {
            glsl_error(s->u, n->line,
                       "argument %d of '%s' expects '%s', got '%s'",
                       i + 1, sym->name,
                       glsl_type_name(sym->param_types[i]),
                       glsl_type_name(argt[i]));
            return fail(n);
        }
        /* An out or inout parameter is written by the callee, so the argument
         * has to be something that can be written back to. */
        if ((sym->param_quals[i] == GLSL_Q_PARAM_OUT ||
             sym->param_quals[i] == GLSL_Q_PARAM_INOUT) && !a->is_lvalue) {
            glsl_error(s->u, n->line,
                       "argument %d of '%s' is '%s' and must be assignable",
                       i + 1, sym->name,
                       sym->param_quals[i] == GLSL_Q_PARAM_OUT ? "out" : "inout");
            return fail(n);
        }
    }

    n->type = sym->type;
    return sym->type;
}

static const glsl_type_t *check_binary(sema_t *s, glsl_node_t *n) {
    const glsl_type_t *a = check_expr(s, n->a);
    const glsl_type_t *b = check_expr(s, n->b);
    if (is_err(a) || is_err(b)) return fail(n);

    n->is_const = (n->a->is_const && n->b->is_const);

    switch (n->op) {
    case GLSL_TOK_COMMA:
        n->type = b;
        return b;

    case GLSL_TOK_AND_AND: case GLSL_TOK_OR_OR: case GLSL_TOK_XOR_XOR:
        if (a->kind != GLSL_TY_BOOL || b->kind != GLSL_TY_BOOL) {
            glsl_error(s->u, n->line,
                       "logical operator requires bool operands, got '%s' "
                       "and '%s'", glsl_type_name(a), glsl_type_name(b));
            return fail(n);
        }
        n->type = T_bool();
        return n->type;

    case GLSL_TOK_EQ: case GLSL_TOK_NE:
        /* Whole-value comparison, yielding ONE bool.  Component-wise
         * comparison is the equal()/notEqual() built-ins. */
        if (!glsl_type_equal(a, b)) {
            glsl_error(s->u, n->line,
                       "cannot compare '%s' with '%s'",
                       glsl_type_name(a), glsl_type_name(b));
            return fail(n);
        }
        if (glsl_type_is_sampler(a)) {
            glsl_error(s->u, n->line, "samplers cannot be compared");
            return fail(n);
        }
        n->type = T_bool();
        return n->type;

    case GLSL_TOK_LT: case GLSL_TOK_GT:
    case GLSL_TOK_LE: case GLSL_TOK_GE:
        /* Scalars only.  For vectors GLSL requires lessThan() and friends,
         * which return a bvec -- there is no sensible single bool for "is
         * this vector less than that one". */
        if ((a->kind != GLSL_TY_FLOAT && a->kind != GLSL_TY_INT) ||
            (b->kind != GLSL_TY_FLOAT && b->kind != GLSL_TY_INT)) {
            glsl_error(s->u, n->line,
                       "relational operators need scalar operands; use "
                       "lessThan()/greaterThan() for vectors");
            return fail(n);
        }
        if (a->kind != b->kind) {
            glsl_error(s->u, n->line,
                       "cannot compare '%s' with '%s' (GLSL ES has no "
                       "implicit conversions)",
                       glsl_type_name(a), glsl_type_name(b));
            return fail(n);
        }
        n->type = T_bool();
        return n->type;

    default:
        break;
    }

    /* Arithmetic. */
    if (glsl_type_is_sampler(a) || glsl_type_is_sampler(b) ||
        a->kind == GLSL_TY_STRUCT || b->kind == GLSL_TY_STRUCT ||
        a->array_len > 0 || b->array_len > 0) {
        glsl_error(s->u, n->line, "invalid operands '%s' and '%s' for arithmetic",
                   glsl_type_name(a), glsl_type_name(b));
        return fail(n);
    }
    if (glsl_type_is_bool_based(a) || glsl_type_is_bool_based(b)) {
        glsl_error(s->u, n->line, "arithmetic on bool is not allowed");
        return fail(n);
    }

    if (n->op == GLSL_TOK_PERCENT) {
        glsl_error(s->u, n->line, "the %% operator is reserved in GLSL ES 1.0; "
                                  "use mod()");
        return fail(n);
    }

    /* Matrix products are the interesting case: mat*vec and vec*mat are
     * linear transforms whose result is a vector, and mat*mat is a matrix
     * product -- none of them component-wise. */
    if (n->op == GLSL_TOK_STAR) {
        if (a->kind == GLSL_TY_MAT && b->kind == GLSL_TY_VEC) {
            if (a->rows != b->rows) {
                glsl_error(s->u, n->line, "cannot multiply '%s' by '%s'",
                           glsl_type_name(a), glsl_type_name(b));
                return fail(n);
            }
            n->type = b;
            return n->type;
        }
        if (a->kind == GLSL_TY_VEC && b->kind == GLSL_TY_MAT) {
            if (a->rows != b->rows) {
                glsl_error(s->u, n->line, "cannot multiply '%s' by '%s'",
                           glsl_type_name(a), glsl_type_name(b));
                return fail(n);
            }
            n->type = a;
            return n->type;
        }
        if (a->kind == GLSL_TY_MAT && b->kind == GLSL_TY_MAT) {
            if (a->rows != b->rows) {
                glsl_error(s->u, n->line, "cannot multiply '%s' by '%s'",
                           glsl_type_name(a), glsl_type_name(b));
                return fail(n);
            }
            n->type = a;
            return n->type;
        }
    }

    /* Same type: component-wise. */
    if (glsl_type_equal(a, b)) {
        n->type = a;
        return a;
    }

    /* Scalar with vector or matrix: component-wise, and the scalar must have
     * the same ELEMENT type -- `vec3 * 2` is an error because 2 is an int. */
    int a_scalar = (glsl_type_components(a) == 1);
    int b_scalar = (glsl_type_components(b) == 1);

    if (a_scalar != b_scalar) {
        const glsl_type_t *scalar = a_scalar ? a : b;
        const glsl_type_t *aggregate = a_scalar ? b : a;
        if (glsl_type_equal(scalar, glsl_type_element(aggregate))) {
            n->type = aggregate;
            return aggregate;
        }
        glsl_error(s->u, n->line,
                   "cannot combine '%s' with '%s': GLSL ES has no implicit "
                   "conversions, so the scalar must be '%s'",
                   glsl_type_name(a), glsl_type_name(b),
                   glsl_type_name(glsl_type_element(aggregate)));
        return fail(n);
    }

    glsl_error(s->u, n->line,
               "cannot combine '%s' with '%s' (GLSL ES has no implicit "
               "conversions)", glsl_type_name(a), glsl_type_name(b));
    return fail(n);
}

static const glsl_type_t *check_expr(sema_t *s, glsl_node_t *n) {
    if (!n) return glsl_type_error();
    if (n->type) return n->type;            /* already checked */

    switch (n->kind) {
    case GLSL_NODE_ERROR:
        return fail(n);

    case GLSL_NODE_INT_LIT:
        n->type = T_int();  n->is_const = 1; return n->type;
    case GLSL_NODE_FLOAT_LIT:
        n->type = T_float(); n->is_const = 1; return n->type;
    case GLSL_NODE_BOOL_LIT:
        n->type = T_bool(); n->is_const = 1; return n->type;

    case GLSL_NODE_IDENT: {
        symbol_t *sym = lookup(s, n->v.name);
        if (!sym) {
            glsl_error(s->u, n->line, "'%s' is not declared",
                       n->v.name ? n->v.name : "?");
            return fail(n);
        }
        if (sym->kind == SYM_FUNC) {
            glsl_error(s->u, n->line, "'%s' is a function; did you mean '%s()'?",
                       sym->name, sym->name);
            return fail(n);
        }
        n->type      = sym->type;
        n->is_const  = (sym->qual == GLSL_Q_CONST);
        n->is_lvalue = !sym->readonly;
        return n->type;
    }

    case GLSL_NODE_UNARY: {
        const glsl_type_t *t = check_expr(s, n->a);
        if (is_err(t)) return fail(n);
        n->is_const = n->a->is_const;

        if (n->op == GLSL_TOK_BANG) {
            if (t->kind != GLSL_TY_BOOL) {
                glsl_error(s->u, n->line,
                           "'!' requires a bool operand, got '%s'",
                           glsl_type_name(t));
                return fail(n);
            }
            n->type = T_bool();
            return n->type;
        }
        if (n->op == GLSL_TOK_INC || n->op == GLSL_TOK_DEC) {
            if (!n->a->is_lvalue) {
                glsl_error(s->u, n->line,
                           "operand of '%s' must be assignable",
                           n->op == GLSL_TOK_INC ? "++" : "--");
                return fail(n);
            }
            n->is_const = 0;
        }
        if (!glsl_type_is_numeric(t) && t->kind != GLSL_TY_MAT) {
            glsl_error(s->u, n->line, "cannot apply unary operator to '%s'",
                       glsl_type_name(t));
            return fail(n);
        }
        n->type = t;
        return t;
    }

    case GLSL_NODE_BINARY:
        return check_binary(s, n);

    case GLSL_NODE_POSTFIX: {
        const glsl_type_t *t = check_expr(s, n->a);
        if (is_err(t)) return fail(n);
        if (!n->a->is_lvalue) {
            glsl_error(s->u, n->line, "operand of '%s' must be assignable",
                       n->op == GLSL_TOK_INC ? "++" : "--");
            return fail(n);
        }
        if (!glsl_type_is_numeric(t)) {
            glsl_error(s->u, n->line, "cannot increment '%s'",
                       glsl_type_name(t));
            return fail(n);
        }
        n->type = t;
        return t;
    }

    case GLSL_NODE_ASSIGN: {
        const glsl_type_t *lt = check_expr(s, n->a);
        const glsl_type_t *rt = check_expr(s, n->b);
        if (is_err(lt) || is_err(rt)) return fail(n);

        if (!n->a->is_lvalue) {
            glsl_error(s->u, n->line, "left side of assignment is not "
                                      "assignable");
            return fail(n);
        }

        if (n->op == GLSL_TOK_ASSIGN) {
            if (!glsl_type_equal(lt, rt)) {
                glsl_error(s->u, n->line,
                           "cannot assign '%s' to '%s' (GLSL ES has no "
                           "implicit conversions)",
                           glsl_type_name(rt), glsl_type_name(lt));
                return fail(n);
            }
        } else {
            /* Compound assignment: the same rules as the equivalent binary
             * operator, plus the result must fit back into the left side.
             * `v *= 2.0` is fine, `f *= v` is not. */
            int ok = glsl_type_equal(lt, rt);
            if (!ok && glsl_type_components(rt) == 1 &&
                glsl_type_equal(rt, glsl_type_element(lt))) {
                ok = 1;
            }
            if (!ok && n->op == GLSL_TOK_MUL_ASSIGN &&
                lt->kind == GLSL_TY_VEC && rt->kind == GLSL_TY_MAT &&
                lt->rows == rt->rows) {
                ok = 1;
            }
            if (!ok) {
                glsl_error(s->u, n->line,
                           "cannot apply '%s' with '%s' to '%s'",
                           n->op == GLSL_TOK_ADD_ASSIGN ? "+=" :
                           n->op == GLSL_TOK_SUB_ASSIGN ? "-=" :
                           n->op == GLSL_TOK_MUL_ASSIGN ? "*=" : "/=",
                           glsl_type_name(rt), glsl_type_name(lt));
                return fail(n);
            }
        }
        n->type = lt;
        n->is_lvalue = 0;   /* the result of an assignment is not assignable */
        return lt;
    }

    case GLSL_NODE_CONDITIONAL: {
        const glsl_type_t *ct = check_expr(s, n->a);
        const glsl_type_t *tt = check_expr(s, n->b);
        const glsl_type_t *et = check_expr(s, n->c);
        if (is_err(ct) || is_err(tt) || is_err(et)) return fail(n);

        if (ct->kind != GLSL_TY_BOOL) {
            glsl_error(s->u, n->line,
                       "condition of '?:' must be bool, got '%s'",
                       glsl_type_name(ct));
            return fail(n);
        }
        if (!glsl_type_equal(tt, et)) {
            glsl_error(s->u, n->line,
                       "both branches of '?:' must have the same type: "
                       "'%s' vs '%s'", glsl_type_name(tt), glsl_type_name(et));
            return fail(n);
        }
        n->type = tt;
        n->is_const = (n->a->is_const && n->b->is_const && n->c->is_const);
        return tt;
    }

    case GLSL_NODE_CALL:
        return check_call(s, n);

    case GLSL_NODE_FIELD: {
        const glsl_type_t *t = check_expr(s, n->a);
        if (is_err(t)) return fail(n);
        if (!n->v.name) return fail(n);

        if (t->array_len > 0) {
            /* `.length` is GLSL 3.0; in ES 1.0 there is nothing to select. */
            glsl_error(s->u, n->line, "cannot select a field of an array");
            return fail(n);
        }

        if (t->kind == GLSL_TY_STRUCT) {
            for (int i = 0; i < t->field_count; i++) {
                if (strcmp(t->fields[i].name, n->v.name) == 0) {
                    n->type      = t->fields[i].type;
                    n->is_lvalue = n->a->is_lvalue;
                    n->is_const  = n->a->is_const;
                    return n->type;
                }
            }
            glsl_error(s->u, n->line, "'%s' has no field named '%s'",
                       glsl_type_name(t), n->v.name);
            return fail(n);
        }

        int comps = 0;
        if (t->kind == GLSL_TY_VEC || t->kind == GLSL_TY_IVEC ||
            t->kind == GLSL_TY_BVEC) {
            comps = t->rows;
        } else if (glsl_type_components(t) == 1) {
            /* A scalar has one component and `f.x` is legal GLSL. */
            comps = 1;
        } else {
            glsl_error(s->u, n->line, "cannot swizzle '%s'", glsl_type_name(t));
            return fail(n);
        }

        int assignable = 1;
        int len = swizzle_len(n->v.name, comps, &assignable, NULL);
        if (len == 0) {
            glsl_error(s->u, n->line, "'%s' is not a valid swizzle",
                       n->v.name);
            return fail(n);
        }
        if (len < 0) {
            glsl_error(s->u, n->line,
                       "swizzle '%s' selects a component beyond '%s'",
                       n->v.name, glsl_type_name(t));
            return fail(n);
        }

        glsl_ty_kind_t base =
            (t->kind == GLSL_TY_IVEC) ? GLSL_TY_IVEC :
            (t->kind == GLSL_TY_BVEC) ? GLSL_TY_BVEC : GLSL_TY_VEC;
        n->type = (len == 1) ? glsl_type_element(t)
                             : glsl_type_basic(base, len);
        n->is_lvalue = n->a->is_lvalue && assignable;
        n->is_const  = n->a->is_const;
        return n->type;
    }

    case GLSL_NODE_INDEX: {
        const glsl_type_t *t  = check_expr(s, n->a);
        const glsl_type_t *it = check_expr(s, n->b);
        if (is_err(t) || is_err(it)) return fail(n);

        if (it->kind != GLSL_TY_INT) {
            glsl_error(s->u, n->line, "array index must be int, got '%s'",
                       glsl_type_name(it));
            return fail(n);
        }

        /* A constant index out of range is a compile-time error, and catching
         * it here is far better than an out-of-bounds read in G11b.
         *
         * `-1` parses as a unary minus over the literal 1, so the negation
         * has to be seen through -- otherwise the single most likely
         * out-of-range index is the one case that slips past. */
        long idx = 0;
        int have_idx = 0;
        if (n->b->kind == GLSL_NODE_INT_LIT) {
            idx = n->b->v.ival;
            have_idx = 1;
        } else if (n->b->kind == GLSL_NODE_UNARY &&
                   n->b->op == GLSL_TOK_MINUS && n->b->a &&
                   n->b->a->kind == GLSL_NODE_INT_LIT) {
            idx = -n->b->a->v.ival;
            have_idx = 1;
        }

        if (have_idx) {
            int bound = t->array_len > 0 ? t->array_len :
                        (t->kind == GLSL_TY_MAT) ? t->rows :
                        (t->kind == GLSL_TY_VEC || t->kind == GLSL_TY_IVEC ||
                         t->kind == GLSL_TY_BVEC) ? t->rows : 0;
            if (bound > 0 && (idx < 0 || idx >= bound)) {
                glsl_error(s->u, n->line,
                           "index %ld is out of range for '%s'",
                           idx, glsl_type_name(t));
                return fail(n);
            }
        }

        if (t->array_len > 0) {
            glsl_type_t *el = (glsl_type_t *)glsl_alloc(s->u,
                                                        sizeof(glsl_type_t));
            if (!el) return fail(n);
            *el = *t;
            el->array_len = 0;
            n->type = el;
        } else if (t->kind == GLSL_TY_MAT) {
            n->type = T_vec(t->rows);          /* a column */
        } else if (t->kind == GLSL_TY_VEC || t->kind == GLSL_TY_IVEC ||
                   t->kind == GLSL_TY_BVEC) {
            n->type = glsl_type_element(t);
        } else {
            glsl_error(s->u, n->line, "cannot index '%s'", glsl_type_name(t));
            return fail(n);
        }
        n->is_lvalue = n->a->is_lvalue;
        n->is_const  = n->a->is_const && n->b->is_const;
        return n->type;
    }

    default:
        return fail(n);
    }
}

/* ============================================================================
 * Statement checking
 * ==========================================================================*/

static void check_stmt(sema_t *s, glsl_node_t *n);

static void check_decl(sema_t *s, glsl_node_t *n, int global) {
    if (!n->decl_type) return;

    if (n->decl_type->kind == GLSL_TY_VOID) {
        glsl_error(s->u, n->line, "variable '%s' cannot have type void",
                   n->v.name ? n->v.name : "?");
        return;
    }

    if (n->v.name && strncmp(n->v.name, "gl_", 3) == 0) {
        glsl_error(s->u, n->line,
                   "'%s': names beginning with 'gl_' are reserved", n->v.name);
        return;
    }

    if (lookup_local(s, n->v.name)) {
        glsl_error(s->u, n->line, "'%s' is already declared in this scope",
                   n->v.name ? n->v.name : "?");
        return;
    }

    /* Storage qualifiers are only meaningful at global scope, and each has a
     * shader stage where it makes sense (§4.3). */
    if (!global && (n->qual == GLSL_Q_ATTRIBUTE || n->qual == GLSL_Q_UNIFORM ||
                    n->qual == GLSL_Q_VARYING)) {
        glsl_error(s->u, n->line,
                   "'%s' qualifier is only allowed on global variables",
                   n->qual == GLSL_Q_ATTRIBUTE ? "attribute" :
                   n->qual == GLSL_Q_UNIFORM   ? "uniform" : "varying");
        return;
    }

    if (n->qual == GLSL_Q_ATTRIBUTE) {
        if (s->u->kind != GLSL_SHADER_VERTEX) {
            glsl_error(s->u, n->line,
                       "'attribute' is not allowed in a fragment shader");
            return;
        }
        if (n->decl_type->kind != GLSL_TY_FLOAT &&
            n->decl_type->kind != GLSL_TY_VEC &&
            n->decl_type->kind != GLSL_TY_MAT) {
            glsl_error(s->u, n->line,
                       "attribute '%s' must be float, vec or mat, not '%s'",
                       n->v.name, glsl_type_name(n->decl_type));
            return;
        }
        if (n->decl_type->array_len > 0) {
            glsl_error(s->u, n->line, "attribute '%s' cannot be an array",
                       n->v.name);
            return;
        }
    }

    if (n->qual == GLSL_Q_VARYING) {
        if (n->decl_type->kind != GLSL_TY_FLOAT &&
            n->decl_type->kind != GLSL_TY_VEC &&
            n->decl_type->kind != GLSL_TY_MAT) {
            glsl_error(s->u, n->line,
                       "varying '%s' must be float, vec or mat, not '%s'",
                       n->v.name, glsl_type_name(n->decl_type));
            return;
        }
    }

    if (glsl_type_is_sampler(n->decl_type) && n->qual != GLSL_Q_UNIFORM) {
        /* A sampler is an opaque handle to a texture unit; it can only come
         * from a uniform or a function parameter (§4.1.7). */
        if (global) {
            glsl_error(s->u, n->line,
                       "sampler '%s' must be declared uniform", n->v.name);
            return;
        }
        glsl_error(s->u, n->line,
                   "sampler '%s' cannot be a local variable", n->v.name);
        return;
    }

    if (n->a) {
        const glsl_type_t *it = check_expr(s, n->a);
        if (!is_err(it) && !glsl_type_equal(it, n->decl_type)) {
            glsl_error(s->u, n->line,
                       "cannot initialise '%s' with '%s' (GLSL ES has no "
                       "implicit conversions)",
                       glsl_type_name(n->decl_type), glsl_type_name(it));
        }
        if (n->qual == GLSL_Q_CONST && !n->a->is_const) {
            glsl_error(s->u, n->line,
                       "initialiser of const '%s' is not a constant expression",
                       n->v.name);
        }
        if (n->qual == GLSL_Q_UNIFORM || n->qual == GLSL_Q_ATTRIBUTE ||
            n->qual == GLSL_Q_VARYING) {
            glsl_error(s->u, n->line,
                       "'%s' cannot have an initialiser", n->v.name);
        }
    } else if (n->qual == GLSL_Q_CONST) {
        glsl_error(s->u, n->line, "const '%s' must be initialised", n->v.name);
    }

    symbol_t *sym = declare(s, n->v.name);
    if (!sym) {
        glsl_error(s->u, n->line, "too many declarations (limit %d)",
                   GLSL_MAX_SYMBOLS);
        return;
    }
    sym->kind = SYM_VAR;
    sym->type = n->decl_type;
    sym->qual = n->qual;
    /* A uniform, attribute, or a varying read by a fragment shader, is
     * read-only to the shader; a vertex shader writes its varyings. */
    sym->readonly = (n->qual == GLSL_Q_CONST || n->qual == GLSL_Q_UNIFORM ||
                     n->qual == GLSL_Q_ATTRIBUTE ||
                     (n->qual == GLSL_Q_VARYING &&
                      s->u->kind == GLSL_SHADER_FRAGMENT));
}

static void check_stmt(sema_t *s, glsl_node_t *n) {
    if (!n) return;

    switch (n->kind) {
    case GLSL_NODE_BLOCK:
        scope_push(s);
        for (glsl_node_t *st = n->list; st; st = st->next) check_stmt(s, st);
        scope_pop(s);
        break;

    case GLSL_NODE_DECL:
        check_decl(s, n, 0);
        break;

    case GLSL_NODE_EXPR_STMT:
        check_expr(s, n->a);
        break;

    case GLSL_NODE_IF: {
        const glsl_type_t *c = check_expr(s, n->a);
        if (!is_err(c) && c->kind != GLSL_TY_BOOL) {
            glsl_error(s->u, n->line,
                       "'if' condition must be bool, got '%s'",
                       glsl_type_name(c));
        }
        check_stmt(s, n->b);
        check_stmt(s, n->c);
        break;
    }

    case GLSL_NODE_WHILE: case GLSL_NODE_DO: {
        const glsl_type_t *c = check_expr(s, n->a);
        if (!is_err(c) && c->kind != GLSL_TY_BOOL) {
            glsl_error(s->u, n->line,
                       "loop condition must be bool, got '%s'",
                       glsl_type_name(c));
        }
        s->loop_depth++;
        check_stmt(s, n->b);
        s->loop_depth--;
        break;
    }

    case GLSL_NODE_FOR: {
        /* The initialiser's scope encloses the whole loop, so `for (int i...)`
         * does not leak `i` but the condition and step can see it. */
        scope_push(s);
        for (glsl_node_t *d = n->a; d; d = d->next) check_stmt(s, d);
        if (n->b) {
            const glsl_type_t *c = check_expr(s, n->b);
            if (!is_err(c) && c->kind != GLSL_TY_BOOL) {
                glsl_error(s->u, n->line,
                           "'for' condition must be bool, got '%s'",
                           glsl_type_name(c));
            }
        }
        if (n->c) check_expr(s, n->c);
        s->loop_depth++;
        check_stmt(s, n->d);
        s->loop_depth--;
        scope_pop(s);
        break;
    }

    case GLSL_NODE_RETURN: {
        if (!s->in_function) {
            glsl_error(s->u, n->line, "'return' outside a function");
            break;
        }
        int want_void = s->return_type &&
                        s->return_type->kind == GLSL_TY_VOID;
        if (n->a) {
            const glsl_type_t *t = check_expr(s, n->a);
            if (want_void) {
                glsl_error(s->u, n->line,
                           "cannot return a value from a void function");
            } else if (!is_err(t) && !glsl_type_equal(t, s->return_type)) {
                glsl_error(s->u, n->line,
                           "returning '%s' from a function declared '%s'",
                           glsl_type_name(t), glsl_type_name(s->return_type));
            }
        } else if (!want_void) {
            glsl_error(s->u, n->line,
                       "'return' with no value in a function declared '%s'",
                       glsl_type_name(s->return_type));
        }
        break;
    }

    case GLSL_NODE_BREAK:
        if (s->loop_depth == 0) {
            glsl_error(s->u, n->line, "'break' outside a loop");
        }
        break;

    case GLSL_NODE_CONTINUE:
        if (s->loop_depth == 0) {
            glsl_error(s->u, n->line, "'continue' outside a loop");
        }
        break;

    case GLSL_NODE_DISCARD:
        if (s->u->kind != GLSL_SHADER_FRAGMENT) {
            glsl_error(s->u, n->line,
                       "'discard' is only allowed in a fragment shader");
        }
        break;

    case GLSL_NODE_EMPTY: case GLSL_NODE_ERROR:
        break;

    default:
        check_expr(s, n);
        break;
    }
}

/* ============================================================================
 * Top level
 * ==========================================================================*/

/* Does this statement definitely leave the function?  Used for the
 * "not all paths return a value" check.  Deliberately conservative: it only
 * says yes when it is certain, so a shader is never rejected for a path that
 * does return. */
static int always_returns(const glsl_node_t *n) {
    if (!n) return 0;
    switch (n->kind) {
    case GLSL_NODE_RETURN:
    case GLSL_NODE_DISCARD:
        return 1;
    case GLSL_NODE_BLOCK: {
        for (const glsl_node_t *st = n->list; st; st = st->next) {
            if (always_returns(st)) return 1;
        }
        return 0;
    }
    case GLSL_NODE_IF:
        /* Only if BOTH branches do, and there is an else. */
        return n->c && always_returns(n->b) && always_returns(n->c);
    default:
        return 0;
    }
}

static void check_function(sema_t *s, glsl_node_t *fn) {
    if (fn->v.name && strncmp(fn->v.name, "gl_", 3) == 0) {
        glsl_error(s->u, fn->line,
                   "'%s': names beginning with 'gl_' are reserved", fn->v.name);
    }

    symbol_t *prev = lookup(s, fn->v.name);
    if (prev && prev->kind == SYM_FUNC && prev->defined && fn->body) {
        glsl_error(s->u, fn->line, "function '%s' is already defined",
                   fn->v.name);
    }

    symbol_t *sym = prev && prev->kind == SYM_FUNC && !prev->is_builtin
                  ? prev : declare(s, fn->v.name);
    if (sym) {
        sym->kind = SYM_FUNC;
        sym->type = fn->decl_type;
        sym->param_count = fn->param_count;
        for (int i = 0; fn->params && i < fn->param_count; i++) {
            sym->param_types[i] = fn->params[i].type;
            sym->param_quals[i] = fn->params[i].qual;
        }
        if (fn->body) sym->defined = 1;
    }

    int is_main = fn->v.name && strcmp(fn->v.name, "main") == 0;
    if (is_main) {
        if (fn->decl_type->kind != GLSL_TY_VOID) {
            glsl_error(s->u, fn->line, "'main' must return void");
        }
        if (fn->param_count != 0) {
            glsl_error(s->u, fn->line, "'main' must take no parameters");
        }
        if (fn->body) s->found_main = 1;
    }

    if (!fn->body) return;                     /* a prototype */

    /* Parameters live in a scope between the globals and the body, so the
     * body may shadow them but they may not collide with each other. */
    scope_push(s);
    for (int i = 0; fn->params && i < fn->param_count; i++) {
        if (!fn->params[i].name) continue;
        if (lookup_local(s, fn->params[i].name)) {
            glsl_error(s->u, fn->line, "duplicate parameter name '%s'",
                       fn->params[i].name);
            continue;
        }
        if (fn->params[i].type->kind == GLSL_TY_VOID) {
            glsl_error(s->u, fn->line, "parameter '%s' cannot have type void",
                       fn->params[i].name);
            continue;
        }
        symbol_t *ps = declare(s, fn->params[i].name);
        if (!ps) continue;
        ps->kind = SYM_VAR;
        ps->type = fn->params[i].type;
        ps->qual = fn->params[i].qual;
        ps->readonly = 0;         /* even an `in` parameter is a local copy */
    }

    const glsl_type_t *saved_ret = s->return_type;
    int saved_in = s->in_function;
    s->return_type = fn->decl_type;
    s->in_function = 1;

    check_stmt(s, fn->body);

    if (fn->decl_type->kind != GLSL_TY_VOID && !always_returns(fn->body)) {
        glsl_error(s->u, fn->line,
                   "not all control paths of '%s' return a value", fn->v.name);
    }

    s->return_type = saved_ret;
    s->in_function = saved_in;
    scope_pop(s);
}

/* Does the tree write gl_Position anywhere?  A vertex shader that does not is
 * legal by the letter of the specification but renders nothing, and saying so
 * at compile time saves an hour of staring at a blank window. */
static int writes_ident(const glsl_node_t *n, const char *name) {
    if (!n) return 0;

    if (n->kind == GLSL_NODE_ASSIGN) {
        const glsl_node_t *lhs = n->a;
        while (lhs && (lhs->kind == GLSL_NODE_FIELD ||
                       lhs->kind == GLSL_NODE_INDEX)) {
            lhs = lhs->a;
        }
        if (lhs && lhs->kind == GLSL_NODE_IDENT && lhs->v.name &&
            strcmp(lhs->v.name, name) == 0) {
            return 1;
        }
    }

    if (writes_ident(n->a, name)) return 1;
    if (writes_ident(n->b, name)) return 1;
    if (writes_ident(n->c, name)) return 1;
    if (writes_ident(n->d, name)) return 1;
    if (writes_ident(n->body, name)) return 1;
    for (const glsl_node_t *st = n->list; st; st = st->next) {
        if (writes_ident(st, name)) return 1;
    }
    return 0;
}

int glsl_check(glsl_unit_t *u) {
    if (!u->root) return 0;

    sema_t *s = (sema_t *)glsl_alloc(u, sizeof(sema_t));
    if (!s) {
        glsl_error(u, 1, "shader too complex (compiler out of memory)");
        return 0;
    }
    memset(s, 0, sizeof *s);
    s->u = u;
    s->scope = 0;

    install_builtins(s);

    /* Globals and functions share one scope, and a function may call one
     * declared later, so functions are entered before their bodies are
     * checked. */
    s->scope = 1;

    for (glsl_node_t *ext = u->root->list; ext; ext = ext->next) {
        if (ext->kind != GLSL_NODE_FUNCTION) continue;
        if (lookup_local(s, ext->v.name)) continue;   /* prototype seen */
        symbol_t *sym = declare(s, ext->v.name);
        if (!sym) continue;
        sym->kind = SYM_FUNC;
        sym->type = ext->decl_type;
        sym->param_count = ext->param_count;
        for (int i = 0; ext->params && i < ext->param_count; i++) {
            sym->param_types[i] = ext->params[i].type;
            sym->param_quals[i] = ext->params[i].qual;
        }
    }

    for (glsl_node_t *ext = u->root->list; ext; ext = ext->next) {
        switch (ext->kind) {
        case GLSL_NODE_FUNCTION: check_function(s, ext);   break;
        case GLSL_NODE_DECL:     check_decl(s, ext, 1);    break;
        default: break;
        }
    }

    if (!s->found_main) {
        glsl_error(u, 1, "shader has no 'main' function");
    } else if (u->kind == GLSL_SHADER_VERTEX &&
               !writes_ident(u->root, "gl_Position")) {
        glsl_error(u, 1, "vertex shader never writes gl_Position");
    } else if (u->kind == GLSL_SHADER_FRAGMENT &&
               !writes_ident(u->root, "gl_FragColor")) {
        /* A fragment shader that only discards is legal, so this is checked
         * against the whole tree rather than per path. */
        int discards = 0;
        for (glsl_node_t *ext = u->root->list; ext && !discards;
             ext = ext->next) {
            /* A cheap scan: any discard anywhere excuses the missing write. */
            const glsl_node_t *stack[GLSL_MAX_NEST];
            int top = 0;
            stack[top++] = ext;
            while (top > 0 && !discards) {
                const glsl_node_t *cur = stack[--top];
                if (!cur) continue;
                if (cur->kind == GLSL_NODE_DISCARD) { discards = 1; break; }
                const glsl_node_t *kids[5] = { cur->a, cur->b, cur->c,
                                               cur->d, cur->body };
                for (int i = 0; i < 5 && top < GLSL_MAX_NEST; i++) {
                    if (kids[i]) stack[top++] = kids[i];
                }
                for (const glsl_node_t *st = cur->list;
                     st && top < GLSL_MAX_NEST; st = st->next) {
                    stack[top++] = st;
                }
            }
        }
        if (!discards) {
            glsl_error(u, 1, "fragment shader never writes gl_FragColor");
        }
    }

    return u->error_count == 0;
}

/* ---- GL2 phase L5: the conservative early-Z predicate ----
 *
 * Same cheap whole-tree scan the missing-gl_FragColor check above uses: a
 * `discard` statement anywhere makes shade-then-depth the only order whose
 * observable behaviour matches the spec, because a discarded fragment must
 * reach no framebuffer operation at all.  Everything else can take early-Z.
 *
 * A store to `gl_FragDepth` would belong here for the same reason (the depth
 * test would consult a value the fragment shader computes), but the language
 * does not have `gl_FragDepth` yet -- when it gains one, this scan must learn
 * it in the same commit. */
int glsl_fragment_may_kill_early_z(const glsl_unit_t *u) {
    if (!u || !u->root) return 0;
    for (const glsl_node_t *ext = u->root->list; ext; ext = ext->next) {
        const glsl_node_t *stack[GLSL_MAX_NEST];
        int top = 0;
        stack[top++] = ext;
        while (top > 0) {
            const glsl_node_t *cur = stack[--top];
            if (!cur) continue;
            if (cur->kind == GLSL_NODE_DISCARD) return 1;
            const glsl_node_t *kids[5] = { cur->a, cur->b, cur->c,
                                           cur->d, cur->body };
            for (int i = 0; i < 5 && top < GLSL_MAX_NEST; i++) {
                if (kids[i]) stack[top++] = kids[i];
            }
            for (const glsl_node_t *st = cur->list;
                 st && top < GLSL_MAX_NEST; st = st->next) {
                stack[top++] = st;
            }
        }
    }
    return 0;
}

/* libgl/src/glsl_exec.c — GLSL ES 1.0 execution engine.
 *
 * Phase G11b of GL_PLAN.md.
 *
 * WHAT THIS IS
 *
 * An AST-walking interpreter over the typed tree G11a produces.  It runs
 * main(), reads and writes the outside world through glsl_env_t callbacks, and
 * knows nothing about rasterizers, contexts or textures beyond a sampling
 * callback.  That is what lets the whole thing be tested numerically with no
 * GL context in sight — and what will let G11c attach the real pipeline
 * without touching this file.
 *
 * WHY NOT A BYTECODE VM
 *
 * The plan permitted one "if profiling demands it".  Profiling says otherwise:
 * a second IR would add a translation step and a second set of bugs to remove
 * one switch dispatch per node, while the actual cost is the per-component
 * float arithmetic and the fact that a fragment shader runs once per pixel at
 * all.  Both strategies land one to two orders of magnitude off the
 * fixed-function path.  Only a JIT closes that, and a JIT is out of scope.
 *
 * EVERYTHING IS FLOAT
 *
 * Values carry 16 floats plus a type pointer.  Ints and bools live in those
 * floats too.  The places where integer semantics are actually observable —
 * division truncating, the fact that `1/2` is 0 — are handled at the operator,
 * because that is the only place the difference exists.  Storing ints in a
 * separate array would double the branches on every read to save nothing.
 *
 * TYPE CHECKING ALREADY HAPPENED
 *
 * G11a left a type on every node, and this file trusts it.  There are no
 * "what if the operands disagree" branches, because a tree that reaches here
 * has been proved consistent.  What IS checked at run time is everything the
 * type system cannot know: dynamic array indices, loop iteration counts, call
 * depth, and division by zero.
 */

#include <math.h>
#include <string.h>

#include "glsl.h"

/* ============================================================================
 * Interpreter state
 * ==========================================================================*/

/* One live variable.  Storage is a flat float array so that a struct or an
 * array is addressed by offset, exactly like a vector's components — the
 * lvalue machinery then has one representation to deal with instead of three.
 */
#define GLSL_MAX_STORAGE 4096

typedef struct {
    const char        *name;
    const glsl_type_t *type;
    int                offset;   /* into frame storage                     */
    int                size;     /* components                             */
    int                scope;
} rt_var_t;

typedef struct {
    glsl_unit_t *u;
    glsl_env_t  *env;

    rt_var_t     vars[GLSL_MAX_LOCALS];
    int          var_count;
    int          scope;

    float        storage[GLSL_MAX_STORAGE];
    int          storage_used;

    long         iterations;     /* budget across the whole run            */
    int          call_depth;
    int          failed;         /* a runtime limit was hit                */

    /* Control flow in flight.  A flag rather than setjmp: the tree walk has
     * to unwind through C frames anyway, and a flag checked after each
     * statement is both simpler to reason about and impossible to get wrong
     * across an early return. */
    int          returning;
    int          breaking;
    int          continuing;
    int          discarded;

    glsl_value_t return_value;

    /* Scratch for call_user() to park the caller's visible variables in.
     *
     * This used to be a local array, which put 5.9 KB on the C stack per
     * interpreted call frame.  At a nesting depth of 32 that is 189 KB
     * against AuraLite's 64 KB user stack -- so a shader that recursed hit a
     * guard page instead of the interpreter's own depth limit.  Invisible on
     * the host, where the stack is 8 MB, and caught only by running the test
     * suite on the target.
     *
     * One shared buffer works because the frames nest strictly: a call at
     * depth d only ever touches rows [d]. */
    rt_var_t     stash[GLSL_MAX_CALL_DEPTH][GLSL_MAX_LOCALS];
    int          stash_count[GLSL_MAX_CALL_DEPTH];

    /* Argument scratch for evaluating a call.
     *
     * A local `glsl_value_t args[16]` inside eval() cost 1.2 KB on EVERY
     * eval() frame -- not just the ones evaluating a call -- because the
     * compiler reserves it unconditionally.  eval() recurses once per level
     * of expression nesting, so a modest shader burned tens of kilobytes and
     * overflowed AuraLite's 64 KB user stack.
     *
     * Indexed by ARGUMENT NESTING, not by call depth: `max(dot(a,b), 0.0)`
     * evaluates a built-in inside another built-in's argument list, and
     * built-ins do not push a call frame.  Keying on call_depth therefore
     * handed both the same slot and the inner call overwrote the outer's
     * first argument -- which showed up as a Lambert term that ignored its
     * clamp.  Depth is pushed around the argument evaluation instead. */
    glsl_value_t args[GLSL_MAX_ARG_NESTING][GLSL_MAX_ARGS];
    int          arg_depth;

    /* Constructor flattening scratch, off the stack for the same reason and
     * keyed the same way. */
    float        flat[GLSL_MAX_ARG_NESTING][64];
    int          flat_depth;
} exec_t;

static void rt_error(exec_t *ex, int line, const char *msg) {
    if (ex->failed) return;      /* the first failure is the useful one */
    ex->failed = 1;
    glsl_error(ex->u, line, "%s", msg);
}

/* ============================================================================
 * Values
 * ==========================================================================*/

static int ncomp(const glsl_type_t *t) {
    int n = glsl_type_components(t);
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    return n;
}

static glsl_value_t make_value(const glsl_type_t *t) {
    glsl_value_t v;
    memset(&v, 0, sizeof v);
    v.type = t;
    return v;
}

static glsl_value_t make_float(float f) {
    glsl_value_t v = make_value(glsl_type_basic(GLSL_TY_FLOAT, 1));
    v.v[0] = f;
    return v;
}

static glsl_value_t make_bool(int b) {
    glsl_value_t v = make_value(glsl_type_basic(GLSL_TY_BOOL, 1));
    v.v[0] = b ? 1.0f : 0.0f;
    return v;
}

static glsl_value_t make_int(long i) {
    glsl_value_t v = make_value(glsl_type_basic(GLSL_TY_INT, 1));
    v.v[0] = (float)i;
    return v;
}

/* Truncate towards zero, the C and GLSL rule for int conversion. */
static float trunc_to_int(float f) {
    return f < 0.0f ? ceilf(f) : floorf(f);
}

/* ============================================================================
 * Variable storage
 * ==========================================================================*/

static void scope_push(exec_t *ex) { ex->scope++; }

static void scope_pop(exec_t *ex) {
    while (ex->var_count > 0 && ex->vars[ex->var_count - 1].scope >= ex->scope) {
        rt_var_t *v = &ex->vars[ex->var_count - 1];
        /* Storage is a stack: popping a scope reclaims it, so a loop body
         * that declares a variable does not consume storage per iteration. */
        if (v->offset + v->size == ex->storage_used) {
            ex->storage_used = v->offset;
        }
        ex->var_count--;
    }
    ex->scope--;
}

static rt_var_t *find_var(exec_t *ex, const char *name) {
    if (!name) return NULL;
    for (int i = ex->var_count - 1; i >= 0; i--) {
        if (strcmp(ex->vars[i].name, name) == 0) return &ex->vars[i];
    }
    return NULL;
}

static rt_var_t *declare_var(exec_t *ex, const char *name,
                             const glsl_type_t *t, int line) {
    if (ex->var_count >= GLSL_MAX_LOCALS) {
        rt_error(ex, line, "too many live variables");
        return NULL;
    }
    int size = glsl_type_components(t);
    if (size < 1) size = 1;
    if (ex->storage_used + size > GLSL_MAX_STORAGE) {
        rt_error(ex, line, "shader needs too much variable storage");
        return NULL;
    }
    rt_var_t *v = &ex->vars[ex->var_count++];
    v->name   = name;
    v->type   = t;
    v->offset = ex->storage_used;
    v->size   = size;
    v->scope  = ex->scope;
    memset(&ex->storage[v->offset], 0, (size_t)size * sizeof(float));
    ex->storage_used += size;
    return v;
}

/* ============================================================================
 * Lvalues
 *
 * An assignable expression resolves to a base pointer plus a component map:
 * `v.zx = ...` writes components 2 and 0, in that order.  Representing it as
 * an explicit map is what makes swizzled and indexed writes fall out of one
 * code path instead of three.
 * ==========================================================================*/

typedef struct {
    float *base;                 /* into exec storage, or NULL when invalid */
    int    map[16];              /* component indices, in write order       */
    int    count;
    const glsl_type_t *type;
    /* An output the environment owns rather than storage: gl_Position and
     * friends.  Written through the callback at the end of the assignment. */
    const char *ext_name;
    const glsl_type_t *ext_type;  /* the WHOLE variable's type            */
    int         ext_count;        /* its component count                  */
} lvalue_t;

static glsl_value_t eval(exec_t *ex, glsl_node_t *n);

/* Field offset of a struct member, in components. */
static int field_offset(const glsl_type_t *st, const char *name, int *size_out,
                        const glsl_type_t **type_out) {
    int off = 0;
    for (int i = 0; i < st->field_count; i++) {
        int sz = glsl_type_components(st->fields[i].type);
        if (strcmp(st->fields[i].name, name) == 0) {
            if (size_out) *size_out = sz;
            if (type_out) *type_out = st->fields[i].type;
            return off;
        }
        off += sz;
    }
    return -1;
}

/* Map a swizzle string to component indices.  The alphabets were validated by
 * G11a, so only the index arithmetic matters here. */
static int swizzle_map(const char *field, int *map) {
    static const char *sets[3] = { "xyzw", "rgba", "stpq" };
    int n = 0;
    for (const char *c = field; *c && n < 16; c++, n++) {
        int idx = -1;
        for (int k = 0; k < 3 && idx < 0; k++) {
            const char *at = strchr(sets[k], *c);
            if (at) idx = (int)(at - sets[k]);
        }
        if (idx < 0) return 0;
        map[n] = idx;
    }
    return n;
}

static int resolve_lvalue(exec_t *ex, glsl_node_t *n, lvalue_t *out) {
    memset(out, 0, sizeof *out);

    switch (n->kind) {
    case GLSL_NODE_IDENT: {
        rt_var_t *v = find_var(ex, n->v.name);
        if (!v) {
            /* A built-in output such as gl_Position: it has no storage here,
             * so the write goes straight through the environment. */
            out->ext_name  = n->v.name;
            out->ext_type  = n->type;
            out->ext_count = ncomp(n->type);
            out->type = n->type;
            out->count = out->ext_count;
            for (int i = 0; i < out->count; i++) out->map[i] = i;
            return 1;
        }
        out->base = &ex->storage[v->offset];
        out->type = v->type;
        out->count = v->size;
        for (int i = 0; i < v->size && i < 16; i++) out->map[i] = i;
        return 1;
    }

    case GLSL_NODE_FIELD: {
        lvalue_t base;
        if (!resolve_lvalue(ex, n->a, &base)) return 0;

        if (base.type && base.type->kind == GLSL_TY_STRUCT) {
            int sz = 0;
            const glsl_type_t *ft = NULL;
            int off = field_offset(base.type, n->v.name, &sz, &ft);
            if (off < 0) return 0;
            out->base = base.base ? base.base + off : NULL;
            out->ext_name  = base.ext_name;
            out->ext_type  = base.ext_type;
            out->ext_count = base.ext_count;
            out->type = ft;
            out->count = sz;
            for (int i = 0; i < sz && i < 16; i++) out->map[i] = i;
            return 1;
        }

        /* A swizzle: compose the component maps so that `v.xyz.z` lands on
         * the right component of the original. */
        int m[16];
        int n_sw = swizzle_map(n->v.name, m);
        if (n_sw == 0) return 0;
        out->base = base.base;
        out->ext_name  = base.ext_name;
        out->ext_type  = base.ext_type;
        out->ext_count = base.ext_count;
        out->type = n->type;
        out->count = n_sw;
        for (int i = 0; i < n_sw; i++) {
            int src = m[i];
            out->map[i] = (src < base.count) ? base.map[src] : base.map[0];
        }
        return 1;
    }

    case GLSL_NODE_INDEX: {
        lvalue_t base;
        if (!resolve_lvalue(ex, n->a, &base)) return 0;

        glsl_value_t iv = eval(ex, n->b);
        int idx = (int)trunc_to_int(iv.v[0]);

        const glsl_type_t *bt = base.type;
        int stride = 1;
        int bound = 0;

        if (bt && bt->array_len > 0) {
            glsl_type_t elem = *bt;
            elem.array_len = 0;
            stride = glsl_type_components(&elem);
            bound = bt->array_len;
        } else if (bt && bt->kind == GLSL_TY_MAT) {
            stride = bt->rows;               /* one column */
            bound  = bt->rows;
        } else if (bt) {
            stride = 1;
            bound  = bt->rows > 0 ? bt->rows : 1;
        }

        /* A dynamic index is the one thing the type checker could not bound.
         * GLSL leaves out-of-range behaviour undefined; clamping is the
         * cheapest defined answer and cannot read outside the value. */
        if (idx < 0) idx = 0;
        if (bound > 0 && idx >= bound) idx = bound - 1;

        out->base = base.base;
        out->ext_name  = base.ext_name;
        out->ext_type  = base.ext_type;
        out->ext_count = base.ext_count;
        out->type = n->type;
        out->count = stride;
        for (int i = 0; i < stride && i < 16; i++) {
            int src = idx * stride + i;
            out->map[i] = (src < base.count) ? base.map[src] : base.map[0];
        }
        return 1;
    }

    default:
        return 0;
    }
}

/* Read through an lvalue. */
static glsl_value_t lvalue_read(exec_t *ex, const lvalue_t *lv) {
    glsl_value_t out = make_value(lv->type);

    if (!lv->base) {
        /* An environment-owned variable: ask for the whole thing, then pick
         * out the components this lvalue names. */
        glsl_value_t whole = make_value(lv->ext_type ? lv->ext_type : lv->type);
        if (lv->ext_name && ex->env && ex->env->read_var) {
            ex->env->read_var(ex->env, lv->ext_name, &whole);
        }
        for (int i = 0; i < lv->count && i < 16; i++) {
            out.v[i] = whole.v[lv->map[i] < 16 ? lv->map[i] : 0];
        }
        return out;
    }

    for (int i = 0; i < lv->count && i < 16; i++) out.v[i] = lv->base[lv->map[i]];
    return out;
}

static void lvalue_write(exec_t *ex, const lvalue_t *lv,
                         const glsl_value_t *val) {
    if (!lv->base) {
        if (!lv->ext_name || !ex->env || !ex->env->write_var) return;

        /* A partial write to an environment variable — gl_Position.xyz — has
         * to read, merge and write back, since the callback owns the whole
         * value.  Full writes are the common case and skip the read.
         *
         * "Full" is measured against the WHOLE variable, not against this
         * lvalue's type: for gl_Position.xy the lvalue type is vec2 and its
         * count is 2, so comparing the two would call every partial write
         * complete and silently drop z and w. */
        int whole_n = lv->ext_count > 0 ? lv->ext_count : ncomp(lv->type);
        int full = (lv->count == whole_n);
        int identity = 1;
        for (int i = 0; i < lv->count; i++) {
            if (lv->map[i] != i) { identity = 0; break; }
        }

        if (full && identity) {
            ex->env->write_var(ex->env, lv->ext_name, val);
            return;
        }

        glsl_value_t whole = make_value(lv->ext_type ? lv->ext_type : lv->type);
        if (ex->env->read_var) {
            ex->env->read_var(ex->env, lv->ext_name, &whole);
        }
        for (int i = 0; i < lv->count && i < 16; i++) {
            int dst = lv->map[i];
            if (dst >= 0 && dst < 16) whole.v[dst] = val->v[i];
        }
        ex->env->write_var(ex->env, lv->ext_name, &whole);
        return;
    }

    for (int i = 0; i < lv->count && i < 16; i++) {
        lv->base[lv->map[i]] = val->v[i];
    }
}

/* ============================================================================
 * Built-in functions
 * ==========================================================================*/

/* Apply a one-argument float function to every component. */
static glsl_value_t map1(const glsl_value_t *a, float (*f)(float)) {
    glsl_value_t r = *a;
    int n = ncomp(a->type);
    for (int i = 0; i < n; i++) r.v[i] = f(a->v[i]);
    return r;
}

static float f_radians(float d) { return d * 0.017453292519943295f; }
static float f_degrees(float r) { return r * 57.29577951308232f; }
static float f_fract(float x)   { return x - floorf(x); }
static float f_sign(float x)    { return x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f); }
static float f_exp2(float x)    { return powf(2.0f, x); }
static float f_log2(float x)    { return x > 0.0f ? logf(x) / 0.6931471805599453f : 0.0f; }
static float f_sqrt(float x)    { return x > 0.0f ? sqrtf(x) : 0.0f; }
static float f_isqrt(float x)   { return x > 0.0f ? 1.0f / sqrtf(x) : 0.0f; }
static float f_log(float x)     { return x > 0.0f ? logf(x) : 0.0f; }

/* Two-argument, component-wise, with the second operand allowed to be a
 * scalar broadcast over the first — which is what makes `min(v, 0.0)` work. */
static glsl_value_t map2(const glsl_value_t *a, const glsl_value_t *b,
                         float (*f)(float, float)) {
    int na = ncomp(a->type), nb = ncomp(b->type);
    const glsl_value_t *shape = (na >= nb) ? a : b;
    int n = ncomp(shape->type);

    glsl_value_t r = *shape;
    for (int i = 0; i < n; i++) {
        float x = a->v[na == 1 ? 0 : i];
        float y = b->v[nb == 1 ? 0 : i];
        r.v[i] = f(x, y);
    }
    return r;
}

static float f_min(float a, float b)  { return a < b ? a : b; }
static float f_max(float a, float b)  { return a > b ? a : b; }
static float f_step(float e, float x) { return x < e ? 0.0f : 1.0f; }
static float f_pow(float a, float b)  { return powf(a, b); }
static float f_mod(float a, float b)  {
    /* GLSL's mod is a - b*floor(a/b): the result takes the SIGN OF THE
     * DIVISOR, unlike C's fmodf which takes the sign of the dividend.
     * mod(-1.0, 3.0) is 2.0 in GLSL and -1.0 in C, and shaders that wrap a
     * coordinate rely on the GLSL answer. */
    if (b == 0.0f) return 0.0f;
    return a - b * floorf(a / b);
}
static float f_atan2(float y, float x) { return atan2f(y, x); }

static float dot_of(const glsl_value_t *a, const glsl_value_t *b) {
    int n = ncomp(a->type);
    float s = 0.0f;
    for (int i = 0; i < n; i++) s += a->v[i] * b->v[i];
    return s;
}

static float length_of(const glsl_value_t *a) {
    return sqrtf(dot_of(a, a));
}

/* Dispatch a built-in by name.  Returns 1 when handled.
 *
 * A name-keyed switch rather than an index assigned at check time: the symbol
 * table in G11a is the compiler's, not the runtime's, and threading an ID
 * through would couple the two phases for a strcmp on a call that already
 * costs several float operations. */
static int call_builtin(exec_t *ex, const char *name, glsl_value_t *args,
                        int argc, int line, glsl_value_t *out) {
    #define A0 (&args[0])
    #define A1 (&args[1])
    #define A2 (&args[2])

    if (argc >= 1) {
        if (!strcmp(name, "radians"))     { *out = map1(A0, f_radians); return 1; }
        if (!strcmp(name, "degrees"))     { *out = map1(A0, f_degrees); return 1; }
        if (!strcmp(name, "sin"))         { *out = map1(A0, sinf);   return 1; }
        if (!strcmp(name, "cos"))         { *out = map1(A0, cosf);   return 1; }
        if (!strcmp(name, "tan"))         { *out = map1(A0, tanf);   return 1; }
        if (!strcmp(name, "asin"))        { *out = map1(A0, asinf);  return 1; }
        if (!strcmp(name, "acos"))        { *out = map1(A0, acosf);  return 1; }
        if (!strcmp(name, "exp"))         { *out = map1(A0, expf);   return 1; }
        if (!strcmp(name, "log"))         { *out = map1(A0, f_log);  return 1; }
        if (!strcmp(name, "exp2"))        { *out = map1(A0, f_exp2); return 1; }
        if (!strcmp(name, "log2"))        { *out = map1(A0, f_log2); return 1; }
        if (!strcmp(name, "sqrt"))        { *out = map1(A0, f_sqrt); return 1; }
        if (!strcmp(name, "inversesqrt")) { *out = map1(A0, f_isqrt);return 1; }
        if (!strcmp(name, "abs"))         { *out = map1(A0, fabsf);  return 1; }
        if (!strcmp(name, "sign"))        { *out = map1(A0, f_sign); return 1; }
        if (!strcmp(name, "floor"))       { *out = map1(A0, floorf); return 1; }
        if (!strcmp(name, "ceil"))        { *out = map1(A0, ceilf);  return 1; }
        if (!strcmp(name, "fract"))       { *out = map1(A0, f_fract);return 1; }

        if (!strcmp(name, "atan")) {
            /* atan has one- and two-argument forms; GLSL overloads the name. */
            if (argc == 1) { *out = map1(A0, atanf); return 1; }
            *out = map2(A0, A1, f_atan2);
            return 1;
        }

        if (!strcmp(name, "length")) {
            *out = make_float(length_of(A0));
            return 1;
        }

        if (!strcmp(name, "normalize")) {
            float len = length_of(A0);
            glsl_value_t r = *A0;
            int n = ncomp(A0->type);
            /* Normalising a zero vector is undefined in GLSL.  Returning zero
             * is defined, finite, and never propagates a NaN into a colour --
             * which a division by zero would, turning one bad vertex into a
             * black or garbage pixel. */
            if (len > 1e-20f) {
                for (int i = 0; i < n; i++) r.v[i] = A0->v[i] / len;
            } else {
                for (int i = 0; i < n; i++) r.v[i] = 0.0f;
            }
            *out = r;
            return 1;
        }

        if (!strcmp(name, "any") || !strcmp(name, "all")) {
            int n = ncomp(A0->type);
            int want_all = (name[0] == 'a' && name[1] == 'l');
            int acc = want_all ? 1 : 0;
            for (int i = 0; i < n; i++) {
                int b = (A0->v[i] != 0.0f);
                if (want_all) acc = acc && b;
                else          acc = acc || b;
            }
            *out = make_bool(acc);
            return 1;
        }

        if (!strcmp(name, "not")) {
            glsl_value_t r = *A0;
            int n = ncomp(A0->type);
            for (int i = 0; i < n; i++) r.v[i] = (A0->v[i] != 0.0f) ? 0.0f : 1.0f;
            *out = r;
            return 1;
        }
    }

    if (argc >= 2) {
        if (!strcmp(name, "pow"))  { *out = map2(A0, A1, f_pow); return 1; }
        if (!strcmp(name, "mod"))  { *out = map2(A0, A1, f_mod); return 1; }
        if (!strcmp(name, "min"))  { *out = map2(A0, A1, f_min); return 1; }
        if (!strcmp(name, "max"))  { *out = map2(A0, A1, f_max); return 1; }
        if (!strcmp(name, "step")) { *out = map2(A0, A1, f_step); return 1; }

        if (!strcmp(name, "dot")) {
            *out = make_float(dot_of(A0, A1));
            return 1;
        }

        if (!strcmp(name, "distance")) {
            glsl_value_t d = *A0;
            int n = ncomp(A0->type);
            for (int i = 0; i < n; i++) d.v[i] = A0->v[i] - A1->v[i];
            *out = make_float(length_of(&d));
            return 1;
        }

        if (!strcmp(name, "cross")) {
            glsl_value_t r = make_value(glsl_type_basic(GLSL_TY_VEC, 3));
            r.v[0] = A0->v[1]*A1->v[2] - A0->v[2]*A1->v[1];
            r.v[1] = A0->v[2]*A1->v[0] - A0->v[0]*A1->v[2];
            r.v[2] = A0->v[0]*A1->v[1] - A0->v[1]*A1->v[0];
            *out = r;
            return 1;
        }

        if (!strcmp(name, "reflect")) {
            /* I - 2*dot(N,I)*N */
            float d = dot_of(A1, A0);
            glsl_value_t r = *A0;
            int n = ncomp(A0->type);
            for (int i = 0; i < n; i++) r.v[i] = A0->v[i] - 2.0f * d * A1->v[i];
            *out = r;
            return 1;
        }

        if (!strcmp(name, "matrixCompMult")) {
            glsl_value_t r = *A0;
            int n = ncomp(A0->type);
            for (int i = 0; i < n; i++) r.v[i] = A0->v[i] * A1->v[i];
            *out = r;
            return 1;
        }

        /* Vector relational functions all share one shape. */
        {
            int rel = 0;
            if      (!strcmp(name, "lessThan"))         rel = 1;
            else if (!strcmp(name, "lessThanEqual"))    rel = 2;
            else if (!strcmp(name, "greaterThan"))      rel = 3;
            else if (!strcmp(name, "greaterThanEqual")) rel = 4;
            else if (!strcmp(name, "equal"))            rel = 5;
            else if (!strcmp(name, "notEqual"))         rel = 6;
            if (rel) {
                int n = ncomp(A0->type);
                glsl_value_t r = make_value(
                    glsl_type_basic(GLSL_TY_BVEC, n));
                for (int i = 0; i < n; i++) {
                    float x = A0->v[i], y = A1->v[i];
                    int b = 0;
                    switch (rel) {
                    case 1: b = x <  y; break;
                    case 2: b = x <= y; break;
                    case 3: b = x >  y; break;
                    case 4: b = x >= y; break;
                    case 5: b = x == y; break;
                    default: b = x != y; break;
                    }
                    r.v[i] = b ? 1.0f : 0.0f;
                }
                *out = r;
                return 1;
            }
        }

        /* Texture sampling goes out through the environment. */
        if (!strcmp(name, "texture2D") || !strcmp(name, "texture2DProj") ||
            !strcmp(name, "texture2DLod") || !strcmp(name, "textureCube") ||
            !strcmp(name, "textureCubeLod")) {
            int is_cube = (name[7] == 'C');
            float coord[3] = { 0.0f, 0.0f, 0.0f };
            int ncoord = is_cube ? 3 : 2;

            if (!strcmp(name, "texture2DProj")) {
                /* The coordinate is divided by its last component (§8.7),
                 * which is what makes projective texturing work. */
                float w = A1->v[3] != 0.0f ? A1->v[3] : 1.0f;
                coord[0] = A1->v[0] / w;
                coord[1] = A1->v[1] / w;
            } else {
                for (int i = 0; i < ncoord; i++) coord[i] = A1->v[i];
            }

            float rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            if (ex->env && ex->env->sample) {
                ex->env->sample(ex->env, (int)A0->v[0], is_cube,
                                coord, ncoord, rgba);
            }
            glsl_value_t r = make_value(glsl_type_basic(GLSL_TY_VEC, 4));
            for (int i = 0; i < 4; i++) r.v[i] = rgba[i];
            *out = r;
            return 1;
        }
    }

    if (argc >= 3) {
        if (!strcmp(name, "clamp")) {
            int n = ncomp(A0->type);
            int n1 = ncomp(A1->type), n2 = ncomp(A2->type);
            glsl_value_t r = *A0;
            for (int i = 0; i < n; i++) {
                float lo = A1->v[n1 == 1 ? 0 : i];
                float hi = A2->v[n2 == 1 ? 0 : i];
                float x  = A0->v[i];
                r.v[i] = x < lo ? lo : (x > hi ? hi : x);
            }
            *out = r;
            return 1;
        }

        if (!strcmp(name, "mix")) {
            int n = ncomp(A0->type);
            int n2 = ncomp(A2->type);
            glsl_value_t r = *A0;
            for (int i = 0; i < n; i++) {
                float t = A2->v[n2 == 1 ? 0 : i];
                r.v[i] = A0->v[i] * (1.0f - t) + A1->v[i] * t;
            }
            *out = r;
            return 1;
        }

        if (!strcmp(name, "smoothstep")) {
            int n = ncomp(A2->type);
            int n0 = ncomp(A0->type), n1 = ncomp(A1->type);
            glsl_value_t r = *A2;
            for (int i = 0; i < n; i++) {
                float e0 = A0->v[n0 == 1 ? 0 : i];
                float e1 = A1->v[n1 == 1 ? 0 : i];
                float x  = A2->v[i];
                float t  = (e1 == e0) ? 0.0f : (x - e0) / (e1 - e0);
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                r.v[i] = t * t * (3.0f - 2.0f * t);
            }
            *out = r;
            return 1;
        }

        if (!strcmp(name, "faceforward")) {
            /* dot(Nref, I) < 0 ? N : -N */
            float d = dot_of(A2, A1);
            glsl_value_t r = *A0;
            int n = ncomp(A0->type);
            if (d >= 0.0f) {
                for (int i = 0; i < n; i++) r.v[i] = -A0->v[i];
            }
            *out = r;
            return 1;
        }

        if (!strcmp(name, "refract")) {
            float eta = A2->v[0];
            float d = dot_of(A1, A0);
            float k = 1.0f - eta * eta * (1.0f - d * d);
            glsl_value_t r = *A0;
            int n = ncomp(A0->type);
            if (k < 0.0f) {
                /* Total internal reflection: the specification says the
                 * result is zero, not a NaN from sqrt of a negative. */
                for (int i = 0; i < n; i++) r.v[i] = 0.0f;
            } else {
                float s = eta * d + sqrtf(k);
                for (int i = 0; i < n; i++) {
                    r.v[i] = eta * A0->v[i] - s * A1->v[i];
                }
            }
            *out = r;
            return 1;
        }
    }

    #undef A0
    #undef A1
    #undef A2
    rt_error(ex, line, "unimplemented built-in function");
    return 0;
}

/* ============================================================================
 * Expression evaluation
 * ==========================================================================*/

static void exec_stmt(exec_t *ex, glsl_node_t *n);

/* Find a user function by name in the compiled unit. */
static glsl_node_t *find_function(exec_t *ex, const char *name) {
    if (!name || !ex->u->root) return NULL;
    for (glsl_node_t *f = ex->u->root->list; f; f = f->next) {
        if (f->kind == GLSL_NODE_FUNCTION && f->body && f->v.name &&
            strcmp(f->v.name, name) == 0) {
            return f;
        }
    }
    return NULL;
}

static glsl_value_t call_user(exec_t *ex, glsl_node_t *fn, glsl_value_t *args,
                              int argc, glsl_node_t *call);

/* Matrix times vector.  Matrices are COLUMN-major, matching GLSL and
 * matching glUniformMatrix, so element (row r, column c) is at c*n + r. */
static glsl_value_t mat_mul_vec(const glsl_value_t *m, const glsl_value_t *v,
                                int n) {
    glsl_value_t r = make_value(glsl_type_basic(GLSL_TY_VEC, n));
    for (int row = 0; row < n; row++) {
        float s = 0.0f;
        for (int col = 0; col < n; col++) s += m->v[col * n + row] * v->v[col];
        r.v[row] = s;
    }
    return r;
}

static glsl_value_t vec_mul_mat(const glsl_value_t *v, const glsl_value_t *m,
                                int n) {
    /* v * M is the row-vector product: component c is dot(v, column c). */
    glsl_value_t r = make_value(glsl_type_basic(GLSL_TY_VEC, n));
    for (int col = 0; col < n; col++) {
        float s = 0.0f;
        for (int row = 0; row < n; row++) s += v->v[row] * m->v[col * n + row];
        r.v[col] = s;
    }
    return r;
}

static glsl_value_t mat_mul_mat(const glsl_value_t *a, const glsl_value_t *b,
                                int n) {
    glsl_value_t r = make_value(glsl_type_basic(GLSL_TY_MAT, n));
    for (int col = 0; col < n; col++) {
        for (int row = 0; row < n; row++) {
            float s = 0.0f;
            for (int k = 0; k < n; k++) {
                s += a->v[k * n + row] * b->v[col * n + k];
            }
            r.v[col * n + row] = s;
        }
    }
    return r;
}

static glsl_value_t eval_binary(exec_t *ex, glsl_node_t *n) {
    if (n->op == GLSL_TOK_AND_AND) {
        /* Short-circuit: the right side must not run when the left decides
         * the answer, because it may contain a call with side effects. */
        glsl_value_t a = eval(ex, n->a);
        if (a.v[0] == 0.0f) return make_bool(0);
        glsl_value_t b = eval(ex, n->b);
        return make_bool(b.v[0] != 0.0f);
    }
    if (n->op == GLSL_TOK_OR_OR) {
        glsl_value_t a = eval(ex, n->a);
        if (a.v[0] != 0.0f) return make_bool(1);
        glsl_value_t b = eval(ex, n->b);
        return make_bool(b.v[0] != 0.0f);
    }

    glsl_value_t a = eval(ex, n->a);
    glsl_value_t b = eval(ex, n->b);

    switch (n->op) {
    case GLSL_TOK_COMMA:
        return b;

    case GLSL_TOK_XOR_XOR:
        return make_bool((a.v[0] != 0.0f) != (b.v[0] != 0.0f));

    case GLSL_TOK_EQ: case GLSL_TOK_NE: {
        int na = ncomp(a.type);
        int same = 1;
        for (int i = 0; i < na; i++) {
            if (a.v[i] != b.v[i]) { same = 0; break; }
        }
        return make_bool(n->op == GLSL_TOK_EQ ? same : !same);
    }

    case GLSL_TOK_LT: return make_bool(a.v[0] <  b.v[0]);
    case GLSL_TOK_GT: return make_bool(a.v[0] >  b.v[0]);
    case GLSL_TOK_LE: return make_bool(a.v[0] <= b.v[0]);
    case GLSL_TOK_GE: return make_bool(a.v[0] >= b.v[0]);

    default:
        break;
    }

    /* Matrix products, which the type checker already sized. */
    if (n->op == GLSL_TOK_STAR) {
        if (a.type->kind == GLSL_TY_MAT && b.type->kind == GLSL_TY_VEC) {
            return mat_mul_vec(&a, &b, a.type->rows);
        }
        if (a.type->kind == GLSL_TY_VEC && b.type->kind == GLSL_TY_MAT) {
            return vec_mul_mat(&a, &b, b.type->rows);
        }
        if (a.type->kind == GLSL_TY_MAT && b.type->kind == GLSL_TY_MAT) {
            return mat_mul_mat(&a, &b, a.type->rows);
        }
    }

    /* Component-wise, with a scalar broadcast over the aggregate. */
    int na = ncomp(a.type), nb = ncomp(b.type);
    const glsl_type_t *rt = n->type ? n->type : (na >= nb ? a.type : b.type);
    glsl_value_t r = make_value(rt);
    int nr = ncomp(rt);
    int is_int = glsl_type_is_int_based(rt);

    for (int i = 0; i < nr; i++) {
        float x = a.v[na == 1 ? 0 : i];
        float y = b.v[nb == 1 ? 0 : i];
        float z;
        switch (n->op) {
        case GLSL_TOK_PLUS:  z = x + y; break;
        case GLSL_TOK_MINUS: z = x - y; break;
        case GLSL_TOK_STAR:  z = x * y; break;
        case GLSL_TOK_SLASH:
            if (y == 0.0f) {
                /* Division by zero is undefined in GLSL and produces an
                 * infinity in hardware.  Zero is defined and finite, and it
                 * keeps a NaN out of the framebuffer -- one bad fragment
                 * should not poison a blend. */
                z = 0.0f;
            } else {
                z = x / y;
                /* Integer division truncates towards zero: 7/2 is 3, and
                 * -7/2 is -3.  Doing this in float and truncating gives the
                 * same answer without a separate integer path. */
                if (is_int) z = trunc_to_int(z);
            }
            break;
        default: z = 0.0f; break;
        }
        r.v[i] = z;
    }
    return r;
}

/* Build a value from constructor arguments: flatten every argument's
 * components into a stream and take as many as the target needs. */
static glsl_value_t eval_constructor(exec_t *ex, glsl_node_t *n) {
    const glsl_type_t *t = n->decl_type;
    glsl_value_t r = make_value(t);
    int want = ncomp(t);

    /* Gather.  The scratch is indexed by nesting depth because a constructor
     * argument may itself be a constructor: vec4(vec2(1.0), vec2(2.0)). */
    int fdepth = ex->flat_depth;
    if (fdepth < 0 || fdepth >= GLSL_MAX_ARG_NESTING) {
        rt_error(ex, n->line, "constructor nesting too deep");
        return r;
    }
    ex->flat_depth++;
    float *flat = ex->flat[fdepth];
    int have = 0;
    for (glsl_node_t *a = n->list; a && have < 64; a = a->next) {
        glsl_value_t av = eval(ex, a);
        int na = ncomp(av.type);
        for (int i = 0; i < na && have < 64; i++) flat[have++] = av.v[i];
    }
    ex->flat_depth--;
    if (have == 0) return r;

    /* A single scalar fills a vector, or a matrix's DIAGONAL — mat4(1.0) is
     * the identity, not a matrix of ones.  This is the constructor rule that
     * silently produces a wrong scene if it is missed. */
    if (have == 1) {
        if (t->kind == GLSL_TY_MAT) {
            int m = t->rows;
            for (int c = 0; c < m; c++) {
                for (int row = 0; row < m; row++) {
                    r.v[c * m + row] = (c == row) ? flat[0] : 0.0f;
                }
            }
        } else {
            for (int i = 0; i < want; i++) r.v[i] = flat[0];
        }
    } else if (t->kind == GLSL_TY_MAT && n->list && !n->list->next &&
               n->list->type && n->list->type->kind == GLSL_TY_MAT) {
        /* mat3(mat4): copy the overlapping top-left block, identity
         * elsewhere. */
        int dst = t->rows;
        int src = n->list->type->rows;
        for (int c = 0; c < dst; c++) {
            for (int row = 0; row < dst; row++) {
                float val;
                if (c < src && row < src) val = flat[c * src + row];
                else                      val = (c == row) ? 1.0f : 0.0f;
                r.v[c * dst + row] = val;
            }
        }
    } else {
        for (int i = 0; i < want && i < have; i++) r.v[i] = flat[i];
    }

    /* Conversions: int(1.7) truncates, bool(x) is x != 0.  A vector
     * constructor converts every component the same way. */
    if (glsl_type_is_int_based(t)) {
        for (int i = 0; i < want; i++) r.v[i] = trunc_to_int(r.v[i]);
    } else if (glsl_type_is_bool_based(t)) {
        for (int i = 0; i < want; i++) r.v[i] = (r.v[i] != 0.0f) ? 1.0f : 0.0f;
    }
    return r;
}

static glsl_value_t eval(exec_t *ex, glsl_node_t *n) {
    if (!n || ex->failed) return make_float(0.0f);

    switch (n->kind) {
    case GLSL_NODE_INT_LIT:   return make_int(n->v.ival);
    case GLSL_NODE_FLOAT_LIT: return make_float((float)n->v.fval);
    case GLSL_NODE_BOOL_LIT:  return make_bool((int)n->v.ival);

    case GLSL_NODE_IDENT: {
        rt_var_t *v = find_var(ex, n->v.name);
        if (v) {
            glsl_value_t r = make_value(v->type);
            for (int i = 0; i < v->size && i < 16; i++) {
                r.v[i] = ex->storage[v->offset + i];
            }
            return r;
        }
        /* Not a local: an attribute, uniform, varying or built-in, all of
         * which the environment owns. */
        glsl_value_t r = make_value(n->type);
        if (ex->env && ex->env->read_var) {
            ex->env->read_var(ex->env, n->v.name, &r);
            r.type = n->type;
        }
        return r;
    }

    case GLSL_NODE_UNARY: {
        if (n->op == GLSL_TOK_INC || n->op == GLSL_TOK_DEC) {
            lvalue_t lv;
            if (!resolve_lvalue(ex, n->a, &lv)) return make_float(0.0f);
            glsl_value_t cur = lvalue_read(ex, &lv);
            int nn = ncomp(cur.type);
            float d = (n->op == GLSL_TOK_INC) ? 1.0f : -1.0f;
            for (int i = 0; i < nn; i++) cur.v[i] += d;
            lvalue_write(ex, &lv, &cur);
            return cur;              /* pre-increment yields the NEW value */
        }

        glsl_value_t a = eval(ex, n->a);
        if (n->op == GLSL_TOK_PLUS) return a;
        if (n->op == GLSL_TOK_BANG) return make_bool(a.v[0] == 0.0f);

        glsl_value_t r = a;
        int nn = ncomp(a.type);
        for (int i = 0; i < nn; i++) r.v[i] = -a.v[i];
        return r;
    }

    case GLSL_NODE_POSTFIX: {
        lvalue_t lv;
        if (!resolve_lvalue(ex, n->a, &lv)) return make_float(0.0f);
        glsl_value_t before = lvalue_read(ex, &lv);
        glsl_value_t after = before;
        int nn = ncomp(after.type);
        float d = (n->op == GLSL_TOK_INC) ? 1.0f : -1.0f;
        for (int i = 0; i < nn; i++) after.v[i] += d;
        lvalue_write(ex, &lv, &after);
        return before;               /* post-increment yields the OLD value */
    }

    case GLSL_NODE_BINARY:
        return eval_binary(ex, n);

    case GLSL_NODE_ASSIGN: {
        glsl_value_t rhs = eval(ex, n->b);

        lvalue_t lv;
        if (!resolve_lvalue(ex, n->a, &lv)) return rhs;

        if (n->op != GLSL_TOK_ASSIGN) {
            glsl_value_t cur = lvalue_read(ex, &lv);

            /* `v *= m` is a matrix transform, not a component-wise multiply,
             * and it is the only compound assignment that changes shape
             * partway through. */
            if (n->op == GLSL_TOK_MUL_ASSIGN &&
                cur.type->kind == GLSL_TY_VEC &&
                rhs.type->kind == GLSL_TY_MAT) {
                cur = vec_mul_mat(&cur, &rhs, rhs.type->rows);
            } else if (n->op == GLSL_TOK_MUL_ASSIGN &&
                       cur.type->kind == GLSL_TY_MAT &&
                       rhs.type->kind == GLSL_TY_MAT) {
                cur = mat_mul_mat(&cur, &rhs, cur.type->rows);
            } else {
                int nc = ncomp(cur.type), nr = ncomp(rhs.type);
                int is_int = glsl_type_is_int_based(cur.type);
                for (int i = 0; i < nc; i++) {
                    float y = rhs.v[nr == 1 ? 0 : i];
                    switch (n->op) {
                    case GLSL_TOK_ADD_ASSIGN: cur.v[i] += y; break;
                    case GLSL_TOK_SUB_ASSIGN: cur.v[i] -= y; break;
                    case GLSL_TOK_MUL_ASSIGN: cur.v[i] *= y; break;
                    case GLSL_TOK_DIV_ASSIGN:
                        if (y == 0.0f) cur.v[i] = 0.0f;
                        else {
                            cur.v[i] /= y;
                            if (is_int) cur.v[i] = trunc_to_int(cur.v[i]);
                        }
                        break;
                    default: break;
                    }
                }
            }
            rhs = cur;
        }

        lvalue_write(ex, &lv, &rhs);
        return rhs;
    }

    case GLSL_NODE_CONDITIONAL: {
        glsl_value_t c = eval(ex, n->a);
        /* Only the taken branch runs: the other may divide by zero or call
         * something expensive, and GLSL guarantees it is not evaluated. */
        return (c.v[0] != 0.0f) ? eval(ex, n->b) : eval(ex, n->c);
    }

    case GLSL_NODE_FIELD: {
        /* A struct can also exceed 16 components, so a member read goes
         * through storage when it can, for the same reason as an index. */
        if (n->a && n->a->type && n->a->type->kind == GLSL_TY_STRUCT) {
            lvalue_t lv;
            if (resolve_lvalue(ex, n, &lv)) return lvalue_read(ex, &lv);
        }

        glsl_value_t base = eval(ex, n->a);

        if (base.type && base.type->kind == GLSL_TY_STRUCT) {
            int sz = 0;
            const glsl_type_t *ft = NULL;
            int off = field_offset(base.type, n->v.name, &sz, &ft);
            if (off < 0) return make_float(0.0f);
            glsl_value_t r = make_value(ft);
            for (int i = 0; i < sz && i < 16; i++) r.v[i] = base.v[off + i];
            return r;
        }

        int m[16];
        int len = swizzle_map(n->v.name, m);
        glsl_value_t r = make_value(n->type);
        for (int i = 0; i < len && i < 16; i++) {
            r.v[i] = base.v[m[i] < 16 ? m[i] : 0];
        }
        return r;
    }

    case GLSL_NODE_INDEX: {
        /* An ARRAY does not fit in a glsl_value_t: `vec4 a[8]` is 32
         * components and the value type holds 16.  Reading the element
         * through the lvalue path addresses storage directly and never
         * materialises the whole array -- which is both correct and cheaper.
         *
         * Copying the array into a temporary was the original approach and it
         * silently truncated: every element past the sixteenth component read
         * as zero, and a[1] on a short array aliased a[0].  Both failures are
         * invisible without a numeric test. */
        lvalue_t lv;
        if (resolve_lvalue(ex, n, &lv)) return lvalue_read(ex, &lv);

        /* Not addressable -- indexing a temporary, such as f().xyz[1].  The
         * value fits by construction in that case, since a function cannot
         * return an array in GLSL ES 1.0. */
        glsl_value_t base = eval(ex, n->a);
        glsl_value_t iv = eval(ex, n->b);
        int idx = (int)trunc_to_int(iv.v[0]);

        const glsl_type_t *bt = base.type;
        int stride = 1, bound = 1;
        if (bt && bt->kind == GLSL_TY_MAT) {
            stride = bt->rows;
            bound  = bt->rows;
        } else if (bt) {
            stride = 1;
            bound  = bt->rows > 0 ? bt->rows : 1;
        }

        if (idx < 0) idx = 0;
        if (idx >= bound) idx = bound - 1;

        glsl_value_t r = make_value(n->type);
        for (int i = 0; i < stride && i < 16; i++) {
            int src = idx * stride + i;
            r.v[i] = (src < 16) ? base.v[src] : 0.0f;
        }
        return r;
    }

    case GLSL_NODE_CALL: {
        if (n->decl_type) return eval_constructor(ex, n);

        int slot = ex->arg_depth;
        if (slot < 0 || slot >= GLSL_MAX_ARG_NESTING) {
            rt_error(ex, n->line, "call arguments nested too deeply");
            return make_float(0.0f);
        }
        glsl_value_t *args = ex->args[slot];

        /* Claim the slot for the whole argument evaluation, so any call
         * inside an argument gets the next one down. */
        ex->arg_depth++;
        int argc = 0;
        for (glsl_node_t *a = n->list; a && argc < GLSL_MAX_ARGS; a = a->next) {
            args[argc++] = eval(ex, a);
        }
        ex->arg_depth--;

        glsl_node_t *fn = find_function(ex, n->v.name);
        if (fn) return call_user(ex, fn, args, argc, n);

        glsl_value_t out = make_value(n->type);
        if (!call_builtin(ex, n->v.name ? n->v.name : "", args, argc,
                          n->line, &out)) {
            return make_float(0.0f);
        }
        return out;
    }

    default:
        return make_float(0.0f);
    }
}

/* ============================================================================
 * Calling a user function
 * ==========================================================================*/

static glsl_value_t call_user(exec_t *ex, glsl_node_t *fn, glsl_value_t *args,
                              int argc, glsl_node_t *call) {
    if (ex->call_depth >= GLSL_MAX_CALL_DEPTH) {
        /* GLSL ES 1.0 forbids recursion, so this only fires on a shader doing
         * something the specification does not allow -- but it must fire,
         * because the alternative is a stack overflow in user mode. */
        rt_error(ex, call->line, "call depth exceeded (recursion is not "
                                 "allowed in GLSL ES)");
        return make_float(0.0f);
    }

    /* A function body sees its parameters and the globals, NOT the caller's
     * locals.  Saving and restoring the visible variable count is what
     * enforces that; the globals sit below the mark and stay visible. */
    int saved_count   = ex->var_count;
    int saved_storage = ex->storage_used;
    int saved_scope   = ex->scope;

    /* Hide the caller's locals by remembering where the globals end.  Globals
     * were declared at scope 1, so anything above that belongs to a caller. */
    int hidden_from = 0;
    for (int i = 0; i < ex->var_count; i++) {
        if (ex->vars[i].scope > 1) { hidden_from = i; break; }
        hidden_from = i + 1;
    }
    int moved = ex->var_count - hidden_from;
    int slot = ex->call_depth;                   /* before the increment */
    if (moved > 0 && slot >= 0 && slot < GLSL_MAX_CALL_DEPTH) {
        memcpy(ex->stash[slot], &ex->vars[hidden_from],
               (size_t)moved * sizeof(rt_var_t));
        ex->stash_count[slot] = moved;
        ex->var_count = hidden_from;
    } else {
        moved = 0;
    }

    ex->call_depth++;
    ex->scope = 2;

    for (int i = 0; i < fn->param_count && i < argc; i++) {
        if (!fn->params || !fn->params[i].name) continue;
        rt_var_t *pv = declare_var(ex, fn->params[i].name,
                                   fn->params[i].type, call->line);
        if (!pv) break;
        /* Even an `in` parameter is a local copy the callee may assign to. */
        int nn = ncomp(fn->params[i].type);
        for (int k = 0; k < nn && k < pv->size; k++) {
            ex->storage[pv->offset + k] = args[i].v[k];
        }
    }

    int saved_returning = ex->returning;
    ex->returning = 0;
    ex->return_value = make_value(fn->decl_type);

    exec_stmt(ex, fn->body);

    glsl_value_t result = ex->return_value;
    result.type = fn->decl_type;

    /* Copy `out` and `inout` parameters back into the caller's lvalues.  This
     * has to happen while the callee's frame is still live, since that is
     * where the written values are. */
    {
        int i = 0;
        for (glsl_node_t *a = call->list;
             a && i < fn->param_count && i < argc; a = a->next, i++) {
            if (!fn->params) break;
            glsl_qualifier_t q = fn->params[i].qual;
            if (q != GLSL_Q_PARAM_OUT && q != GLSL_Q_PARAM_INOUT) continue;
            rt_var_t *pv = find_var(ex, fn->params[i].name);
            if (!pv) continue;
            glsl_value_t back = make_value(pv->type);
            for (int k = 0; k < pv->size && k < 16; k++) {
                back.v[k] = ex->storage[pv->offset + k];
            }
            args[i] = back;          /* stashed for the caller-side write */
        }
    }

    ex->returning = saved_returning;
    ex->call_depth--;

    /* Restore the caller's frame. */
    ex->var_count    = saved_count;
    ex->storage_used = saved_storage;
    ex->scope        = saved_scope;
    if (moved > 0) {
        memcpy(&ex->vars[hidden_from], ex->stash[slot],
               (size_t)moved * sizeof(rt_var_t));
    }

    /* Now that the caller's variables are visible again, write back. */
    {
        int i = 0;
        for (glsl_node_t *a = call->list;
             a && i < fn->param_count && i < argc; a = a->next, i++) {
            if (!fn->params) break;
            glsl_qualifier_t q = fn->params[i].qual;
            if (q != GLSL_Q_PARAM_OUT && q != GLSL_Q_PARAM_INOUT) continue;
            lvalue_t lv;
            if (resolve_lvalue(ex, a, &lv)) lvalue_write(ex, &lv, &args[i]);
        }
    }

    return result;
}

/* ============================================================================
 * Statements
 * ==========================================================================*/

/* Has control flow left the current statement sequence? */
static int unwinding(const exec_t *ex) {
    return ex->returning || ex->breaking || ex->continuing ||
           ex->discarded || ex->failed;
}

/* Charge one iteration against the budget.  Returns 0 when it is spent. */
static int tick(exec_t *ex, int line) {
    if (++ex->iterations > GLSL_MAX_ITERATIONS) {
        rt_error(ex, line, "shader exceeded its iteration budget "
                           "(possible infinite loop)");
        return 0;
    }
    return 1;
}

static void exec_stmt(exec_t *ex, glsl_node_t *n) {
    if (!n || unwinding(ex)) return;

    switch (n->kind) {
    case GLSL_NODE_BLOCK:
        scope_push(ex);
        for (glsl_node_t *st = n->list; st; st = st->next) {
            exec_stmt(ex, st);
            if (unwinding(ex)) break;
        }
        scope_pop(ex);
        break;

    case GLSL_NODE_DECL: {
        rt_var_t *v = declare_var(ex, n->v.name, n->decl_type, n->line);
        if (!v) break;
        if (n->a) {
            glsl_value_t init = eval(ex, n->a);
            for (int i = 0; i < v->size && i < 16; i++) {
                ex->storage[v->offset + i] = init.v[i];
            }
        }
        break;
    }

    case GLSL_NODE_EXPR_STMT:
        eval(ex, n->a);
        break;

    case GLSL_NODE_IF: {
        glsl_value_t c = eval(ex, n->a);
        if (c.v[0] != 0.0f) exec_stmt(ex, n->b);
        else                exec_stmt(ex, n->c);
        break;
    }

    case GLSL_NODE_WHILE:
        for (;;) {
            if (!tick(ex, n->line)) break;
            glsl_value_t c = eval(ex, n->a);
            if (c.v[0] == 0.0f || ex->failed) break;
            exec_stmt(ex, n->b);
            if (ex->breaking) { ex->breaking = 0; break; }
            ex->continuing = 0;
            if (ex->returning || ex->discarded || ex->failed) break;
        }
        break;

    case GLSL_NODE_DO:
        for (;;) {
            if (!tick(ex, n->line)) break;
            exec_stmt(ex, n->b);
            if (ex->breaking) { ex->breaking = 0; break; }
            ex->continuing = 0;
            if (ex->returning || ex->discarded || ex->failed) break;
            glsl_value_t c = eval(ex, n->a);
            if (c.v[0] == 0.0f) break;
        }
        break;

    case GLSL_NODE_FOR:
        /* The initialiser's scope wraps the whole loop, so a variable it
         * declares is visible to the condition and step but not after. */
        scope_push(ex);
        for (glsl_node_t *d = n->a; d; d = d->next) {
            exec_stmt(ex, d);
            if (unwinding(ex)) break;
        }
        while (!unwinding(ex)) {
            if (!tick(ex, n->line)) break;
            if (n->b) {
                glsl_value_t c = eval(ex, n->b);
                if (c.v[0] == 0.0f || ex->failed) break;
            }
            exec_stmt(ex, n->d);
            if (ex->breaking) { ex->breaking = 0; break; }
            ex->continuing = 0;
            if (ex->returning || ex->discarded || ex->failed) break;
            if (n->c) eval(ex, n->c);
        }
        scope_pop(ex);
        break;

    case GLSL_NODE_RETURN:
        if (n->a) ex->return_value = eval(ex, n->a);
        ex->returning = 1;
        break;

    case GLSL_NODE_BREAK:    ex->breaking = 1;   break;
    case GLSL_NODE_CONTINUE: ex->continuing = 1; break;
    case GLSL_NODE_DISCARD:  ex->discarded = 1;  break;

    case GLSL_NODE_EMPTY: case GLSL_NODE_ERROR:
        break;

    default:
        eval(ex, n);
        break;
    }
}

/* ============================================================================
 * Entry point
 * ==========================================================================*/

glsl_run_status_t glsl_run(glsl_unit_t *u, glsl_env_t *env) {
    if (!u || !u->compiled || !u->root) return GLSL_RUN_ERROR;

    /* The interpreter state is ~90 KB, which is a page fault waiting to happen
     * as a local -- the same lesson G12 learned from aglxResize().  It is
     * allocated ONCE per unit and reused: a fragment shader runs once per
     * pixel, and both allocating it and zeroing it per invocation were
     * measurable against the interpretation itself. */
    exec_t *ex = (exec_t *)u->exec_state;
    if (!ex) {
        ex = (exec_t *)glsl_alloc(u, sizeof(exec_t));
        if (!ex) {
            glsl_error(u, 1, "not enough memory to run the shader");
            return GLSL_RUN_ERROR;
        }
        u->exec_state = ex;
    }
    /* Clear only what a run actually depends on being zero.
     *
     * memset() over the whole exec_t was 90 KB per invocation -- the stash,
     * argument and constructor scratch arrays dominate it -- and a fragment
     * shader runs once per PIXEL.  That one line cost 3.6 us of the 3.9 us a
     * trivial shader took, a 14x slowdown over the same interpreter measured
     * standalone in G11b, and it only became visible once the pipeline
     * started calling glsl_run() 76 800 times a frame.
     *
     * The scratch arrays need no clearing: every one is written before it is
     * read, indexed by a depth counter that starts at zero.  Only the
     * bookkeeping below must be reset. */
    ex->u = u;
    ex->env = env;
    ex->scope = 1;
    ex->var_count = 0;
    ex->storage_used = 0;
    ex->iterations = 0;
    ex->call_depth = 0;
    ex->arg_depth = 0;
    ex->flat_depth = 0;
    ex->failed = 0;
    ex->returning = ex->breaking = ex->continuing = ex->discarded = 0;
    memset(&ex->return_value, 0, sizeof ex->return_value);

    /* Globals: `const` and plain globals get storage and their initialisers
     * run; uniforms, attributes and varyings stay with the environment, since
     * their values come from outside the shader. */
    for (glsl_node_t *g = u->root->list; g; g = g->next) {
        if (g->kind != GLSL_NODE_DECL) continue;
        if (g->qual == GLSL_Q_UNIFORM || g->qual == GLSL_Q_ATTRIBUTE ||
            g->qual == GLSL_Q_VARYING) {
            continue;
        }
        rt_var_t *v = declare_var(ex, g->v.name, g->decl_type, g->line);
        if (!v) break;
        if (g->a) {
            glsl_value_t init = eval(ex, g->a);
            for (int i = 0; i < v->size && i < 16; i++) {
                ex->storage[v->offset + i] = init.v[i];
            }
        }
    }

    glsl_node_t *main_fn = find_function(ex, "main");
    if (!main_fn) {
        glsl_error(u, 1, "shader has no 'main' to run");
        return GLSL_RUN_ERROR;
    }

    ex->scope = 2;
    exec_stmt(ex, main_fn->body);

    glsl_run_status_t st = GLSL_RUN_OK;
    if (ex->failed) {
        st = GLSL_RUN_ERROR;
        /* A runtime diagnostic has to reach the info log, or a shader that
         * loops forever reports a failure with nothing to explain it. */
        glsl_build_log(u);
    } else if (ex->discarded) {
        st = GLSL_RUN_DISCARD;
    }

    return st;
}

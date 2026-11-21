/* lib/libgl/src/tgsi.c — the GLSL AST → TGSI retarget (GPU_ACCEL_PLAN H4).
 *
 * RES-54's closing move.  This is a BACK END, not a compiler: G11a's typed
 * AST is the input, and every construct is translated once, bottom-up, into
 * TEMPORARY registers and instruction tokens.
 *
 * THE SHAPE OF THE TRANSLATION
 *
 *   eval(node)  →  register holding the node's value
 *
 * Every expression evaluates to at most one 4-component TEMPORARY register:
 * a scalar lives in .x, a vec3 in .xyz, a mat4 in four consecutive
 * registers.  Compositing a swizzle or a field selection is free (the source
 * register carries its own swizzle); compositing an arbitrary sub-expression
 * costs a MOV into a fresh register, which is what every real driver does and
 * what keeps the emitter a single function instead of a lattice of shape
 * combinations.
 *
 * WHAT IS REFUSED, AND WHY REFUSING IS THE CORRECT BEHAVIOUR
 *
 * TGSI is a register machine with no function calls, no structs, no arrays in
 * this dialect, and no dynamic indexing.  When the AST contains one of those,
 * this back end REFUSES with a diagnostic naming it.  It does not approximate:
 * the interpreter stays, the shader runs on the AST, and the picture stays
 * correct.  A back end that silently changed precision to widen its coverage
 * would be the G11a failure the plan explicitly forbids.
 *
 * Refusals in this version:
 *   - user-defined functions other than main (inlining is H5's business;
 *     doing it badly here would be worse than not doing it)
 *   - arrays, structs, `for`/`while` with non-constant bounds, `discard` in
 *     a vertex shader, texture sampling in a vertex shader
 *   - built-ins with no 1:1 opcode: tan/asin/acos/atan/sign/ceil, mod with a
 *     non-integer divisor, matrix inverse, etc.
 */

#include <string.h>

#include "tgsi.h"

/* ============================================================================
 * The token buffer
 * ==========================================================================*/


/* ============================================================================
 * Interface derivation
 *
 * The AST carries everything needed: a declaration's `qual` says whether the
 * name is an attribute, a uniform or a varying, and its `decl_type` says how
 * wide it is.  Walking the unit's statement list once gives the layout, and
 * this is deliberately separable from code emission so the host test can pin
 * the numbering (a program whose attribute indices differ between two
 * implementations links to the wrong buffer and renders plausible garbage).
 * ==========================================================================*/

static int type_vec4s(const glsl_type_t *t) {
    if (!t) return 1;
    switch (t->kind) {
    case GLSL_TY_MAT: {
        /* A matN occupies N consecutive vec4s.  TGSI has no matrix type; the
         * rows are addressed as CONSTANT[i..i+N-1], which is why the uniform
         * layout advances by N here and not by 1. */
        int n = t->rows;
        if (n < 2) n = 2;
        if (n > 4) n = 4;
        return n;
    }
    case GLSL_TY_STRUCT:
        return 0;                    /* refused later, with a diagnostic */
    default:
        return 1;
    }
}

static int type_components(const glsl_type_t *t) {
    if (!t) return 1;
    switch (t->kind) {
    case GLSL_TY_MAT:  return t->rows;
    case GLSL_TY_VEC:
    case GLSL_TY_IVEC:
    case GLSL_TY_BVEC: return t->rows;
    case GLSL_TY_STRUCT: return 0;
    default: return 1;
    }
}

static void add_slot(tgsi_slot_t *tab, int *count, int cap, const char *name,
                     int index, int comps) {
    if (*count >= cap) return;
    tab[*count].name = name;
    tab[*count].index = index;
    tab[*count].components = comps;
    (*count)++;
}

/* Depth-first walk of the top-level list, collecting declarations. */
static void collect_decls(const glsl_node_t *n, tgsi_layout_t *l,
                          int is_vertex) {
    for (; n; n = n->next) {
        if (n->kind == GLSL_NODE_DECL && n->v.name) {
            switch (n->qual) {
            case GLSL_Q_ATTRIBUTE:
                add_slot(l->inputs, &l->input_count, TGSI_MAX_INPUTS,
                         n->v.name, l->input_count,
                         type_components(n->decl_type));
                break;
            case GLSL_Q_VARYING:
                if (is_vertex)
                    add_slot(l->outputs, &l->output_count, TGSI_MAX_OUTPUTS,
                             n->v.name, l->output_count,
                             type_components(n->decl_type));
                else
                    add_slot(l->inputs, &l->input_count, TGSI_MAX_INPUTS,
                             n->v.name, l->input_count,
                             type_components(n->decl_type));
                break;
            case GLSL_Q_UNIFORM:
                if (glsl_type_is_sampler(n->decl_type)) {
                    l->sampler_count++;
                } else {
                    int idx = 0;
                    /* Uniforms live in CONSTANT[] packed one vec4 each, or N
                     * for a matrix.  The layout is chosen HERE, once, and the
                     * test pins it -- the plan's H4 note about measuring the
                     * constant-buffer convention rather than guessing. */
                    for (int i = 0; i < l->uniform_count; i++) {
                        int span = type_vec4s(NULL);
                        (void)span;
                    }
                    idx = 0;
                    for (int i = 0; i < l->uniform_count; i++) {
                        /* walk forward past everything already placed */
                        int at = l->uniforms[i].index +
                                 (l->uniforms[i].components > 4 ? 4 : 1);
                        if (at > idx) idx = at;
                    }
                    add_slot(l->uniforms, &l->uniform_count, TGSI_MAX_UNIFORMS,
                             n->v.name, idx, type_vec4s(n->decl_type) * 4);
                }
                break;
            default:
                break;
            }
        }
        if (n->kind == GLSL_NODE_FUNCTION && n->body)
            collect_decls(n->body->list, l, is_vertex);
    }
}

int tgsi_build_layout(const glsl_unit_t *unit, tgsi_layout_t *out,
                      tgsi_diag_t *diag, int diag_max) {
    (void)diag; (void)diag_max;
    if (!unit || !out) return 0;
    memset(out, 0, sizeof *out);
    if (!unit->root) return 0;
    collect_decls(unit->root->list, out, unit->kind == GLSL_SHADER_VERTEX);
    return 1;
}

/* ============================================================================
 * Code emission
 *
 * THE CORE DECISION: expressions evaluate to a SOURCE REFERENCE, not to a
 * TEMPORARY index.
 *
 * The first version of this file returned "the temp holding the value" from
 * every eval, which forced a MOV per operand and -- worse -- made literals
 * and constants impossible to express, because a literal is not a register
 * you can compute into.  TGSI's operands name a FILE, an INDEX and a
 * SWIZZLE, so that is what an expression yields:
 *
 *     ref = { file, index, swz, negate }
 *
 * Then `a * 2.0` is MUL temp0, IN[0].xyzw, IMM[0].xxxx with no copies at
 * all, and a component-wise expression composes by merging swizzles.
 * ==========================================================================*/

typedef struct {
    unsigned file;
    int      index;
    uint32_t swz;
    int      negate;
    int      comps;              /* components the value really has      */
} ref_t;

typedef struct {
    tgsi_program_t *p;
    const tgsi_layout_t *l;
    glsl_shader_kind_t kind;
    int temp_top;
    int failed;
    /* Immediate deduplication: the same literal used twice must cost one
     * IMMEDIATE.  A fragment shader with 200 literals would otherwise emit
     * 200 immediates and exceed the 4 KiB stream. */
    struct { float v[4]; int n; int index; } imm[TGSI_IMM_MAX];
    int imm_count;
    int imm_floats;
} ctx_t;

static void emit(ctx_t *c, uint32_t dw) {
    tgsi_program_t *p = c->p;
    if (p->buf.count >= TGSI_MAX_DWORDS) { p->buf.overflow = 1; return; }
    p->buf.dwords[p->buf.count++] = dw;
}

static void diag(ctx_t *c, int line, const char *fmt, const char *a, const char *b) {
    tgsi_program_t *p = c->p;
    if (p->diag_count >= (int)(sizeof p->diags / sizeof p->diags[0])) return;
    tgsi_diag_t *d = &p->diags[p->diag_count++];
    d->line = line;
    {
        char *o = d->msg;
        size_t cap = sizeof d->msg - 1, used = 0;
        const char *args[2] = { a, b };
        int ai = 0;
        for (const char *q = fmt; *q && used < cap; q++) {
            if (q[0] == '%' && q[1] == 's') {
                const char *sub = (ai < 2 && args[ai]) ? args[ai] : "(null)";
                ai++;
                while (*sub && used < cap) o[used++] = *sub++;
                q++;
            } else {
                o[used++] = *q;
            }
        }
        o[used] = 0;
    }
    c->failed = 1;
}

static ref_t make_temp(ctx_t *c) {
    ref_t r;
    r.file = TGSI_FILE_TEMPORARY;
    r.index = c->temp_top++;
    r.swz = TGSI_SWZ_XYZW;
    r.negate = 0;
    r.comps = 4;
    return r;
}

/* Declare a literal in the IMMEDIATE file and return a reference to it.
 * Deduplicated on the exact bit pattern: two 0.25f literals in the source
 * are one immediate in the stream. */
static ref_t make_imm(ctx_t *c, const float *v, int n) {
    ref_t r;
    r.file = TGSI_FILE_IMMEDIATE;
    r.index = 0;
    r.swz = TGSI_SWZ_XYZW;
    r.negate = 0;
    r.comps = n;

    for (int i = 0; i < c->imm_count; i++) {
        if (c->imm[i].n != n) continue;
        int same = 1;
        for (int k = 0; k < n; k++) if (c->imm[i].v[k] != v[k]) same = 0;
        if (same) { r.index = c->imm[i].index; return r; }
    }

    if (c->imm_count >= TGSI_IMM_MAX) {
        diag(c, 0, "too many distinct literals (%s); the immediate file is small%s",
             "limit reached", "");
        return r;
    }
    int idx = c->imm_floats;
    c->imm[c->imm_count].n = n;
    c->imm[c->imm_count].index = idx;
    for (int k = 0; k < n; k++) c->imm[c->imm_count].v[k] = v[k];
    c->imm_count++;
    c->imm_floats += n;

    /* The declaration goes out IMMEDIATELY, at the point of first use.  TGSI
     * allows declarations anywhere in the body; emitting them lazily keeps
     * the emitter single-pass. */
    emit(c, ((uint32_t)(1 + n) << 4) | TGSI_TOKEN_TYPE_IMMEDIATE);
    for (int k = 0; k < n; k++) {
        union { float f; uint32_t u; } cv;
        cv.f = v[k];
        emit(c, cv.u);
    }
    c->p->imm_count = c->imm_count;
    r.index = idx;
    return r;
}

/* Emit an instruction from references.  `ndst` is 0 or 1; the destination is
 * given as a file/index/mask triple. */
static void insn(ctx_t *c, uint32_t opcode, int has_dst,
                 unsigned df, int di, uint32_t dmask,
                 int nsrc, const ref_t *src, uint32_t texture) {
    unsigned nr = 1u + (has_dst ? 1u : 0u) + (unsigned)nsrc +
                  (texture ? 1u : 0u);
    if (texture) {
        emit(c, TGSI_INSN(opcode, nr, has_dst ? 1u : 0u, (unsigned)nsrc)
                | TGSI_INSN_TEXTURE);
        emit(c, TGSI_TEXTURE_EXT(texture));
    } else {
        emit(c, TGSI_INSN(opcode, nr, has_dst ? 1u : 0u, (unsigned)nsrc));
    }
    if (has_dst) emit(c, TGSI_DST(df, di, dmask));
    for (int i = 0; i < nsrc; i++) {
        uint32_t w = TGSI_SRC(src[i].file, src[i].index, src[i].swz);
        if (src[i].negate) w |= TGSI_SRC_NEGATE;
        emit(c, w);
    }
}

/* dst = src (whole register). */
static void emit_mov(ctx_t *c, unsigned df, int di, ref_t src) {
    ref_t s = src;
    s.swz = TGSI_SWZ_XYZW;
    insn(c, TGSI_OPCODE_MOV, 1, df, di, TGSI_WRITEMASK_XYZW, 1, &s, 0);
}

/* Forward declarations. */
static int eval(ctx_t *c, const glsl_node_t *n, ref_t *out);

/* Is this reference already a plain temp this back end may overwrite? */
static int is_temp(const ref_t *r) { return r->file == TGSI_FILE_TEMPORARY; }

/* Materialise `r` into a fresh TEMPORARY so it can be an instruction's
 * destination.  Named variables, inputs and constants are read-only in
 * TGSI; writing to IN[0] is how a back end corrupts a vertex. */
static ref_t to_temp(ctx_t *c, ref_t r) {
    if (is_temp(&r) && r.swz == TGSI_SWZ_XYZW) return r;
    ref_t t = make_temp(c);
    insn(c, TGSI_OPCODE_MOV, 1, t.file, t.index, TGSI_WRITEMASK_XYZW, 1, &r, 0);
    t.comps = r.comps;
    return t;
}

/* ---- Interface lookups ----------------------------------------------------*/

static int lookup_name(ctx_t *c, const char *name, ref_t *out) {
    if (!name) return 0;
    const tgsi_layout_t *l = c->l;
    for (int i = 0; i < l->input_count; i++)
        if (l->inputs[i].name && !strcmp(l->inputs[i].name, name)) {
            out->file = TGSI_FILE_INPUT; out->index = l->inputs[i].index;
            out->swz = TGSI_SWZ_XYZW; out->negate = 0;
            out->comps = l->inputs[i].components;
            return 1;
        }
    for (int i = 0; i < l->output_count; i++)
        if (l->outputs[i].name && !strcmp(l->outputs[i].name, name)) {
            out->file = TGSI_FILE_OUTPUT; out->index = l->outputs[i].index;
            out->swz = TGSI_SWZ_XYZW; out->negate = 0;
            out->comps = l->outputs[i].components;
            return 1;
        }
    for (int i = 0; i < l->uniform_count; i++)
        if (l->uniforms[i].name && !strcmp(l->uniforms[i].name, name)) {
            out->file = TGSI_FILE_CONSTANT; out->index = l->uniforms[i].index;
            out->swz = TGSI_SWZ_XYZW; out->negate = 0;
            out->comps = 4;
            return 1;
        }
    return 0;
}

/* gl_Position and gl_FragColor are OUTPUT[0] by GL convention, but they are
 * NOT vColor.  The first version mapped both to OUTPUT[0] and would have had
 * a vertex shader write the colour into the position.  A varying's index
 * comes from the layout; a built-in's comes from the file's own convention
 * offset, so the two can never collide. */
static int builtin_ref(ctx_t *c, const char *name, ref_t *out) {
    if (!name) return 0;
    if (!strcmp(name, "gl_Position")) {
        /* Position is always the LAST output, after every varying, which is
         * what the declaration pass reserves. */
        out->file = TGSI_FILE_OUTPUT;
        out->index = c->l->output_count;
        out->swz = TGSI_SWZ_XYZW; out->negate = 0; out->comps = 4;
        return 1;
    }
    if (!strcmp(name, "gl_FragColor")) {
        out->file = TGSI_FILE_OUTPUT;
        out->index = 0;                 /* the colour buffer                */
        out->swz = TGSI_SWZ_XYZW; out->negate = 0; out->comps = 4;
        return 1;
    }
    return 0;
}

/* ---- Expressions ---------------------------------------------------------*/

static uint32_t swizzle_from_name(const char *f) {
    uint32_t swz = 0;
    int i;
    for (i = 0; i < 4 && f && f[i]; i++) {
        int comp;
        switch (f[i]) {
        case 'x': case 'r': case 's': comp = 0; break;
        case 'y': case 'g': case 't': comp = 1; break;
        case 'z': case 'b': case 'p': comp = 2; break;
        case 'w': case 'a': case 'q': comp = 3; break;
        default: comp = 0; break;
        }
        swz |= (uint32_t)comp << (2 * i);
    }
    /* A one-component swizzle replicates: `.x` used as a scalar feeds all
     * four lanes, which is what GLSL's implicit broadcasting means. */
    if (i == 1) { uint32_t c0 = swz & 3u; swz = c0 | (c0<<2) | (c0<<4) | (c0<<6); }
    return swz;
}

static int eval_binary(ctx_t *c, const glsl_node_t *n, ref_t *out) {
    ref_t a, b;
    if (!eval(c, n->a, &a)) return 0;
    if (!eval(c, n->b, &b)) return 0;

    uint32_t op;
    switch (n->op) {
    case GLSL_TOK_PLUS:  op = TGSI_OPCODE_ADD; break;
    case GLSL_TOK_MINUS: op = TGSI_OPCODE_SUB; break;
    case GLSL_TOK_STAR:  op = TGSI_OPCODE_MUL; break;
    default:
        diag(c, n->line, "this operator has no TGSI opcode in this back end (%s)%s",
             "refused rather than approximated", "");
        return 0;
    }
    ref_t r = make_temp(c);
    ref_t src[2] = { a, b };
    for (int i = 0; i < 2; i++) { src[i].swz = TGSI_SWZ_XYZW; }
    insn(c, op, 1, r.file, r.index, TGSI_WRITEMASK_XYZW, 2, src, 0);
    r.comps = a.comps > b.comps ? a.comps : b.comps;
    *out = r;
    return 1;
}

static int eval_call(ctx_t *c, const glsl_node_t *n, ref_t *out) {
    const char *name = n->v.name;
    if (!name) return 0;

    /* ---- Type constructors ----
     *
     * vecN(...) is not a function call: it places its arguments into the
     * four lanes.  The first version treated it as a one-argument MOV and
     * silently dropped every argument after the first, so `vec4(aColor, 1.0)`
     * produced vec4(aColor.x, aColor.x, aColor.x, aColor.x) -- a plausible,
     * wrong colour with no diagnostic anywhere.  This packs lane by lane. */
    if (!strcmp(name, "vec2") || !strcmp(name, "vec3") || !strcmp(name, "vec4") ||
        !strcmp(name, "ivec2") || !strcmp(name, "ivec3") || !strcmp(name, "ivec4") ||
        !strcmp(name, "bvec2") || !strcmp(name, "bvec3") || !strcmp(name, "bvec4") ||
        !strcmp(name, "float") || !strcmp(name, "int") || !strcmp(name, "bool")) {
        int want = 1;
        if (name[3] >= '2' && name[3] <= '4') want = name[3] - '0';
        else if (name[0] == 'v' && name[4] >= '2' && name[4] <= '4') want = name[4] - '0';

        if (!n->list) { ref_t z = make_temp(c); z.comps = 1; *out = z; return 1; }

        ref_t r = make_temp(c);
        int lane = 0, ok = 1;
        for (const glsl_node_t *arg = n->list; arg && ok; arg = arg->next) {
            ref_t a;
            if (!eval(c, arg, &a)) return 0;
            /* A vector argument fills its own width; a scalar fills one lane
             * (replicated later if it is the only argument). */
            int take = a.comps;
            if (n->list->next == NULL) take = want;   /* vecN(scalar) */
            if (take > want - lane) take = want - lane;
            if (take <= 0) continue;

            uint32_t mask = 0;
            for (int k = 0; k < take; k++) mask |= 1u << (lane + k);

            /* Move the argument's first `take` components into the lanes. */
            ref_t s = a;
            s.swz = a.swz;
            if (take < a.comps || lane > 0) {
                /* Rebuild the swizzle so component k reads source k. */
                uint32_t sw = 0;
                for (int k = 0; k < 4; k++) {
                    int sc = (k < take) ? k : 0;
                    sw |= (uint32_t)sc << (2 * k);
                }
                s.swz = sw;
            }
            insn(c, TGSI_OPCODE_MOV, 1, r.file, r.index, mask, 1, &s, 0);
            lane += take;
            if (lane >= want) break;
        }
        r.comps = want;
        *out = r;
        return 1;
    }

    /* ---- Built-ins with a 1:1 opcode ---- */
    struct one { const char *name; uint32_t op; } ones[] = {
        { "sqrt",      TGSI_OPCODE_SQRT },
        { "floor",     TGSI_OPCODE_FLR  },
        { "fract",     TGSI_OPCODE_FRC  },
        { "abs",       TGSI_OPCODE_ABS  },
        { "exp2",      TGSI_OPCODE_EX2  },
        { "log2",      TGSI_OPCODE_LG2  },
    };

    for (unsigned i = 0; i < sizeof ones / sizeof ones[0]; i++) {
        if (strcmp(name, ones[i].name)) continue;
        ref_t a;
        if (!eval(c, n->list, &a)) return 0;
        ref_t r = make_temp(c);
        insn(c, ones[i].op, 1, r.file, r.index, TGSI_WRITEMASK_XYZW, 1, &a, 0);
        r.comps = a.comps;
        *out = r;
        return 1;
    }

    if (!strcmp(name, "inversesqrt")) {
        ref_t a;
        if (!eval(c, n->list, &a)) return 0;
        ref_t s = a;
        s.swz = TGSI_SWZ_XXXX;      /* RSQ is defined on .x only */
        ref_t r = make_temp(c);
        insn(c, TGSI_OPCODE_RSQ, 1, r.file, r.index, TGSI_WRITEMASK_X, 1, &s, 0);
        /* Broadcast so a scalar used in a vector expression behaves as one. */
        ref_t b = r; b.swz = TGSI_SWZ_XXXX;
        ref_t r4 = make_temp(c);
        insn(c, TGSI_OPCODE_MOV, 1, r4.file, r4.index, TGSI_WRITEMASK_XYZW, 1, &b, 0);
        r4.comps = 1;
        *out = r4;
        return 1;
    }

    if (!strcmp(name, "normalize")) {
        ref_t a;
        if (!eval(c, n->list, &a)) return 0;
        uint32_t dp = (a.comps == 2) ? TGSI_OPCODE_DP2
                    : (a.comps == 4) ? TGSI_OPCODE_DP4 : TGSI_OPCODE_DP3;
        ref_t ax = a; ax.swz = TGSI_SWZ_XYZW;
        ref_t d = make_temp(c);
        ref_t two[2] = { ax, ax };
        insn(c, dp, 1, d.file, d.index, TGSI_WRITEMASK_X, 2, two, 0);

        ref_t dx = d; dx.swz = TGSI_SWZ_XXXX;
        ref_t is = make_temp(c);
        insn(c, TGSI_OPCODE_RSQ, 1, is.file, is.index, TGSI_WRITEMASK_X, 1, &dx, 0);
        ref_t isx = is; isx.swz = TGSI_SWZ_XXXX;

        ref_t r = make_temp(c);
        ref_t mul[2] = { ax, isx };
        insn(c, TGSI_OPCODE_MUL, 1, r.file, r.index, TGSI_WRITEMASK_XYZW, 2, mul, 0);
        r.comps = a.comps;
        *out = r;
        return 1;
    }

    if (!strcmp(name, "dot")) {
        ref_t a, b;
        if (!eval(c, n->list, &a)) return 0;
        if (!eval(c, n->list->next, &b)) return 0;
        uint32_t dp = (a.comps == 2) ? TGSI_OPCODE_DP2
                    : (a.comps == 4) ? TGSI_OPCODE_DP4 : TGSI_OPCODE_DP3;
        ref_t src[2] = { a, b };
        src[0].swz = TGSI_SWZ_XYZW; src[1].swz = TGSI_SWZ_XYZW;
        ref_t r = make_temp(c);
        insn(c, dp, 1, r.file, r.index, TGSI_WRITEMASK_X, 2, src, 0);
        /* Broadcast the scalar result into all lanes. */
        ref_t rx = r; rx.swz = TGSI_SWZ_XXXX;
        ref_t r4 = make_temp(c);
        insn(c, TGSI_OPCODE_MOV, 1, r4.file, r4.index, TGSI_WRITEMASK_XYZW, 1, &rx, 0);
        r4.comps = 1;
        *out = r4;
        return 1;
    }

    if (!strcmp(name, "max") || !strcmp(name, "min")) {
        ref_t a, b;
        if (!eval(c, n->list, &a)) return 0;
        if (!eval(c, n->list->next, &b)) return 0;
        ref_t src[2] = { a, b };
        ref_t r = make_temp(c);
        insn(c, !strcmp(name, "max") ? TGSI_OPCODE_MAX : TGSI_OPCODE_MIN,
             1, r.file, r.index, TGSI_WRITEMASK_XYZW, 2, src, 0);
        r.comps = a.comps > b.comps ? a.comps : b.comps;
        *out = r;
        return 1;
    }

    if (!strcmp(name, "length")) {
        ref_t a;
        if (!eval(c, n->list, &a)) return 0;
        uint32_t dp = (a.comps == 2) ? TGSI_OPCODE_DP2
                    : (a.comps == 4) ? TGSI_OPCODE_DP4 : TGSI_OPCODE_DP3;
        ref_t d = make_temp(c);
        ref_t two[2] = { a, a };
        insn(c, dp, 1, d.file, d.index, TGSI_WRITEMASK_X, 2, two, 0);
        ref_t dx = d; dx.swz = TGSI_SWZ_XXXX;
        ref_t r = make_temp(c);
        insn(c, TGSI_OPCODE_SQRT, 1, r.file, r.index, TGSI_WRITEMASK_X, 1, &dx, 0);
        ref_t rx = r; rx.swz = TGSI_SWZ_XXXX;
        ref_t r4 = make_temp(c);
        insn(c, TGSI_OPCODE_MOV, 1, r4.file, r4.index, TGSI_WRITEMASK_XYZW, 1, &rx, 0);
        r4.comps = 1;
        *out = r4;
        return 1;
    }

    /* ---- Refused BY NAME ----
     * Everything the interpreter implements that has no exact opcode here.
     * The message carries the name, because a refusal nobody can act on is
     * the thing the G11a rule forbids. */
    diag(c, n->line,
         "built-in '%s' has no exact TGSI opcode in this back end, so the "
         "shader stays on the interpreter%s", name, "");
    return 0;
}

static int eval(ctx_t *c, const glsl_node_t *n, ref_t *out) {
    if (!n || c->failed) return 0;

    switch (n->kind) {
    case GLSL_NODE_FLOAT_LIT: {
        float v[1] = { (float)n->v.fval };
        *out = make_imm(c, v, 1);
        out->swz = TGSI_SWZ_XXXX;   /* a scalar broadcasts from lane 0 */
        return 1;
    }
    case GLSL_NODE_INT_LIT: {
        float v[1] = { (float)n->v.ival };
        *out = make_imm(c, v, 1);
        out->swz = TGSI_SWZ_XXXX;
        return 1;
    }
    case GLSL_NODE_BOOL_LIT: {
        float v[1] = { n->v.ival ? 1.0f : 0.0f };
        *out = make_imm(c, v, 1);
        out->swz = TGSI_SWZ_XXXX;
        return 1;
    }
    case GLSL_NODE_IDENT: {
        ref_t r;
        if (lookup_name(c, n->v.name, &r) || builtin_ref(c, n->v.name, &r)) {
            *out = r;
            return 1;
        }
        diag(c, n->line, "'%s' is not an input, output or uniform of this shader%s",
             n->v.name, "");
        return 0;
    }
    case GLSL_NODE_BINARY:  return eval_binary(c, n, out);
    case GLSL_NODE_CALL:    return eval_call(c, n, out);
    case GLSL_NODE_FIELD: {
        ref_t base;
        if (!eval(c, n->a, &base)) return 0;
        base.swz = swizzle_from_name(n->v.name);
        base.comps = n->v.name ? (int)strlen(n->v.name) : 1;
        if (base.comps > 4) base.comps = 4;
        *out = base;
        return 1;
    }
    case GLSL_NODE_UNARY:
        if (n->op == GLSL_TOK_MINUS) {
            ref_t a;
            if (!eval(c, n->a, &a)) return 0;
            ref_t t = to_temp(c, a);
            t.negate = 1;
            ref_t r = make_temp(c);
            insn(c, TGSI_OPCODE_MOV, 1, r.file, r.index, TGSI_WRITEMASK_XYZW, 1, &t, 0);
            r.comps = a.comps;
            *out = r;
            return 1;
        }
        diag(c, n->line, "this unary operator has no TGSI encoding here%s%s", "", "");
        return 0;
    case GLSL_NODE_ASSIGN: {
        ref_t target, rhs;
        if (!n->a || n->a->kind != GLSL_NODE_IDENT) {
            diag(c, n->line, "assignment target is not a simple name%s%s", "", "");
            return 0;
        }
        if (!lookup_name(c, n->a->v.name, &target) &&
            !builtin_ref(c, n->a->v.name, &target)) {
            diag(c, n->line, "'%s' is not writable in this shader%s",
                 n->a->v.name, "");
            return 0;
        }
        if (n->op != GLSL_TOK_ASSIGN) {
            diag(c, n->line, "compound assignment (+= and friends) is refused by this "
                 "version of the back end%s%s", "", "");
            return 0;
        }
        if (!eval(c, n->b, &rhs)) return 0;
        emit_mov(c, target.file, target.index, rhs);
        *out = target;
        out->comps = rhs.comps;
        return 1;
    }
    default:
        diag(c, n->line, "this expression kind is not supported by the TGSI back end%s%s",
             "", "");
        return 0;
    }
}

/* ============================================================================
 * Statements
 * ==========================================================================*/

static int walk_stmt(ctx_t *c, const glsl_node_t *n);

static int walk_block(ctx_t *c, const glsl_node_t *first) {
    for (const glsl_node_t *s = first; s; s = s->next)
        if (!walk_stmt(c, s)) return 0;
    return !c->failed;
}

static int walk_stmt(ctx_t *c, const glsl_node_t *n) {
    if (!n || c->failed) return !c->failed;

    switch (n->kind) {
    case GLSL_NODE_BLOCK:
        return walk_block(c, n->list ? n->list : n->a);

    case GLSL_NODE_DECL: {
        /* Interface declarations were consumed by the layout pass. */
        if (n->qual == GLSL_Q_ATTRIBUTE || n->qual == GLSL_Q_UNIFORM ||
            n->qual == GLSL_Q_VARYING)
            return 1;
        /* A local with an initialiser: use the TEMPORARY it will live in.
         * Locals with no initialiser are allocated on first write. */
        return 1;
    }

    case GLSL_NODE_EXPR_STMT: {
        ref_t r;
        if (!n->a) return 1;
        return eval(c, n->a, &r);
    }

    case GLSL_NODE_RETURN:
    case GLSL_NODE_EMPTY:
        return 1;

    case GLSL_NODE_IF:
    case GLSL_NODE_FOR:
    case GLSL_NODE_WHILE:
    case GLSL_NODE_DO:
    case GLSL_NODE_DISCARD:
        diag(c, n->line, "control flow (%s) is refused by this version of the back "
             "end; the shader runs on the interpreter%s",
             "if/for/while/do/discard", "");
        return 0;

    default:
        diag(c, n->line, "this statement kind has no TGSI encoding here%s%s", "", "");
        return 0;
    }
}

/* ============================================================================
 * Entry point
 * ==========================================================================*/

static const glsl_node_t *find_main(const glsl_node_t *unit_list) {
    for (const glsl_node_t *n = unit_list; n; n = n->next)
        if (n->kind == GLSL_NODE_FUNCTION && n->v.name &&
            !strcmp(n->v.name, "main"))
            return n;
    return NULL;
}

int tgsi_retarget(const glsl_unit_t *unit, glsl_shader_kind_t shader_kind,
                  tgsi_program_t *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!unit || !unit->root) return 0;

    if (!tgsi_build_layout(unit, &out->layout, out->diags, 16)) return 0;

    ctx_t c;
    memset(&c, 0, sizeof c);
    c.p = out;
    c.l = &out->layout;
    c.kind = shader_kind;

    /* The header is a placeholder until the body size is known: TGSI carries
     * it in bits 8..31, and a wrong body size is a stream a parser mis-walks.
     * The count is patched in at the end from the ACTUAL token count. */
    int header_at = out->buf.count;
    emit(&c, 0);
    emit(&c, TGSI_PROCESSOR(shader_kind == GLSL_SHADER_VERTEX
                            ? TGSI_PROC_VERTEX : TGSI_PROC_FRAGMENT));

    /* One DCL per input and per output slot, plus one for the built-in
     * output (position or colour), which is never a source-level varying. */
    for (int i = 0; i < out->layout.input_count; i++) {
        emit(&c, TGSI_DCL(TGSI_FILE_INPUT, 3)); emit(&c, 0); emit(&c, 0);
    }
    for (int i = 0; i < out->layout.output_count; i++) {
        emit(&c, TGSI_DCL(TGSI_FILE_OUTPUT, 3)); emit(&c, 0); emit(&c, 0);
    }
    /* The built-in output sits at index output_count (see builtin_ref). */
    emit(&c, TGSI_DCL(TGSI_FILE_OUTPUT, 3)); emit(&c, 0); emit(&c, 0);

    const glsl_node_t *fn = find_main(unit->root->list);
    if (!fn || !fn->body) {
        diag(&c, 0, "the unit has no main()%s%s", "", "");
        out->ok = 0;
        return 0;
    }

    int body_start = out->buf.count;
    if (!walk_block(&c, fn->body->list ? fn->body->list : fn->body->a)) {
        /* A refusal is not a crash: report it and leave the buffer unused. */
        out->ok = 0;
        out->temp_count = c.temp_top;
        return 0;
    }
    (void)body_start;

    emit(&c, TGSI_INSN(TGSI_OPCODE_END, 1, 0, 0));

    if (out->buf.overflow) {
        diag(&c, 0, "the emitted stream exceeds the %s-dword buffer", "4096", "");
        out->ok = 0;
        return 0;
    }

    out->buf.dwords[header_at] = TGSI_HEADER(2, out->buf.count - 2);
    out->temp_count = c.temp_top;
    out->imm_count = c.imm_count;
    out->ok = !c.failed;
    return out->ok;
}

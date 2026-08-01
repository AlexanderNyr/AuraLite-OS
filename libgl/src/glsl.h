/* libgl/src/glsl.h — GLSL ES 1.0 front end: tokens, types and AST.
 *
 * Phase G11a of GL_PLAN.md.  PRIVATE to libgl.
 *
 * SCOPE
 *
 * This header describes the whole front end: lexer (glsl_lex.c), parser
 * (glsl_parse.c) and type checker (glsl_sema.c).  It stops at a typed AST.
 * Executing that AST is phase G11b; wiring it to the pipeline is G11c.
 *
 * Splitting the phase here is deliberate: a front end can be tested to
 * exhaustion on the host with no rasterizer, no context and no window, and a
 * compiler with untested diagnostics is worse than no compiler — an
 * application whose shader silently fails to compile has nothing to go on.
 *
 * MEMORY: ONE ARENA PER COMPILATION
 *
 * Every AST node, type and interned string is allocated from a bump arena
 * owned by the glsl_unit_t.  Freeing is one call, and a parse that aborts
 * halfway through leaks nothing.  The alternative — per-node malloc/free with
 * error paths unwinding partial trees — is where compilers grow their leaks
 * and double frees, and it buys nothing here because a shader is compiled
 * once and its AST lives exactly as long as the shader object.
 *
 * ERRORS ARE VALUES, NOT CONTROL FLOW
 *
 * There is no setjmp and no abort.  A failed parse returns a node of kind
 * GLSL_NODE_ERROR, which propagates: the type checker treats an error type as
 * compatible with everything, so one mistake produces one diagnostic instead
 * of a cascade.  That is what makes the info log readable.
 */
#ifndef AURALITE_GLSL_H
#define AURALITE_GLSL_H

#include <stddef.h>
#include <stdint.h>

#include "GL/gl.h"

/* ============================================================================
 * Limits
 *
 * A shader is user input, so every one of these is a hard bound rather than a
 * guideline: the compiler must terminate and stay inside its arena whatever it
 * is handed.
 * ==========================================================================*/

#define GLSL_MAX_SOURCE      65536   /* bytes of source accepted            */
#define GLSL_MAX_TOKENS      16384
#define GLSL_MAX_ERRORS      32      /* diagnostics kept; the rest counted  */
#define GLSL_MAX_LOG         2048    /* bytes of info log                   */
#define GLSL_MAX_IDENT       64      /* identifier length                   */
#define GLSL_MAX_ARGS        16      /* function parameters / call arguments */
#define GLSL_MAX_FIELDS      32      /* struct members                      */
#define GLSL_MAX_SCOPE_DEPTH 32
#define GLSL_MAX_SYMBOLS     512
#define GLSL_MAX_NEST        64      /* expression / statement nesting      */

/* ============================================================================
 * Tokens
 * ==========================================================================*/

typedef enum {
    GLSL_TOK_EOF = 0,
    GLSL_TOK_IDENT,
    GLSL_TOK_INTCONST,
    GLSL_TOK_FLOATCONST,
    GLSL_TOK_BOOLCONST,

    /* Keywords.  Type names are keywords in GLSL, not identifiers. */
    GLSL_TOK_VOID, GLSL_TOK_BOOL, GLSL_TOK_INT, GLSL_TOK_FLOAT,
    GLSL_TOK_VEC2, GLSL_TOK_VEC3, GLSL_TOK_VEC4,
    GLSL_TOK_IVEC2, GLSL_TOK_IVEC3, GLSL_TOK_IVEC4,
    GLSL_TOK_BVEC2, GLSL_TOK_BVEC3, GLSL_TOK_BVEC4,
    GLSL_TOK_MAT2, GLSL_TOK_MAT3, GLSL_TOK_MAT4,
    GLSL_TOK_SAMPLER2D, GLSL_TOK_SAMPLERCUBE,
    GLSL_TOK_STRUCT,

    GLSL_TOK_ATTRIBUTE, GLSL_TOK_UNIFORM, GLSL_TOK_VARYING, GLSL_TOK_CONST,
    GLSL_TOK_IN, GLSL_TOK_OUT, GLSL_TOK_INOUT,
    GLSL_TOK_LOWP, GLSL_TOK_MEDIUMP, GLSL_TOK_HIGHP, GLSL_TOK_PRECISION,
    GLSL_TOK_INVARIANT,

    GLSL_TOK_IF, GLSL_TOK_ELSE, GLSL_TOK_FOR, GLSL_TOK_WHILE, GLSL_TOK_DO,
    GLSL_TOK_RETURN, GLSL_TOK_BREAK, GLSL_TOK_CONTINUE, GLSL_TOK_DISCARD,

    /* Punctuation and operators. */
    GLSL_TOK_LPAREN, GLSL_TOK_RPAREN, GLSL_TOK_LBRACE, GLSL_TOK_RBRACE,
    GLSL_TOK_LBRACKET, GLSL_TOK_RBRACKET,
    GLSL_TOK_SEMICOLON, GLSL_TOK_COMMA, GLSL_TOK_DOT, GLSL_TOK_COLON,
    GLSL_TOK_QUESTION,

    GLSL_TOK_PLUS, GLSL_TOK_MINUS, GLSL_TOK_STAR, GLSL_TOK_SLASH,
    GLSL_TOK_PERCENT,
    GLSL_TOK_ASSIGN,
    GLSL_TOK_ADD_ASSIGN, GLSL_TOK_SUB_ASSIGN,
    GLSL_TOK_MUL_ASSIGN, GLSL_TOK_DIV_ASSIGN,
    GLSL_TOK_INC, GLSL_TOK_DEC,
    GLSL_TOK_EQ, GLSL_TOK_NE, GLSL_TOK_LT, GLSL_TOK_GT,
    GLSL_TOK_LE, GLSL_TOK_GE,
    GLSL_TOK_AND_AND, GLSL_TOK_OR_OR, GLSL_TOK_XOR_XOR, GLSL_TOK_BANG,

    GLSL_TOK_ERROR               /* an unrecognised character */
} glsl_tok_kind_t;

typedef struct {
    glsl_tok_kind_t kind;
    int             line;
    /* Payload.  Only one is meaningful, selected by `kind`; a union would save
     * 8 bytes per token and cost clarity in every debug session. */
    const char     *text;        /* interned, for IDENT                     */
    double          fval;
    long            ival;
} glsl_token_t;

/* ============================================================================
 * Types
 *
 * GLSL's type system is small enough to describe with a base kind plus two
 * dimensions, which keeps every rule (assignability, constructor arity,
 * component count) arithmetic rather than a table lookup.  Structs are the one
 * exception and carry a field list.
 * ==========================================================================*/

typedef enum {
    GLSL_TY_ERROR = 0,   /* propagates silently; see the header comment */
    GLSL_TY_VOID,
    GLSL_TY_BOOL,
    GLSL_TY_INT,
    GLSL_TY_FLOAT,
    GLSL_TY_VEC,         /* float vector, rows = 2..4                  */
    GLSL_TY_IVEC,
    GLSL_TY_BVEC,
    GLSL_TY_MAT,         /* square float matrix, rows = cols = 2..4    */
    GLSL_TY_SAMPLER2D,
    GLSL_TY_SAMPLERCUBE,
    GLSL_TY_STRUCT
} glsl_ty_kind_t;

typedef struct glsl_type glsl_type_t;

typedef struct {
    const char        *name;
    const glsl_type_t *type;
} glsl_field_t;

struct glsl_type {
    glsl_ty_kind_t kind;
    int            rows;         /* components of a vector, order of a matrix */
    int            array_len;    /* 0 = not an array                          */
    /* Struct payload. */
    const char    *struct_name;
    glsl_field_t   fields[GLSL_MAX_FIELDS];
    int            field_count;
};

/* Storage qualifiers (§4.3).  These decide where a variable's value comes from
 * at run time, which is what G11c will need. */
typedef enum {
    GLSL_Q_NONE = 0,
    GLSL_Q_CONST,
    GLSL_Q_ATTRIBUTE,
    GLSL_Q_UNIFORM,
    GLSL_Q_VARYING,
    GLSL_Q_PARAM_IN,
    GLSL_Q_PARAM_OUT,
    GLSL_Q_PARAM_INOUT
} glsl_qualifier_t;

/* Which shader is being compiled.  It changes the rules, not just the
 * built-ins: `attribute` is illegal in a fragment shader, `discard` is illegal
 * in a vertex shader, and gl_Position must be written by a vertex shader. */
typedef enum {
    GLSL_SHADER_VERTEX = 0,
    GLSL_SHADER_FRAGMENT
} glsl_shader_kind_t;

/* ============================================================================
 * AST
 * ==========================================================================*/

typedef enum {
    GLSL_NODE_ERROR = 0,

    /* Expressions */
    GLSL_NODE_INT_LIT,
    GLSL_NODE_FLOAT_LIT,
    GLSL_NODE_BOOL_LIT,
    GLSL_NODE_IDENT,
    GLSL_NODE_UNARY,
    GLSL_NODE_BINARY,
    GLSL_NODE_ASSIGN,
    GLSL_NODE_CONDITIONAL,       /* ?:                                     */
    GLSL_NODE_CALL,              /* function call OR type constructor      */
    GLSL_NODE_FIELD,             /* .xyz swizzle or .member                */
    GLSL_NODE_INDEX,             /* [] on a vector, matrix or array        */
    GLSL_NODE_POSTFIX,           /* x++ / x--                              */

    /* Statements */
    GLSL_NODE_BLOCK,
    GLSL_NODE_DECL,              /* one declared variable                  */
    GLSL_NODE_EXPR_STMT,
    GLSL_NODE_IF,
    GLSL_NODE_FOR,
    GLSL_NODE_WHILE,
    GLSL_NODE_DO,
    GLSL_NODE_RETURN,
    GLSL_NODE_BREAK,
    GLSL_NODE_CONTINUE,
    GLSL_NODE_DISCARD,
    GLSL_NODE_EMPTY,

    /* Top level */
    GLSL_NODE_FUNCTION,
    GLSL_NODE_UNIT
} glsl_node_kind_t;

typedef struct glsl_node glsl_node_t;

/* A function parameter. */
typedef struct {
    const char        *name;
    const glsl_type_t *type;
    glsl_qualifier_t   qual;
} glsl_param_t;

struct glsl_node {
    glsl_node_kind_t   kind;
    int                line;

    /* Filled in by the type checker.  NULL before semantic analysis, and on a
     * node the checker never reached. */
    const glsl_type_t *type;

    /* Is this expression a compile-time constant?  Needed for array sizes and
     * for `const` initialisers, and cheap to compute while checking. */
    int                is_const;

    /* Can this expression appear on the left of an assignment?  Computed by
     * the checker rather than guessed by the parser, because it depends on
     * the declaration a name resolves to. */
    int                is_lvalue;

    union {
        long        ival;                    /* INT_LIT, BOOL_LIT          */
        double      fval;                    /* FLOAT_LIT                  */
        const char *name;                    /* IDENT, FIELD, CALL callee  */
    } v;

    /* Operator for UNARY / BINARY / ASSIGN / POSTFIX, as a token kind. */
    glsl_tok_kind_t    op;

    /* Children.  Their meaning depends on `kind`; the parser and checker are
     * the only readers, and naming them a/b/c keeps the node small. */
    glsl_node_t       *a, *b, *c, *d;

    /* Statement and argument lists, as a singly linked chain through `next`.
     * A linked list rather than a growable array because the arena makes
     * appending free and nothing ever needs random access. */
    glsl_node_t       *list;
    glsl_node_t       *next;
    int                list_count;

    /* Declaration payload (GLSL_NODE_DECL) and function payload.
     *
     * `params` is a POINTER, allocated only for GLSL_NODE_FUNCTION.  Inlining
     * the array cost 384 bytes on every node of every kind and took a
     * 300-statement shader over the arena; a function node is one in a
     * hundred, so paying for the array only where it is used shrank the node
     * from 504 bytes to 120. */
    glsl_qualifier_t   qual;
    const glsl_type_t *decl_type;
    glsl_param_t      *params;
    int                param_count;
    glsl_node_t       *body;
};

/* ============================================================================
 * Diagnostics
 * ==========================================================================*/

typedef struct {
    int  line;
    char msg[128];
} glsl_diag_t;

/* ============================================================================
 * Compilation unit
 *
 * Owns the arena, so destroying it frees everything the compilation produced.
 * ==========================================================================*/

typedef struct {
    /* Arena.  A single block: a shader that does not fit is rejected with a
     * diagnostic rather than growing without bound, because an arena that can
     * grow invalidates every pointer already handed out. */
    char   *arena;
    size_t  arena_size;
    size_t  arena_used;

    glsl_shader_kind_t kind;

    glsl_token_t *tokens;
    int           token_count;

    glsl_node_t  *root;          /* GLSL_NODE_UNIT, or NULL on failure     */

    glsl_diag_t   diags[GLSL_MAX_ERRORS];
    int           diag_count;    /* capped at GLSL_MAX_ERRORS              */
    int           error_count;   /* total, including those not stored      */

    char          log[GLSL_MAX_LOG];
    int           log_len;

    int           compiled;      /* 1 when the unit is error-free          */
} glsl_unit_t;

/* ============================================================================
 * Interface
 * ==========================================================================*/

/* Compile `source` as `kind`.  Never returns NULL for a non-NULL source: on
 * failure the unit exists and carries the diagnostics, which is the whole
 * point — glGetShaderInfoLog() must have something to report.  Returns NULL
 * only when the arena itself cannot be allocated. */
glsl_unit_t *glsl_compile(const char *source, glsl_shader_kind_t kind);

void glsl_unit_free(glsl_unit_t *u);

/* The info log, always NUL-terminated, empty when there were no diagnostics. */
const char *glsl_unit_log(const glsl_unit_t *u);

/* Individual stages, exposed so tests can drive them in isolation.  Each
 * appends to the unit's diagnostics and returns non-zero on success. */
int  glsl_lex(glsl_unit_t *u, const char *source);
int  glsl_parse(glsl_unit_t *u);
int  glsl_check(glsl_unit_t *u);

/* Rebuild the info log from the recorded diagnostics.  Called at the end of
 * compilation, and again after a run that recorded a runtime diagnostic. */
void glsl_build_log(glsl_unit_t *u);

/* Record a diagnostic.  Safe to call after the cap is reached: the count still
 * rises so the caller can report "and N more". */
void glsl_error(glsl_unit_t *u, int line, const char *fmt, ...);

/* Arena allocation, zero-filled.  Returns NULL when the arena is exhausted,
 * which every caller must treat as a compilation failure rather than a crash. */
void *glsl_alloc(glsl_unit_t *u, size_t n);

/* Intern a string into the arena.  Identifiers are compared by content, not
 * pointer, so this is storage rather than a hash table -- a shader has tens of
 * identifiers, and a table would cost more than it saves. */
const char *glsl_intern(glsl_unit_t *u, const char *s, size_t n);

/* ---- Type helpers, shared by the parser and the checker ---- */

/* Singleton types for everything except structs and arrays.  Returning
 * pointers into a static table means type equality is pointer equality for
 * the common cases, and the checker still compares structurally so an
 * arena-allocated struct type behaves the same. */
const glsl_type_t *glsl_type_basic(glsl_ty_kind_t kind, int rows);
const glsl_type_t *glsl_type_error(void);

int  glsl_type_equal(const glsl_type_t *a, const glsl_type_t *b);

/* Total scalar components: 1 for a scalar, N for vecN, N*N for matN. */
int  glsl_type_components(const glsl_type_t *t);

/* Human-readable name, for diagnostics.  Returns a pointer into a static
 * buffer for structs, so it is not safe to hold across calls -- the diagnostic
 * is formatted immediately. */
const char *glsl_type_name(const glsl_type_t *t);

/* Is `t` a scalar or vector of floats / ints / bools? */
int  glsl_type_is_numeric(const glsl_type_t *t);
int  glsl_type_is_float_based(const glsl_type_t *t);
int  glsl_type_is_int_based(const glsl_type_t *t);
int  glsl_type_is_bool_based(const glsl_type_t *t);
int  glsl_type_is_sampler(const glsl_type_t *t);

/* The scalar element type of a vector or matrix; the type itself otherwise. */
const glsl_type_t *glsl_type_element(const glsl_type_t *t);

/* ============================================================================
 * Execution (phase G11b) — libgl/src/glsl_exec.c
 *
 * An AST-walking interpreter.  The plan allowed for a bytecode VM "if
 * profiling demands it"; it does not, and here is why.  Compiling to bytecode
 * would add a second IR, a second set of bugs and a translation step between
 * them, in exchange for removing a switch dispatch per node.  The cost of a
 * fragment shader here is dominated by the per-component float arithmetic and
 * by the fact that it runs once per PIXEL at all -- an interpreter is one to
 * two orders of magnitude off the fixed-function path either way, and no
 * dispatch strategy closes that gap.  A JIT would, and a JIT is out of scope.
 *
 * The honest framing, matching the G7 finding about vertex arrays: this buys
 * API coverage, not frames per second.
 * ==========================================================================*/

/* A runtime value.
 *
 * Everything is stored as float, including ints and bools, with the type
 * pointer carrying the real shape.  A union of int/float/bool arrays would
 * save nothing -- the components are 4 bytes either way -- and would put a
 * branch on every read.  Integer semantics that actually differ (division,
 * modulus, the fact that `1/2` is 0) are applied at the operator, which is
 * the only place they are observable.
 *
 * 16 components covers mat4, the largest GLSL ES type.  A struct or an array
 * does not fit, which is deliberate: those live in variable STORAGE and are
 * addressed field by field, never copied through an expression temporary.
 */
typedef struct {
    const glsl_type_t *type;
    float              v[16];
} glsl_value_t;

/* How a shader reaches the world outside itself.
 *
 * The interpreter never touches a texture, a uniform store or a vertex
 * attribute directly: it asks through these callbacks.  That keeps G11b
 * testable with no GL context at all -- the unit tests supply their own
 * uniforms and a checkerboard sampler -- and means G11c wires the real
 * pipeline in without changing a line of the interpreter.
 *
 * Every callback may be NULL; the interpreter then supplies zeros, which is
 * what an unset uniform reads as in GL anyway.
 */
typedef struct glsl_env glsl_env_t;

struct glsl_env {
    /* Read an external variable by name.  `out` is pre-filled with zeros of
     * the right type, so a callback that does not know the name can simply
     * return 0 and get GL's own default. */
    int (*read_var)(glsl_env_t *env, const char *name, glsl_value_t *out);

    /* Write a varying or an output.  Called for gl_Position, gl_FragColor and
     * every `varying` a vertex shader assigns. */
    void (*write_var)(glsl_env_t *env, const char *name,
                      const glsl_value_t *val);

    /* Sample a texture.  `unit` is the sampler's value, which is the texture
     * unit number a uniform was set to.  `coord` has 2 components for
     * texture2D and 3 for textureCube. */
    void (*sample)(glsl_env_t *env, int unit, int is_cube,
                   const float *coord, int ncoord, float *rgba_out);

    void *user;                  /* whatever the caller needs */
};

/* Why execution stopped. */
typedef enum {
    GLSL_RUN_OK = 0,
    GLSL_RUN_DISCARD,            /* the fragment shader discarded            */
    GLSL_RUN_ERROR               /* a runtime limit was hit; see the log     */
} glsl_run_status_t;

/* Run main().  The unit must have compiled successfully.
 *
 * Returns GLSL_RUN_DISCARD when a fragment shader executed `discard`, which
 * the caller must treat as "produce no fragment" rather than as a failure.
 */
glsl_run_status_t glsl_run(glsl_unit_t *u, glsl_env_t *env);

/* Iteration budget for one glsl_run(), shared across all loops.
 *
 * A shader is untrusted input and `while (true) {}` is a legal program.  On
 * hardware a hung shader hangs the GPU and the watchdog resets it; here it
 * would hang the compositor with no watchdog at all.  A budget turns that
 * into a diagnostic.  100k iterations is far more than any real shader does
 * per invocation and is reached in microseconds if a shader is looping away.
 */
#define GLSL_MAX_ITERATIONS  100000

/* Call depth, to bound recursion.
 *
 * GLSL ES 1.0 forbids recursion outright, so anything approaching this is a
 * shader doing something the specification does not allow -- but the limit
 * has to be reached BEFORE the C stack runs out, or the interpreter faults
 * instead of diagnosing.
 *
 * AuraLite gives a user process a 64 KB stack.  Each interpreted call costs
 * roughly 300 bytes of C stack once the per-frame scratch was moved off it
 * (it was 5.9 KB before, which overflowed at depth 11 and produced a guard
 * page fault rather than a diagnostic -- found by running the test suite on
 * the target, invisible on the host's 8 MB stack).  Sixteen frames is ample
 * for legal shaders, which nest a handful of helpers at most, and leaves the
 * budget comfortable. */
#define GLSL_MAX_CALL_DEPTH  16

/* Runtime variable storage.  Exposed so tests can seed and inspect it. */
#define GLSL_MAX_LOCALS      128

/* How deeply call arguments may nest: max(dot(a, b), 0.0) is two.  Distinct
 * from the call depth because built-in calls do not push a frame, so a
 * shader can nest arguments far deeper than it nests user functions. */
#define GLSL_MAX_ARG_NESTING 24

#endif /* AURALITE_GLSL_H */

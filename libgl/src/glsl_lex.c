/* libgl/src/glsl_lex.c — GLSL ES 1.0 lexer, arena and diagnostics.
 *
 * Phase G11a of GL_PLAN.md.
 *
 * WHAT THE LEXER HANDLES THAT LOOKS LIKE IT SHOULD BE THE PARSER'S JOB
 *
 * Comments and line continuations, because both can appear in the middle of a
 * token and neither should reach the grammar.  The `#version` and `#extension`
 * directives are also consumed here: GLSL ES has no real preprocessor in this
 * implementation, and skipping the directive line is closer to correct than
 * failing on the first line of every real-world shader.
 *
 * LINE NUMBERS ARE THE POINT
 *
 * Every token carries the line it started on, because a diagnostic without a
 * line number is nearly useless in a shader that failed to compile at run
 * time.  Line counting therefore happens in exactly one place -- advance() --
 * and nothing else in this file may step the cursor.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glsl.h"

/* ============================================================================
 * Arena
 * ==========================================================================*/

/* One block, sized for the largest shader this implementation accepts.
 *
 * Measured usage, so the headroom is a number rather than a hope:
 *
 *   a 22-line Blinn-Phong fragment shader   140 KB
 *   a 300-statement synthetic shader        600 KB
 *
 * The floor of 112 KB is the type checker's symbol table, which is a
 * fixed-size structure allocated here rather than on the stack -- a 112 KB
 * local would fault on AuraLite exactly as aglxResize() did in phase G12.
 * The rest scales with the AST, at 128 bytes per node.
 *
 * Sizing the arena up front rather than growing it keeps every pointer the
 * parser hands out valid for the life of the unit.  A growable arena would
 * have to hand out handles instead, which would touch every line of the
 * parser to save memory nobody is short of -- and a shader that needs more
 * than a megabyte of AST is one this rasterizer could never execute anyway.
 * Exhaustion is reported as "shader too complex", not a crash. */
#define GLSL_ARENA_BYTES (1u << 20)

void *glsl_alloc(glsl_unit_t *u, size_t n) {
    /* Align to 8: the arena hands out glsl_node_t and glsl_type_t, both of
     * which contain pointers and doubles. */
    size_t aligned = (n + 7u) & ~(size_t)7u;
    if (aligned > u->arena_size - u->arena_used) return NULL;
    void *p = u->arena + u->arena_used;
    u->arena_used += aligned;
    memset(p, 0, aligned);
    return p;
}

const char *glsl_intern(glsl_unit_t *u, const char *s, size_t n) {
    char *p = (char *)glsl_alloc(u, n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ============================================================================
 * Diagnostics
 * ==========================================================================*/

void glsl_error(glsl_unit_t *u, int line, const char *fmt, ...) {
    u->error_count++;
    if (u->diag_count >= GLSL_MAX_ERRORS) return;

    glsl_diag_t *d = &u->diags[u->diag_count++];
    d->line = line;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->msg, sizeof d->msg, fmt, ap);
    va_end(ap);
}

/* Build the info log from the diagnostics.  The format mirrors what desktop
 * GL drivers emit ("ERROR: 0:12: message") because that is what application
 * log parsers and developers already expect to read.
 *
 * Not static: phase G11b's interpreter can also record diagnostics -- an
 * infinite loop or an exceeded call depth -- and those have to reach the log
 * too, or a shader that fails at RUN time reports nothing at all. */
void glsl_build_log(glsl_unit_t *u) {
    u->log_len = 0;
    u->log[0] = '\0';

    for (int i = 0; i < u->diag_count; i++) {
        int left = (int)sizeof(u->log) - u->log_len - 1;
        if (left <= 0) break;
        int n = snprintf(u->log + u->log_len, (size_t)left,
                         "ERROR: 0:%d: %s\n", u->diags[i].line, u->diags[i].msg);
        if (n < 0) break;
        if (n > left) { u->log_len += left; break; }
        u->log_len += n;
    }

    if (u->error_count > u->diag_count) {
        int left = (int)sizeof(u->log) - u->log_len - 1;
        if (left > 0) {
            int n = snprintf(u->log + u->log_len, (size_t)left,
                             "ERROR: %d more error(s) not shown\n",
                             u->error_count - u->diag_count);
            if (n > 0 && n <= left) u->log_len += n;
        }
    }
    u->log[u->log_len] = '\0';
}

const char *glsl_unit_log(const glsl_unit_t *u) {
    return u ? u->log : "";
}

/* ============================================================================
 * Keyword table
 *
 * A linear scan.  There are 40 keywords and a shader has a few hundred
 * identifiers, so a perfect hash would save microseconds once per compile and
 * cost a generator nobody would maintain.
 * ==========================================================================*/

static const struct { const char *word; glsl_tok_kind_t kind; } keywords[] = {
    { "void",          GLSL_TOK_VOID },
    { "bool",          GLSL_TOK_BOOL },
    { "int",           GLSL_TOK_INT },
    { "float",         GLSL_TOK_FLOAT },
    { "vec2",          GLSL_TOK_VEC2 },
    { "vec3",          GLSL_TOK_VEC3 },
    { "vec4",          GLSL_TOK_VEC4 },
    { "ivec2",         GLSL_TOK_IVEC2 },
    { "ivec3",         GLSL_TOK_IVEC3 },
    { "ivec4",         GLSL_TOK_IVEC4 },
    { "bvec2",         GLSL_TOK_BVEC2 },
    { "bvec3",         GLSL_TOK_BVEC3 },
    { "bvec4",         GLSL_TOK_BVEC4 },
    { "mat2",          GLSL_TOK_MAT2 },
    { "mat3",          GLSL_TOK_MAT3 },
    { "mat4",          GLSL_TOK_MAT4 },
    { "sampler2D",     GLSL_TOK_SAMPLER2D },
    { "samplerCube",   GLSL_TOK_SAMPLERCUBE },
    { "struct",        GLSL_TOK_STRUCT },
    { "attribute",     GLSL_TOK_ATTRIBUTE },
    { "uniform",       GLSL_TOK_UNIFORM },
    { "varying",       GLSL_TOK_VARYING },
    { "const",         GLSL_TOK_CONST },
    { "in",            GLSL_TOK_IN },
    { "out",           GLSL_TOK_OUT },
    { "inout",         GLSL_TOK_INOUT },
    { "lowp",          GLSL_TOK_LOWP },
    { "mediump",       GLSL_TOK_MEDIUMP },
    { "highp",         GLSL_TOK_HIGHP },
    { "precision",     GLSL_TOK_PRECISION },
    { "invariant",     GLSL_TOK_INVARIANT },
    { "if",            GLSL_TOK_IF },
    { "else",          GLSL_TOK_ELSE },
    { "for",           GLSL_TOK_FOR },
    { "while",         GLSL_TOK_WHILE },
    { "do",            GLSL_TOK_DO },
    { "return",        GLSL_TOK_RETURN },
    { "break",         GLSL_TOK_BREAK },
    { "continue",      GLSL_TOK_CONTINUE },
    { "discard",       GLSL_TOK_DISCARD },
    { "true",          GLSL_TOK_BOOLCONST },
    { "false",         GLSL_TOK_BOOLCONST },
};

/* GLSL reserves words it does not yet use, so that a shader written today
 * cannot break when a later version defines them.  Diagnosing them by name is
 * far kinder than "syntax error near 'double'". */
static const char *reserved[] = {
    "asm", "class", "union", "enum", "typedef", "template", "this", "packed",
    "goto", "switch", "default", "inline", "noinline", "volatile", "public",
    "static", "extern", "external", "interface", "flat", "long", "short",
    "double", "half", "fixed", "unsigned", "superp", "input", "output",
    "hvec2", "hvec3", "hvec4", "dvec2", "dvec3", "dvec4", "fvec2", "fvec3",
    "fvec4", "sampler1D", "sampler3D", "sampler1DShadow", "sampler2DShadow",
    "sampler2DRect", "sampler3DRect", "sampler2DRectShadow", "sizeof", "cast",
    "namespace", "using",
};

/* ============================================================================
 * Lexer state
 * ==========================================================================*/

typedef struct {
    const char  *p;
    int          line;
    glsl_unit_t *u;
} lexer_t;

static int is_ident_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_ident_char(int c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}
static int is_digit(int c) { return c >= '0' && c <= '9'; }

/* The ONLY function permitted to move the cursor, so line counting cannot
 * drift out of step with it. */
static char advance(lexer_t *lx) {
    char c = *lx->p;
    if (c == '\0') return '\0';
    lx->p++;
    if (c == '\n') lx->line++;
    return c;
}

static char peek(const lexer_t *lx)  { return lx->p[0]; }
static char peek2(const lexer_t *lx) { return lx->p[0] ? lx->p[1] : '\0'; }

static int match(lexer_t *lx, char c) {
    if (peek(lx) != c) return 0;
    advance(lx);
    return 1;
}

/* Skip whitespace, comments, line continuations and directive lines.
 * Returns 0 only on an unterminated block comment, which is an error the
 * caller must report before it produces a cascade of nonsense tokens. */
static int skip_trivia(lexer_t *lx) {
    for (;;) {
        char c = peek(lx);

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
            c == '\v') {
            advance(lx);
            continue;
        }

        /* A backslash at end of line splices the next line on.  Rare in
         * shaders, but a macro-heavy one will have them. */
        if (c == '\\' && (peek2(lx) == '\n' || peek2(lx) == '\r')) {
            advance(lx);
            if (peek(lx) == '\r') advance(lx);
            if (peek(lx) == '\n') advance(lx);
            continue;
        }

        if (c == '/' && peek2(lx) == '/') {
            while (peek(lx) && peek(lx) != '\n') advance(lx);
            continue;
        }

        if (c == '/' && peek2(lx) == '*') {
            int start = lx->line;
            advance(lx); advance(lx);
            for (;;) {
                if (peek(lx) == '\0') {
                    glsl_error(lx->u, start, "unterminated comment");
                    return 0;
                }
                if (peek(lx) == '*' && peek2(lx) == '/') {
                    advance(lx); advance(lx);
                    break;
                }
                advance(lx);
            }
            continue;
        }

        /* Directives.  There is no preprocessor here, so `#version 100` and
         * `#extension ... : enable` are skipped rather than rejected: failing
         * on the first line of every conformant shader would be worse than
         * ignoring a version this implementation only has one of.  Anything
         * else beginning with '#' is diagnosed, because silently dropping a
         * #define would change the shader's meaning. */
        if (c == '#') {
            const char *q = lx->p + 1;
            while (*q == ' ' || *q == '\t') q++;
            int known = (strncmp(q, "version", 7) == 0) ||
                        (strncmp(q, "extension", 9) == 0) ||
                        (strncmp(q, "line", 4) == 0) ||
                        (strncmp(q, "pragma", 6) == 0);
            if (!known) {
                glsl_error(lx->u, lx->line,
                           "preprocessor directives other than #version, "
                           "#extension, #line and #pragma are not supported");
            }
            while (peek(lx) && peek(lx) != '\n') advance(lx);
            continue;
        }

        return 1;
    }
}

static void push(glsl_unit_t *u, glsl_tok_kind_t kind, int line) {
    if (u->token_count >= GLSL_MAX_TOKENS) return;
    glsl_token_t *t = &u->tokens[u->token_count++];
    t->kind = kind;
    t->line = line;
    t->text = NULL;
    t->fval = 0.0;
    t->ival = 0;
}

static glsl_token_t *last(glsl_unit_t *u) {
    return u->token_count ? &u->tokens[u->token_count - 1] : NULL;
}

/* ---- Numbers ----
 *
 * GLSL ES distinguishes int and float literals by spelling: a decimal point or
 * an exponent makes it a float.  `1` and `1.0` are different types, and code
 * relies on that -- `vec2(1, 2)` is legal only because constructors convert,
 * while `1 / 2` is integer division.
 */
static void lex_number(lexer_t *lx) {
    int line = lx->line;
    const char *start = lx->p;
    int is_float = 0;

    /* Hex integers are GLSL ES 3.0, but accepting them costs three lines and
     * refusing them surprises people porting from desktop GL. */
    if (peek(lx) == '0' && (peek2(lx) == 'x' || peek2(lx) == 'X')) {
        advance(lx); advance(lx);
        while (is_digit(peek(lx)) ||
               (peek(lx) >= 'a' && peek(lx) <= 'f') ||
               (peek(lx) >= 'A' && peek(lx) <= 'F')) {
            advance(lx);
        }
        push(lx->u, GLSL_TOK_INTCONST, line);
        glsl_token_t *t = last(lx->u);
        if (t) t->ival = strtol(start, NULL, 16);
        return;
    }

    while (is_digit(peek(lx))) advance(lx);

    if (peek(lx) == '.' && is_digit(peek2(lx))) {
        is_float = 1;
        advance(lx);
        while (is_digit(peek(lx))) advance(lx);
    } else if (peek(lx) == '.' && !is_ident_start(peek2(lx))) {
        /* "1." is a valid float; "1.x" is a swizzle on an int, which is not,
         * but that is the type checker's diagnosis to make, not the lexer's. */
        is_float = 1;
        advance(lx);
    }

    if (peek(lx) == 'e' || peek(lx) == 'E') {
        char n1 = peek2(lx);
        const char *save = lx->p;
        int save_line = lx->line;
        advance(lx);
        if (n1 == '+' || n1 == '-') advance(lx);
        if (is_digit(peek(lx))) {
            is_float = 1;
            while (is_digit(peek(lx))) advance(lx);
        } else {
            /* Not an exponent after all: "1eq" is "1" followed by "eq". */
            lx->p = save;
            lx->line = save_line;
        }
    }

    /* A trailing 'f' suffix is desktop GLSL, not ES, but shaders carry it. */
    if (peek(lx) == 'f' || peek(lx) == 'F') {
        is_float = 1;
        advance(lx);
    }

    push(lx->u, is_float ? GLSL_TOK_FLOATCONST : GLSL_TOK_INTCONST, line);
    glsl_token_t *t = last(lx->u);
    if (!t) return;
    if (is_float) t->fval = strtod(start, NULL);
    else          t->ival = strtol(start, NULL, 10);
}

static void lex_ident(lexer_t *lx) {
    int line = lx->line;
    const char *start = lx->p;
    while (is_ident_char(peek(lx))) advance(lx);
    size_t n = (size_t)(lx->p - start);

    if (n > GLSL_MAX_IDENT) {
        glsl_error(lx->u, line, "identifier longer than %d characters",
                   GLSL_MAX_IDENT);
        n = GLSL_MAX_IDENT;
    }

    for (size_t k = 0; k < sizeof keywords / sizeof keywords[0]; k++) {
        if (strlen(keywords[k].word) == n &&
            strncmp(keywords[k].word, start, n) == 0) {
            push(lx->u, keywords[k].kind, line);
            glsl_token_t *t = last(lx->u);
            if (t && keywords[k].kind == GLSL_TOK_BOOLCONST) {
                t->ival = (start[0] == 't');
            }
            return;
        }
    }

    for (size_t k = 0; k < sizeof reserved / sizeof reserved[0]; k++) {
        if (strlen(reserved[k]) == n && strncmp(reserved[k], start, n) == 0) {
            glsl_error(lx->u, line, "'%s' is a reserved word", reserved[k]);
            break;
        }
    }

    /* GLSL reserves the gl_ prefix for the implementation (§3.7).  Shaders
     * that ignore this break when a later version adds the name they chose. */
    if (n > 3 && strncmp(start, "gl_", 3) == 0) {
        /* Only user-DECLARED gl_ names are illegal; using a built-in is the
         * point.  The parser knows which is which, so this is left to the
         * type checker and only the token is produced here. */
    }

    push(lx->u, GLSL_TOK_IDENT, line);
    glsl_token_t *t = last(lx->u);
    if (t) t->text = glsl_intern(lx->u, start, n);
}

int glsl_lex(glsl_unit_t *u, const char *source) {
    lexer_t lx;
    lx.p = source;
    lx.line = 1;
    lx.u = u;

    for (;;) {
        if (!skip_trivia(&lx)) break;

        char c = peek(&lx);
        if (c == '\0') break;

        if (u->token_count >= GLSL_MAX_TOKENS) {
            glsl_error(u, lx.line, "shader too large (over %d tokens)",
                       GLSL_MAX_TOKENS);
            break;
        }

        int line = lx.line;

        if (is_digit(c) || (c == '.' && is_digit(peek2(&lx)))) {
            lex_number(&lx);
            continue;
        }
        if (is_ident_start(c)) {
            lex_ident(&lx);
            continue;
        }

        advance(&lx);
        switch (c) {
        case '(': push(u, GLSL_TOK_LPAREN, line); break;
        case ')': push(u, GLSL_TOK_RPAREN, line); break;
        case '{': push(u, GLSL_TOK_LBRACE, line); break;
        case '}': push(u, GLSL_TOK_RBRACE, line); break;
        case '[': push(u, GLSL_TOK_LBRACKET, line); break;
        case ']': push(u, GLSL_TOK_RBRACKET, line); break;
        case ';': push(u, GLSL_TOK_SEMICOLON, line); break;
        case ',': push(u, GLSL_TOK_COMMA, line); break;
        case '.': push(u, GLSL_TOK_DOT, line); break;
        case ':': push(u, GLSL_TOK_COLON, line); break;
        case '?': push(u, GLSL_TOK_QUESTION, line); break;
        case '~':
            glsl_error(u, line, "bitwise operators are not supported "
                                "in GLSL ES 1.0");
            break;

        case '+':
            if (match(&lx, '+'))      push(u, GLSL_TOK_INC, line);
            else if (match(&lx, '=')) push(u, GLSL_TOK_ADD_ASSIGN, line);
            else                      push(u, GLSL_TOK_PLUS, line);
            break;
        case '-':
            if (match(&lx, '-'))      push(u, GLSL_TOK_DEC, line);
            else if (match(&lx, '=')) push(u, GLSL_TOK_SUB_ASSIGN, line);
            else                      push(u, GLSL_TOK_MINUS, line);
            break;
        case '*':
            if (match(&lx, '=')) push(u, GLSL_TOK_MUL_ASSIGN, line);
            else                 push(u, GLSL_TOK_STAR, line);
            break;
        case '/':
            if (match(&lx, '=')) push(u, GLSL_TOK_DIV_ASSIGN, line);
            else                 push(u, GLSL_TOK_SLASH, line);
            break;
        case '%':
            push(u, GLSL_TOK_PERCENT, line);
            break;
        case '=':
            if (match(&lx, '=')) push(u, GLSL_TOK_EQ, line);
            else                 push(u, GLSL_TOK_ASSIGN, line);
            break;
        case '!':
            if (match(&lx, '=')) push(u, GLSL_TOK_NE, line);
            else                 push(u, GLSL_TOK_BANG, line);
            break;
        case '<':
            if (match(&lx, '=')) push(u, GLSL_TOK_LE, line);
            else if (peek(&lx) == '<') {
                advance(&lx);
                glsl_error(u, line, "shift operators are not supported "
                                    "in GLSL ES 1.0");
            } else push(u, GLSL_TOK_LT, line);
            break;
        case '>':
            if (match(&lx, '=')) push(u, GLSL_TOK_GE, line);
            else if (peek(&lx) == '>') {
                advance(&lx);
                glsl_error(u, line, "shift operators are not supported "
                                    "in GLSL ES 1.0");
            } else push(u, GLSL_TOK_GT, line);
            break;
        case '&':
            if (match(&lx, '&')) push(u, GLSL_TOK_AND_AND, line);
            else glsl_error(u, line, "bitwise operators are not supported "
                                     "in GLSL ES 1.0");
            break;
        case '|':
            if (match(&lx, '|')) push(u, GLSL_TOK_OR_OR, line);
            else glsl_error(u, line, "bitwise operators are not supported "
                                     "in GLSL ES 1.0");
            break;
        case '^':
            if (match(&lx, '^')) push(u, GLSL_TOK_XOR_XOR, line);
            else glsl_error(u, line, "bitwise operators are not supported "
                                     "in GLSL ES 1.0");
            break;

        default:
            glsl_error(u, line, "unexpected character '%c'", c);
            break;
        }
    }

    push(u, GLSL_TOK_EOF, lx.line);
    return u->error_count == 0;
}

/* ============================================================================
 * Driver
 * ==========================================================================*/

glsl_unit_t *glsl_compile(const char *source, glsl_shader_kind_t kind) {
    if (!source) return NULL;

    glsl_unit_t *u = (glsl_unit_t *)calloc(1, sizeof(glsl_unit_t));
    if (!u) return NULL;

    u->arena = (char *)malloc(GLSL_ARENA_BYTES);
    if (!u->arena) { free(u); return NULL; }
    u->arena_size = GLSL_ARENA_BYTES;
    u->arena_used = 0;
    u->kind = kind;

    u->tokens = (glsl_token_t *)calloc(GLSL_MAX_TOKENS, sizeof(glsl_token_t));
    if (!u->tokens) { free(u->arena); free(u); return NULL; }

    size_t len = strlen(source);
    if (len > GLSL_MAX_SOURCE) {
        glsl_error(u, 1, "shader source exceeds %d bytes", GLSL_MAX_SOURCE);
        glsl_build_log(u);
        return u;
    }

    /* Each stage runs only if the previous one produced something usable.
     * Parsing a token stream with unterminated comments, or checking a tree
     * full of error nodes, produces noise that buries the real diagnostic. */
    if (glsl_lex(u, source)) {
        if (glsl_parse(u)) {
            glsl_check(u);
        }
    }

    u->compiled = (u->error_count == 0);
    glsl_build_log(u);
    return u;
}

void glsl_unit_free(glsl_unit_t *u) {
    if (!u) return;
    free(u->tokens);
    free(u->arena);      /* every node, type and string in one call */
    free(u);
}

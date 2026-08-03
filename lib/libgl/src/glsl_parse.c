/* libgl/src/glsl_parse.c — GLSL ES 1.0 recursive-descent parser.
 *
 * Phase G11a of GL_PLAN.md.
 *
 * WHY RECURSIVE DESCENT
 *
 * GLSL's grammar is small, its precedence table is fixed, and the language has
 * exactly one genuine ambiguity (see "the declaration/expression problem"
 * below).  A hand-written parser is therefore about 900 lines, needs no
 * generator in the build, and -- the part that matters -- can produce a
 * diagnostic that names what it expected.  A generated parser would say
 * "syntax error" and the shader author would be no wiser.
 *
 * ERROR RECOVERY
 *
 * On an unexpected token the parser records one diagnostic and synchronises to
 * the next `;` or `}`.  That is enough to keep reporting real errors further
 * down the file without the cascade that comes from trying to continue inside
 * a broken construct.  A recovery point is never crossed silently: if
 * synchronising consumes a closing brace the enclosing block ends there.
 *
 * THE DECLARATION/EXPRESSION PROBLEM
 *
 * Inside a block, `vec3 x;` is a declaration and `f(x);` is an expression, and
 * both start with an identifier-shaped token.  GLSL solves this the way C
 * does: type names are KEYWORDS, so the lexer has already disambiguated
 * everything except user-defined struct types.  For those the parser consults
 * the set of struct names it has seen -- which is why struct declarations are
 * recorded here rather than left entirely to the type checker.
 *
 * DEPTH LIMITS
 *
 * A shader is untrusted input, so every recursive production checks a depth
 * counter.  Without it `((((((...` recurses until the stack runs out, which on
 * AuraLite means a page fault in user mode rather than a diagnostic.
 */

#include <string.h>

#include "glsl.h"

/* ============================================================================
 * Parser state
 * ==========================================================================*/

#define MAX_STRUCT_NAMES 32

typedef struct {
    glsl_unit_t *u;
    int          pos;            /* index into u->tokens                   */
    int          depth;          /* recursion guard                        */
    int          panic;          /* suppress cascading diagnostics         */

    /* User-defined struct type names, for the declaration/expression
     * disambiguation described above. */
    const char  *struct_names[MAX_STRUCT_NAMES];
    const glsl_type_t *struct_types[MAX_STRUCT_NAMES];
    int          struct_count;
} parser_t;

static glsl_node_t *parse_expression(parser_t *p);
static glsl_node_t *parse_assignment(parser_t *p);
static glsl_node_t *parse_statement(parser_t *p);
static glsl_node_t *parse_block(parser_t *p);
static const glsl_type_t *parse_type(parser_t *p, glsl_qualifier_t *qual_out);

/* ---- Token access ---- */

static const glsl_token_t *cur(parser_t *p) {
    return &p->u->tokens[p->pos];
}

static glsl_tok_kind_t peek_kind(parser_t *p) { return cur(p)->kind; }

static glsl_tok_kind_t peek_kind_at(parser_t *p, int off) {
    int i = p->pos + off;
    if (i >= p->u->token_count) i = p->u->token_count - 1;
    return p->u->tokens[i].kind;
}

static int cur_line(parser_t *p) { return cur(p)->line; }

static const glsl_token_t *bump(parser_t *p) {
    const glsl_token_t *t = cur(p);
    if (t->kind != GLSL_TOK_EOF) p->pos++;
    return t;
}

static int accept(parser_t *p, glsl_tok_kind_t k) {
    if (peek_kind(p) != k) return 0;
    bump(p);
    return 1;
}

/* Printable name of a token kind, for "expected X, found Y" diagnostics.
 * Worth the table: "expected ';'" is actionable, "expected token 42" is not. */
static const char *tok_name(glsl_tok_kind_t k) {
    switch (k) {
    case GLSL_TOK_EOF:        return "end of shader";
    case GLSL_TOK_IDENT:      return "identifier";
    case GLSL_TOK_INTCONST:   return "integer constant";
    case GLSL_TOK_FLOATCONST: return "float constant";
    case GLSL_TOK_BOOLCONST:  return "boolean constant";
    case GLSL_TOK_LPAREN:     return "'('";
    case GLSL_TOK_RPAREN:     return "')'";
    case GLSL_TOK_LBRACE:     return "'{'";
    case GLSL_TOK_RBRACE:     return "'}'";
    case GLSL_TOK_LBRACKET:   return "'['";
    case GLSL_TOK_RBRACKET:   return "']'";
    case GLSL_TOK_SEMICOLON:  return "';'";
    case GLSL_TOK_COMMA:      return "','";
    case GLSL_TOK_DOT:        return "'.'";
    case GLSL_TOK_COLON:      return "':'";
    case GLSL_TOK_QUESTION:   return "'?'";
    case GLSL_TOK_ASSIGN:     return "'='";
    case GLSL_TOK_WHILE:      return "'while'";
    case GLSL_TOK_STRUCT:     return "'struct'";
    default:                  return "token";
    }
}

static glsl_node_t *new_node(parser_t *p, glsl_node_kind_t kind, int line) {
    glsl_node_t *n = (glsl_node_t *)glsl_alloc(p->u, sizeof(glsl_node_t));
    if (!n) {
        /* Arena exhaustion is a compile failure, not a crash.  Reporting it
         * once and letting NULL propagate is safe because every consumer
         * checks -- and the panic flag stops a flood of follow-on errors. */
        if (!p->panic) {
            glsl_error(p->u, line, "shader too complex (compiler out of memory)");
            p->panic = 1;
        }
        return NULL;
    }
    n->kind = kind;
    n->line = line;
    return n;
}

static glsl_node_t *error_node(parser_t *p, int line) {
    glsl_node_t *n = new_node(p, GLSL_NODE_ERROR, line);
    if (n) n->type = glsl_type_error();
    return n;
}

/* Report an unexpected token, unless already recovering. */
static void expected(parser_t *p, const char *what) {
    if (p->panic) return;
    glsl_error(p->u, cur_line(p), "expected %s", what);
    p->panic = 1;
}

/* Skip to a point where parsing can plausibly resume. */
static void synchronise(parser_t *p) {
    while (peek_kind(p) != GLSL_TOK_EOF) {
        if (peek_kind(p) == GLSL_TOK_SEMICOLON) { bump(p); break; }
        if (peek_kind(p) == GLSL_TOK_RBRACE)    { break; }
        bump(p);
    }
    p->panic = 0;
}

static int expect(parser_t *p, glsl_tok_kind_t k) {
    if (accept(p, k)) {
        /* Reaching a statement or declaration terminator means the parser is
         * back in step with the source, so the next mistake deserves its own
         * diagnostic.  Without this, one bad expression silences every error
         * after it and the author fixes them one recompile at a time. */
        if (k == GLSL_TOK_SEMICOLON || k == GLSL_TOK_RBRACE) p->panic = 0;
        return 1;
    }
    expected(p, tok_name(k));
    return 0;
}

/* Recursion guard.  Returns 0 when the limit is hit, having reported it. */
static int enter(parser_t *p) {
    if (p->depth >= GLSL_MAX_NEST) {
        if (!p->panic) {
            glsl_error(p->u, cur_line(p),
                       "expression or statement nested more than %d deep",
                       GLSL_MAX_NEST);
            p->panic = 1;
        }
        return 0;
    }
    p->depth++;
    return 1;
}
static void leave(parser_t *p) { p->depth--; }

/* ---- Struct name table ---- */

static const glsl_type_t *lookup_struct(parser_t *p, const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < p->struct_count; i++) {
        if (strcmp(p->struct_names[i], name) == 0) return p->struct_types[i];
    }
    return NULL;
}

/* ============================================================================
 * Types
 * ==========================================================================*/

/* Is this token the start of a type specifier?  Used to tell a declaration
 * from an expression statement. */
static int starts_type(parser_t *p) {
    switch (peek_kind(p)) {
    case GLSL_TOK_VOID: case GLSL_TOK_BOOL: case GLSL_TOK_INT:
    case GLSL_TOK_FLOAT:
    case GLSL_TOK_VEC2: case GLSL_TOK_VEC3: case GLSL_TOK_VEC4:
    case GLSL_TOK_IVEC2: case GLSL_TOK_IVEC3: case GLSL_TOK_IVEC4:
    case GLSL_TOK_BVEC2: case GLSL_TOK_BVEC3: case GLSL_TOK_BVEC4:
    case GLSL_TOK_MAT2: case GLSL_TOK_MAT3: case GLSL_TOK_MAT4:
    case GLSL_TOK_SAMPLER2D: case GLSL_TOK_SAMPLERCUBE:
    case GLSL_TOK_STRUCT:
    case GLSL_TOK_CONST: case GLSL_TOK_ATTRIBUTE: case GLSL_TOK_UNIFORM:
    case GLSL_TOK_VARYING: case GLSL_TOK_IN: case GLSL_TOK_OUT:
    case GLSL_TOK_INOUT: case GLSL_TOK_INVARIANT:
    case GLSL_TOK_LOWP: case GLSL_TOK_MEDIUMP: case GLSL_TOK_HIGHP:
        return 1;
    case GLSL_TOK_IDENT:
        /* A user struct name, and only when followed by an identifier -- so
         * `Light l;` is a declaration but `Light(1.0)` is a constructor call. */
        return lookup_struct(p, cur(p)->text) != NULL &&
               peek_kind_at(p, 1) == GLSL_TOK_IDENT;
    default:
        return 0;
    }
}

/* Map a type keyword to a type.  Returns NULL when the token is not one. */
static const glsl_type_t *type_from_token(glsl_tok_kind_t k) {
    switch (k) {
    case GLSL_TOK_VOID:  return glsl_type_basic(GLSL_TY_VOID, 0);
    case GLSL_TOK_BOOL:  return glsl_type_basic(GLSL_TY_BOOL, 1);
    case GLSL_TOK_INT:   return glsl_type_basic(GLSL_TY_INT, 1);
    case GLSL_TOK_FLOAT: return glsl_type_basic(GLSL_TY_FLOAT, 1);
    case GLSL_TOK_VEC2:  return glsl_type_basic(GLSL_TY_VEC, 2);
    case GLSL_TOK_VEC3:  return glsl_type_basic(GLSL_TY_VEC, 3);
    case GLSL_TOK_VEC4:  return glsl_type_basic(GLSL_TY_VEC, 4);
    case GLSL_TOK_IVEC2: return glsl_type_basic(GLSL_TY_IVEC, 2);
    case GLSL_TOK_IVEC3: return glsl_type_basic(GLSL_TY_IVEC, 3);
    case GLSL_TOK_IVEC4: return glsl_type_basic(GLSL_TY_IVEC, 4);
    case GLSL_TOK_BVEC2: return glsl_type_basic(GLSL_TY_BVEC, 2);
    case GLSL_TOK_BVEC3: return glsl_type_basic(GLSL_TY_BVEC, 3);
    case GLSL_TOK_BVEC4: return glsl_type_basic(GLSL_TY_BVEC, 4);
    case GLSL_TOK_MAT2:  return glsl_type_basic(GLSL_TY_MAT, 2);
    case GLSL_TOK_MAT3:  return glsl_type_basic(GLSL_TY_MAT, 3);
    case GLSL_TOK_MAT4:  return glsl_type_basic(GLSL_TY_MAT, 4);
    case GLSL_TOK_SAMPLER2D:   return glsl_type_basic(GLSL_TY_SAMPLER2D, 1);
    case GLSL_TOK_SAMPLERCUBE: return glsl_type_basic(GLSL_TY_SAMPLERCUBE, 1);
    default: return NULL;
    }
}

/* struct Name { type field; ... } */
static const glsl_type_t *parse_struct(parser_t *p) {
    int line = cur_line(p);
    bump(p);                                     /* 'struct' */

    const char *name = NULL;
    if (peek_kind(p) == GLSL_TOK_IDENT) name = bump(p)->text;

    if (!expect(p, GLSL_TOK_LBRACE)) return glsl_type_error();

    glsl_type_t *st = (glsl_type_t *)glsl_alloc(p->u, sizeof(glsl_type_t));
    if (!st) return glsl_type_error();
    st->kind = GLSL_TY_STRUCT;
    st->struct_name = name;

    while (peek_kind(p) != GLSL_TOK_RBRACE && peek_kind(p) != GLSL_TOK_EOF) {
        glsl_qualifier_t q = GLSL_Q_NONE;
        const glsl_type_t *ft = parse_type(p, &q);
        if (!ft) { synchronise(p); continue; }

        /* One `type a, b, c;` line declares several fields. */
        do {
            if (peek_kind(p) != GLSL_TOK_IDENT) {
                expected(p, "field name");
                break;
            }
            const char *fname = bump(p)->text;

            const glsl_type_t *this_ft = ft;
            if (accept(p, GLSL_TOK_LBRACKET)) {
                int len = 0;
                if (peek_kind(p) == GLSL_TOK_INTCONST) len = (int)bump(p)->ival;
                else expected(p, "array size");
                expect(p, GLSL_TOK_RBRACKET);

                glsl_type_t *at =
                    (glsl_type_t *)glsl_alloc(p->u, sizeof(glsl_type_t));
                if (at) { *at = *ft; at->array_len = len; this_ft = at; }
            }

            if (st->field_count >= GLSL_MAX_FIELDS) {
                glsl_error(p->u, line, "struct has more than %d fields",
                           GLSL_MAX_FIELDS);
            } else {
                st->fields[st->field_count].name = fname;
                st->fields[st->field_count].type = this_ft;
                st->field_count++;
            }
        } while (accept(p, GLSL_TOK_COMMA));

        if (!expect(p, GLSL_TOK_SEMICOLON)) synchronise(p);
    }

    expect(p, GLSL_TOK_RBRACE);

    if (st->field_count == 0) {
        glsl_error(p->u, line, "struct must declare at least one field");
    }

    if (name && p->struct_count < MAX_STRUCT_NAMES) {
        p->struct_names[p->struct_count] = name;
        p->struct_types[p->struct_count] = st;
        p->struct_count++;
    }
    return st;
}

/* [qualifiers] [precision] type
 *
 * Precision qualifiers are parsed and discarded: this implementation computes
 * everything in float, so lowp/mediump/highp carry no information.  Rejecting
 * them would break nearly every real ES shader, and honouring them would mean
 * pretending to a precision the rasterizer does not have. */
static const glsl_type_t *parse_type(parser_t *p, glsl_qualifier_t *qual_out) {
    glsl_qualifier_t q = GLSL_Q_NONE;

    /* `invariant` may precede a qualifier; it affects nothing here. */
    accept(p, GLSL_TOK_INVARIANT);

    for (;;) {
        glsl_tok_kind_t k = peek_kind(p);
        if      (k == GLSL_TOK_CONST)     { q = GLSL_Q_CONST;      bump(p); }
        else if (k == GLSL_TOK_ATTRIBUTE) { q = GLSL_Q_ATTRIBUTE;  bump(p); }
        else if (k == GLSL_TOK_UNIFORM)   { q = GLSL_Q_UNIFORM;    bump(p); }
        else if (k == GLSL_TOK_VARYING)   { q = GLSL_Q_VARYING;    bump(p); }
        else if (k == GLSL_TOK_IN)        { q = GLSL_Q_PARAM_IN;   bump(p); }
        else if (k == GLSL_TOK_OUT)       { q = GLSL_Q_PARAM_OUT;  bump(p); }
        else if (k == GLSL_TOK_INOUT)     { q = GLSL_Q_PARAM_INOUT;bump(p); }
        else break;
    }

    while (peek_kind(p) == GLSL_TOK_LOWP || peek_kind(p) == GLSL_TOK_MEDIUMP ||
           peek_kind(p) == GLSL_TOK_HIGHP) {
        bump(p);
    }

    if (qual_out) *qual_out = q;

    if (peek_kind(p) == GLSL_TOK_STRUCT) return parse_struct(p);

    if (peek_kind(p) == GLSL_TOK_IDENT) {
        const glsl_type_t *st = lookup_struct(p, cur(p)->text);
        if (st) { bump(p); return st; }
        expected(p, "type name");
        return NULL;
    }

    const glsl_type_t *t = type_from_token(peek_kind(p));
    if (!t) { expected(p, "type name"); return NULL; }
    bump(p);
    return t;
}

/* ============================================================================
 * Expressions
 *
 * Precedence climbing over the GLSL ES table (§5.1), lowest first:
 *   ?:  then ||  ^^  &&  equality  relational  additive  multiplicative,
 *   then unary, postfix and primary.
 * Assignment is right-associative and handled separately, above ?:.
 * ==========================================================================*/

static glsl_node_t *parse_primary(parser_t *p) {
    int line = cur_line(p);

    switch (peek_kind(p)) {
    case GLSL_TOK_INTCONST: {
        glsl_node_t *n = new_node(p, GLSL_NODE_INT_LIT, line);
        if (n) n->v.ival = bump(p)->ival; else bump(p);
        return n;
    }
    case GLSL_TOK_FLOATCONST: {
        glsl_node_t *n = new_node(p, GLSL_NODE_FLOAT_LIT, line);
        if (n) n->v.fval = bump(p)->fval; else bump(p);
        return n;
    }
    case GLSL_TOK_BOOLCONST: {
        glsl_node_t *n = new_node(p, GLSL_NODE_BOOL_LIT, line);
        if (n) n->v.ival = bump(p)->ival; else bump(p);
        return n;
    }
    case GLSL_TOK_IDENT: {
        /* A struct name followed by '(' is a constructor, not a variable.
         * Only the parser knows the struct names -- they are not in the type
         * checker's symbol table -- so the tag has to be attached here or the
         * checker will report the type as an undeclared identifier. */
        const glsl_type_t *st = lookup_struct(p, cur(p)->text);
        if (st && peek_kind_at(p, 1) == GLSL_TOK_LPAREN) {
            bump(p);                                 /* the name */
            bump(p);                                 /* '('      */
            glsl_node_t *n = new_node(p, GLSL_NODE_CALL, line);
            if (!n) return NULL;
            n->v.name    = st->struct_name;
            n->decl_type = st;
            if (peek_kind(p) != GLSL_TOK_RPAREN) {
                glsl_node_t *tail = NULL;
                do {
                    glsl_node_t *arg = parse_assignment(p);
                    if (!arg) break;
                    if (tail) tail->next = arg; else n->list = arg;
                    tail = arg;
                    n->list_count++;
                } while (accept(p, GLSL_TOK_COMMA));
            }
            expect(p, GLSL_TOK_RPAREN);
            return n;
        }
        glsl_node_t *n = new_node(p, GLSL_NODE_IDENT, line);
        if (n) n->v.name = bump(p)->text; else bump(p);
        return n;
    }
    case GLSL_TOK_LPAREN: {
        bump(p);
        glsl_node_t *e = parse_expression(p);
        expect(p, GLSL_TOK_RPAREN);
        return e;
    }
    default:
        break;
    }

    /* A type name in expression position is a constructor: vec3(1,2,3).
     * Represented as a CALL whose callee is the type's spelling, which keeps
     * one node kind for "apply arguments to a name" and lets the checker
     * decide between a constructor and a function. */
    if (type_from_token(peek_kind(p))) {
        const glsl_type_t *t = type_from_token(peek_kind(p));
        bump(p);
        glsl_node_t *n = new_node(p, GLSL_NODE_CALL, line);
        if (!n) return NULL;
        n->v.name = glsl_intern(p->u, glsl_type_name(t),
                                strlen(glsl_type_name(t)));
        n->decl_type = t;             /* marks it as a constructor */
        if (!expect(p, GLSL_TOK_LPAREN)) return n;
        if (peek_kind(p) != GLSL_TOK_RPAREN) {
            glsl_node_t *tail = NULL;
            do {
                glsl_node_t *arg = parse_assignment(p);
                if (!arg) break;
                if (tail) tail->next = arg; else n->list = arg;
                tail = arg;
                n->list_count++;
            } while (accept(p, GLSL_TOK_COMMA));
        }
        expect(p, GLSL_TOK_RPAREN);
        return n;
    }

    expected(p, "expression");
    glsl_node_t *e = error_node(p, line);
    if (peek_kind(p) != GLSL_TOK_EOF &&
        peek_kind(p) != GLSL_TOK_SEMICOLON &&
        peek_kind(p) != GLSL_TOK_RBRACE) {
        bump(p);                 /* make progress, or the loop never ends */
    }
    return e;
}

static glsl_node_t *parse_postfix(parser_t *p) {
    if (!enter(p)) return error_node(p, cur_line(p));
    glsl_node_t *e = parse_primary(p);

    for (;;) {
        int line = cur_line(p);

        if (peek_kind(p) == GLSL_TOK_LPAREN) {
            /* A call on a plain identifier. */
            bump(p);
            glsl_node_t *call = new_node(p, GLSL_NODE_CALL, line);
            if (!call) break;
            call->v.name = (e && e->kind == GLSL_NODE_IDENT) ? e->v.name : NULL;
            call->a = e;
            if (peek_kind(p) != GLSL_TOK_RPAREN) {
                glsl_node_t *tail = NULL;
                do {
                    glsl_node_t *arg = parse_assignment(p);
                    if (!arg) break;
                    if (tail) tail->next = arg; else call->list = arg;
                    tail = arg;
                    call->list_count++;
                } while (accept(p, GLSL_TOK_COMMA));
            }
            expect(p, GLSL_TOK_RPAREN);
            e = call;
            continue;
        }

        if (peek_kind(p) == GLSL_TOK_DOT) {
            bump(p);
            glsl_node_t *f = new_node(p, GLSL_NODE_FIELD, line);
            if (!f) break;
            f->a = e;
            if (peek_kind(p) == GLSL_TOK_IDENT) f->v.name = bump(p)->text;
            else expected(p, "field or swizzle after '.'");
            e = f;
            continue;
        }

        if (peek_kind(p) == GLSL_TOK_LBRACKET) {
            bump(p);
            glsl_node_t *ix = new_node(p, GLSL_NODE_INDEX, line);
            if (!ix) break;
            ix->a = e;
            ix->b = parse_expression(p);
            expect(p, GLSL_TOK_RBRACKET);
            e = ix;
            continue;
        }

        if (peek_kind(p) == GLSL_TOK_INC || peek_kind(p) == GLSL_TOK_DEC) {
            glsl_node_t *n = new_node(p, GLSL_NODE_POSTFIX, line);
            if (!n) break;
            n->op = bump(p)->kind;
            n->a = e;
            e = n;
            continue;
        }
        break;
    }
    leave(p);
    return e;
}

static glsl_node_t *parse_unary(parser_t *p) {
    glsl_tok_kind_t k = peek_kind(p);
    if (k == GLSL_TOK_PLUS || k == GLSL_TOK_MINUS || k == GLSL_TOK_BANG ||
        k == GLSL_TOK_INC  || k == GLSL_TOK_DEC) {
        int line = cur_line(p);
        if (!enter(p)) return error_node(p, line);
        bump(p);
        glsl_node_t *n = new_node(p, GLSL_NODE_UNARY, line);
        glsl_node_t *operand = parse_unary(p);
        leave(p);
        if (!n) return operand;
        n->op = k;
        n->a  = operand;
        return n;
    }
    return parse_postfix(p);
}

/* Binding power of a binary operator, 0 when it is not one. */
static int binop_prec(glsl_tok_kind_t k) {
    switch (k) {
    case GLSL_TOK_STAR: case GLSL_TOK_SLASH: case GLSL_TOK_PERCENT: return 7;
    case GLSL_TOK_PLUS: case GLSL_TOK_MINUS:                        return 6;
    case GLSL_TOK_LT: case GLSL_TOK_GT:
    case GLSL_TOK_LE: case GLSL_TOK_GE:                             return 5;
    case GLSL_TOK_EQ: case GLSL_TOK_NE:                             return 4;
    case GLSL_TOK_AND_AND:                                          return 3;
    case GLSL_TOK_XOR_XOR:                                          return 2;
    case GLSL_TOK_OR_OR:                                            return 1;
    default:                                                        return 0;
    }
}

static glsl_node_t *parse_binary(parser_t *p, int min_prec) {
    if (!enter(p)) return error_node(p, cur_line(p));
    glsl_node_t *lhs = parse_unary(p);

    for (;;) {
        glsl_tok_kind_t k = peek_kind(p);
        int prec = binop_prec(k);
        if (prec == 0 || prec < min_prec) break;

        int line = cur_line(p);
        bump(p);
        /* All GLSL binary operators are left-associative, so the right side
         * binds only tighter operators. */
        glsl_node_t *rhs = parse_binary(p, prec + 1);

        glsl_node_t *n = new_node(p, GLSL_NODE_BINARY, line);
        if (!n) { lhs = rhs; break; }
        n->op = k;
        n->a  = lhs;
        n->b  = rhs;
        lhs = n;
    }
    leave(p);
    return lhs;
}

static glsl_node_t *parse_conditional(parser_t *p) {
    glsl_node_t *cond = parse_binary(p, 1);
    if (peek_kind(p) != GLSL_TOK_QUESTION) return cond;

    int line = cur_line(p);
    if (!enter(p)) return error_node(p, line);
    bump(p);
    glsl_node_t *n = new_node(p, GLSL_NODE_CONDITIONAL, line);
    glsl_node_t *then_e = parse_assignment(p);
    expect(p, GLSL_TOK_COLON);
    glsl_node_t *else_e = parse_assignment(p);
    leave(p);
    if (!n) return then_e;
    n->a = cond; n->b = then_e; n->c = else_e;
    return n;
}

static int is_assign_op(glsl_tok_kind_t k) {
    return k == GLSL_TOK_ASSIGN || k == GLSL_TOK_ADD_ASSIGN ||
           k == GLSL_TOK_SUB_ASSIGN || k == GLSL_TOK_MUL_ASSIGN ||
           k == GLSL_TOK_DIV_ASSIGN;
}

static glsl_node_t *parse_assignment(parser_t *p) {
    glsl_node_t *lhs = parse_conditional(p);

    glsl_tok_kind_t k = peek_kind(p);
    if (!is_assign_op(k)) return lhs;

    int line = cur_line(p);
    if (!enter(p)) return error_node(p, line);
    bump(p);
    /* Right-associative: a = b = c parses as a = (b = c). */
    glsl_node_t *rhs = parse_assignment(p);
    leave(p);

    glsl_node_t *n = new_node(p, GLSL_NODE_ASSIGN, line);
    if (!n) return rhs;
    n->op = k;
    n->a = lhs;
    n->b = rhs;
    return n;
}

static glsl_node_t *parse_expression(parser_t *p) {
    /* GLSL has a comma operator, but only in a for-loop's expressions in
     * practice; parsing it uniformly costs nothing and avoids a surprise. */
    glsl_node_t *e = parse_assignment(p);
    while (peek_kind(p) == GLSL_TOK_COMMA) {
        int line = cur_line(p);
        bump(p);
        glsl_node_t *rhs = parse_assignment(p);
        glsl_node_t *n = new_node(p, GLSL_NODE_BINARY, line);
        if (!n) return rhs;
        n->op = GLSL_TOK_COMMA;
        n->a = e; n->b = rhs;
        e = n;
    }
    return e;
}

/* ============================================================================
 * Declarations and statements
 * ==========================================================================*/

/* Parse `type name [= init] [, name2 [= init]] ;` into a chain of DECL nodes.
 * The caller has already consumed nothing; `semi` says whether to require the
 * terminating semicolon (a for-loop initialiser does not). */
static glsl_node_t *parse_declaration(parser_t *p, int semi) {
    int line = cur_line(p);
    glsl_qualifier_t q = GLSL_Q_NONE;
    const glsl_type_t *base = parse_type(p, &q);
    if (!base) { synchronise(p); return error_node(p, line); }

    /* `struct S { ... };` with no declarator is a type definition only. */
    if (peek_kind(p) == GLSL_TOK_SEMICOLON) {
        bump(p);
        glsl_node_t *n = new_node(p, GLSL_NODE_EMPTY, line);
        return n ? n : error_node(p, line);
    }

    glsl_node_t *head = NULL, *tail = NULL;

    do {
        int dline = cur_line(p);
        if (peek_kind(p) != GLSL_TOK_IDENT) {
            expected(p, "variable name");
            break;
        }
        const char *name = bump(p)->text;

        const glsl_type_t *vt = base;
        if (accept(p, GLSL_TOK_LBRACKET)) {
            int len = 0;
            if (peek_kind(p) == GLSL_TOK_INTCONST) {
                len = (int)bump(p)->ival;
                if (len <= 0) {
                    glsl_error(p->u, dline, "array size must be positive");
                    len = 1;
                }
            } else {
                expected(p, "constant array size");
            }
            expect(p, GLSL_TOK_RBRACKET);
            glsl_type_t *at =
                (glsl_type_t *)glsl_alloc(p->u, sizeof(glsl_type_t));
            if (at) { *at = *base; at->array_len = len; vt = at; }
        }

        glsl_node_t *d = new_node(p, GLSL_NODE_DECL, dline);
        if (!d) break;
        d->v.name    = name;
        d->qual      = q;
        d->decl_type = vt;

        if (accept(p, GLSL_TOK_ASSIGN)) d->a = parse_assignment(p);

        if (tail) tail->next = d; else head = d;
        tail = d;
    } while (accept(p, GLSL_TOK_COMMA));

    if (semi && !expect(p, GLSL_TOK_SEMICOLON)) synchronise(p);
    return head ? head : error_node(p, line);
}

static glsl_node_t *parse_block(parser_t *p) {
    int line = cur_line(p);
    if (!expect(p, GLSL_TOK_LBRACE)) return error_node(p, line);
    if (!enter(p)) return error_node(p, line);

    glsl_node_t *blk = new_node(p, GLSL_NODE_BLOCK, line);
    glsl_node_t *tail = NULL;

    while (peek_kind(p) != GLSL_TOK_RBRACE && peek_kind(p) != GLSL_TOK_EOF) {
        glsl_node_t *s = parse_statement(p);
        if (!s) break;
        if (blk) {
            /* A declaration list is a chain already; splice it in whole so
             * `float a, b;` produces two statements, not one. */
            if (tail) tail->next = s; else blk->list = s;
            while (s->next) { s = s->next; blk->list_count++; }
            tail = s;
            blk->list_count++;
        }
    }

    expect(p, GLSL_TOK_RBRACE);
    leave(p);
    return blk ? blk : error_node(p, line);
}

static glsl_node_t *parse_statement(parser_t *p) {
    int line = cur_line(p);

    switch (peek_kind(p)) {
    case GLSL_TOK_LBRACE:
        return parse_block(p);

    case GLSL_TOK_SEMICOLON: {
        bump(p);
        glsl_node_t *n = new_node(p, GLSL_NODE_EMPTY, line);
        return n ? n : error_node(p, line);
    }

    case GLSL_TOK_IF: {
        bump(p);
        glsl_node_t *n = new_node(p, GLSL_NODE_IF, line);
        if (!enter(p)) return error_node(p, line);
        expect(p, GLSL_TOK_LPAREN);
        glsl_node_t *cond = parse_expression(p);
        expect(p, GLSL_TOK_RPAREN);
        glsl_node_t *then_s = parse_statement(p);
        glsl_node_t *else_s = accept(p, GLSL_TOK_ELSE) ? parse_statement(p)
                                                       : NULL;
        leave(p);
        if (!n) return error_node(p, line);
        n->a = cond; n->b = then_s; n->c = else_s;
        return n;
    }

    case GLSL_TOK_WHILE: {
        bump(p);
        glsl_node_t *n = new_node(p, GLSL_NODE_WHILE, line);
        if (!enter(p)) return error_node(p, line);
        expect(p, GLSL_TOK_LPAREN);
        glsl_node_t *cond = parse_expression(p);
        expect(p, GLSL_TOK_RPAREN);
        glsl_node_t *body = parse_statement(p);
        leave(p);
        if (!n) return error_node(p, line);
        n->a = cond; n->b = body;
        return n;
    }

    case GLSL_TOK_DO: {
        bump(p);
        glsl_node_t *n = new_node(p, GLSL_NODE_DO, line);
        if (!enter(p)) return error_node(p, line);
        glsl_node_t *body = parse_statement(p);
        expect(p, GLSL_TOK_WHILE);
        expect(p, GLSL_TOK_LPAREN);
        glsl_node_t *cond = parse_expression(p);
        expect(p, GLSL_TOK_RPAREN);
        expect(p, GLSL_TOK_SEMICOLON);
        leave(p);
        if (!n) return error_node(p, line);
        n->a = cond; n->b = body;
        return n;
    }

    case GLSL_TOK_FOR: {
        bump(p);
        glsl_node_t *n = new_node(p, GLSL_NODE_FOR, line);
        if (!enter(p)) return error_node(p, line);
        expect(p, GLSL_TOK_LPAREN);

        /* init: a declaration, an expression, or nothing. */
        glsl_node_t *init = NULL;
        if (peek_kind(p) == GLSL_TOK_SEMICOLON) {
            bump(p);
        } else if (starts_type(p)) {
            init = parse_declaration(p, 1);
        } else {
            init = parse_expression(p);
            expect(p, GLSL_TOK_SEMICOLON);
        }

        glsl_node_t *cond = NULL;
        if (peek_kind(p) != GLSL_TOK_SEMICOLON) cond = parse_expression(p);
        expect(p, GLSL_TOK_SEMICOLON);

        glsl_node_t *step = NULL;
        if (peek_kind(p) != GLSL_TOK_RPAREN) step = parse_expression(p);
        expect(p, GLSL_TOK_RPAREN);

        glsl_node_t *body = parse_statement(p);
        leave(p);
        if (!n) return error_node(p, line);
        n->a = init; n->b = cond; n->c = step; n->d = body;
        return n;
    }

    case GLSL_TOK_RETURN: {
        bump(p);
        glsl_node_t *n = new_node(p, GLSL_NODE_RETURN, line);
        if (peek_kind(p) != GLSL_TOK_SEMICOLON) {
            glsl_node_t *e = parse_expression(p);
            if (n) n->a = e;
        }
        if (!expect(p, GLSL_TOK_SEMICOLON)) synchronise(p);
        return n ? n : error_node(p, line);
    }

    case GLSL_TOK_BREAK: case GLSL_TOK_CONTINUE: case GLSL_TOK_DISCARD: {
        glsl_node_kind_t k =
            peek_kind(p) == GLSL_TOK_BREAK    ? GLSL_NODE_BREAK :
            peek_kind(p) == GLSL_TOK_CONTINUE ? GLSL_NODE_CONTINUE
                                              : GLSL_NODE_DISCARD;
        bump(p);
        glsl_node_t *n = new_node(p, k, line);
        if (!expect(p, GLSL_TOK_SEMICOLON)) synchronise(p);
        return n ? n : error_node(p, line);
    }

    default:
        break;
    }

    if (starts_type(p)) return parse_declaration(p, 1);

    glsl_node_t *n = new_node(p, GLSL_NODE_EXPR_STMT, line);
    glsl_node_t *e = parse_expression(p);
    if (!expect(p, GLSL_TOK_SEMICOLON)) synchronise(p);
    if (!n) return error_node(p, line);
    n->a = e;
    return n;
}

/* ============================================================================
 * Top level
 * ==========================================================================*/

/* Either a function (definition or prototype) or a global declaration.  Both
 * begin with a type and a name; the token after the name decides. */
static glsl_node_t *parse_external(parser_t *p) {
    int line = cur_line(p);

    /* `precision mediump float;` — accepted and ignored, as above. */
    if (peek_kind(p) == GLSL_TOK_PRECISION) {
        while (peek_kind(p) != GLSL_TOK_SEMICOLON &&
               peek_kind(p) != GLSL_TOK_EOF) {
            bump(p);
        }
        expect(p, GLSL_TOK_SEMICOLON);
        glsl_node_t *n = new_node(p, GLSL_NODE_EMPTY, line);
        return n ? n : error_node(p, line);
    }

    /* Look ahead for `name (`, which marks a function. */
    int save = p->pos;
    glsl_qualifier_t q = GLSL_Q_NONE;
    const glsl_type_t *rt = parse_type(p, &q);
    if (!rt) { synchronise(p); return error_node(p, line); }

    if (peek_kind(p) == GLSL_TOK_IDENT &&
        peek_kind_at(p, 1) == GLSL_TOK_LPAREN) {
        const char *name = bump(p)->text;
        bump(p);                                 /* '(' */

        glsl_node_t *fn = new_node(p, GLSL_NODE_FUNCTION, line);
        if (!fn) return error_node(p, line);
        fn->v.name    = name;
        fn->decl_type = rt;
        fn->params    = (glsl_param_t *)glsl_alloc(
            p->u, sizeof(glsl_param_t) * GLSL_MAX_ARGS);
        if (!fn->params) return error_node(p, line);

        /* `void f(void)` declares no parameters. */
        if (peek_kind(p) == GLSL_TOK_VOID &&
            peek_kind_at(p, 1) == GLSL_TOK_RPAREN) {
            bump(p);
        } else if (peek_kind(p) != GLSL_TOK_RPAREN) {
            do {
                glsl_qualifier_t pq = GLSL_Q_NONE;
                const glsl_type_t *pt = parse_type(p, &pq);
                if (!pt) break;
                const char *pname = NULL;
                if (peek_kind(p) == GLSL_TOK_IDENT) pname = bump(p)->text;

                if (accept(p, GLSL_TOK_LBRACKET)) {
                    int len = 0;
                    if (peek_kind(p) == GLSL_TOK_INTCONST)
                        len = (int)bump(p)->ival;
                    expect(p, GLSL_TOK_RBRACKET);
                    glsl_type_t *at =
                        (glsl_type_t *)glsl_alloc(p->u, sizeof(glsl_type_t));
                    if (at) { *at = *pt; at->array_len = len; pt = at; }
                }

                if (fn->param_count >= GLSL_MAX_ARGS) {
                    glsl_error(p->u, line, "more than %d parameters",
                               GLSL_MAX_ARGS);
                } else {
                    fn->params[fn->param_count].name = pname;
                    fn->params[fn->param_count].type = pt;
                    fn->params[fn->param_count].qual =
                        (pq == GLSL_Q_NONE) ? GLSL_Q_PARAM_IN : pq;
                    fn->param_count++;
                }
            } while (accept(p, GLSL_TOK_COMMA));
        }
        expect(p, GLSL_TOK_RPAREN);

        if (accept(p, GLSL_TOK_SEMICOLON)) {
            fn->body = NULL;                     /* a prototype */
        } else {
            fn->body = parse_block(p);
        }
        return fn;
    }

    /* Not a function: rewind and parse it as a declaration, so the qualifier
     * and type handling live in one place. */
    p->pos = save;
    return parse_declaration(p, 1);
}

int glsl_parse(glsl_unit_t *u) {
    parser_t p;
    memset(&p, 0, sizeof p);
    p.u = u;

    int line = u->token_count ? u->tokens[0].line : 1;
    glsl_node_t *unit = new_node(&p, GLSL_NODE_UNIT, line);
    if (!unit) return 0;

    glsl_node_t *tail = NULL;
    while (peek_kind(&p) != GLSL_TOK_EOF) {
        int before = p.pos;

        glsl_node_t *ext = parse_external(&p);
        if (ext) {
            if (tail) tail->next = ext; else unit->list = ext;
            while (ext->next) { ext = ext->next; unit->list_count++; }
            tail = ext;
            unit->list_count++;
        }

        /* Guarantee progress.  A production that consumes nothing on a token
         * it cannot handle would spin here forever, and a hang is far worse
         * than a bad diagnostic. */
        if (p.pos == before) {
            if (!p.panic) {
                glsl_error(u, cur_line(&p), "unexpected token at top level");
            }
            bump(&p);
            p.panic = 0;
        }

        if (u->error_count > GLSL_MAX_ERRORS * 4) {
            glsl_error(u, cur_line(&p), "too many errors, giving up");
            break;
        }
    }

    u->root = unit;
    return u->error_count == 0;
}

/*
 * sh_parse.h — quote-aware tokenizer and operator extraction for the
 * AuraLite shell.
 *
 * SELFHOST SH6b introduced quotes and redirects in one pass.  SH6c adds
 * the remaining operators a build script composes commands with:
 *
 *   |    pipe stages of a pipeline
 *   ;    run the next command unconditionally
 *   &&   run the next command only if the previous status is 0
 *   ||   run the next command only if the previous status is nonzero
 *
 * Quotes and operators have to be recognised in the SAME pass, because
 * whether `|` / `;` / `&&` / `||` / `>` is an operator depends on whether
 * it is quoted.  Doing it in two passes (find the operators, then tokenize)
 * is how a shell ends up piping on a `|` that was inside a string.
 *
 * Pure and dependency-free for the same reason sh_expand.h is: the host unit
 * test includes this file and calls the shipped body.
 *
 * Deliberately out of scope:
 *   - `2>` and friends.  The shell writes everything to fd 1; there is no
 *     separate stderr stream to redirect, so `2>file` parses as the word `2`
 *     followed by a redirect.  Adding it would be inventing a stream that
 *     does not exist.
 *   - here-documents.  Not in any sub-phase's scope.
 *   - reserved-word tokens.  SH6d's `if`/`while`/`for` stay SH_TOK_WORD;
 *     the command dispatcher recognises them when they open a line.
 */

#ifndef AURALITE_SH_PARSE_H
#define AURALITE_SH_PARSE_H

#include <stddef.h>

/* Token kinds.  SH_TOK_WORD is 0 so a zeroed struct is a word, not an
 * operator -- a token array that is only partly filled must not look like a
 * list of redirects. */
enum {
    SH_TOK_WORD = 0,   /* a command name or argument */
    SH_TOK_GT,         /* >   truncate and write */
    SH_TOK_GGT,        /* >>  append */
    SH_TOK_LT,         /* <   read */
    SH_TOK_AMP,        /* &   run in the background (list terminator) */
    SH_TOK_PIPE,       /* |   connect stages of a pipeline */
    SH_TOK_SEMI,       /* ;   end of command list, run next unconditionally */
    SH_TOK_ANDAND,     /* &&  run next only if previous status is 0 */
    SH_TOK_OROR        /* ||  run next only if previous status is nonzero */
};

#define SH_PARSE_OK        0   /* return value is the token count */
#define SH_PARSE_TOOMANY  (-1) /* more tokens than the caller's array holds */
#define SH_PARSE_QUOTE    (-2) /* unterminated ' or " */
#define SH_PARSE_NOTARGET (-3) /* a redirect with no filename after it */
#define SH_PARSE_NOCOMMAND (-4) /* | && || with no command where one is due */

struct sh_tok {
    int         type;   /* one of SH_TOK_* */
    const char *text;   /* points INTO the input line; quotes still present */
    size_t      len;
};

/* Split line[0..len) into tokens.
 *
 * Quotes are preserved in the token text on purpose: whether a `$` expands
 * depends on the quote that surrounds it, so quote removal has to happen in
 * the expander, not here.  A word therefore ends at the first UNQUOTED
 * whitespace or operator, and `foo>bar` yields three tokens (word, GT, word)
 * exactly as POSIX does.  `a|b` and `a&&b` likewise split without spaces.
 *
 * Two-character operators (`>>`, `&&`, `||`) are recognised before their
 * one-character prefixes, so `a && b` is ANDAND, not AMP AMP, and `a || b`
 * is OROR, not PIPE PIPE.
 *
 * Returns the token count (>= 0), or a negative SH_PARSE_* code.  Callers
 * must treat the negative codes as errors: silently dropping the rest of a
 * line is how a build step ends up doing something other than what it says.
 */
static int sh_tokenize(const char *line, size_t len,
                       struct sh_tok *toks, int max_toks)
{
    size_t i = 0;
    int n = 0;

    if (!line || !toks || max_toks <= 0) return SH_PARSE_TOOMANY;

    while (i < len && line[i] != '\0') {
        char c = line[i];

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            i++;
            continue;
        }

        if (c == '>' || c == '<' || c == '&' || c == '|' || c == ';') {
            int type;
            size_t oplen = 1;

            if (c == '>' && i + 1 < len && line[i + 1] == '>') {
                type = SH_TOK_GGT;
                oplen = 2;
            } else if (c == '>') {
                type = SH_TOK_GT;
            } else if (c == '<') {
                type = SH_TOK_LT;
            } else if (c == '&' && i + 1 < len && line[i + 1] == '&') {
                type = SH_TOK_ANDAND;
                oplen = 2;
            } else if (c == '&') {
                type = SH_TOK_AMP;
            } else if (c == '|' && i + 1 < len && line[i + 1] == '|') {
                type = SH_TOK_OROR;
                oplen = 2;
            } else if (c == '|') {
                type = SH_TOK_PIPE;
            } else {
                type = SH_TOK_SEMI;
            }

            if (n >= max_toks) return SH_PARSE_TOOMANY;
            toks[n].type = type;
            toks[n].text = line + i;
            toks[n].len  = oplen;
            n++;
            i += oplen;

            /* A redirect with nothing after it is an error, not a no-op:
             * `tcc -o >` truncating an empty file and then "succeeding" is
             * worse than refusing to run.  `|`, `&&` and `||` at end of line
             * likewise have no command where one is due.  `;` and `&` MAY
             * end a line -- an empty trailing command is a no-op, and a
             * trailing `&` backgrounds what came before.  Lookahead only. */
            if (type != SH_TOK_AMP && type != SH_TOK_SEMI) {
                size_t j = i;
                while (j < len && (line[j] == ' ' || line[j] == '\t')) j++;
                if (j >= len || line[j] == '\0') {
                    if (type == SH_TOK_GT || type == SH_TOK_GGT ||
                        type == SH_TOK_LT)
                        return SH_PARSE_NOTARGET;
                    return SH_PARSE_NOCOMMAND;
                }
            }
            continue;
        }

        /* A word. */
        size_t start = i;
        int quote = 0;

        while (i < len && line[i] != '\0') {
            char d = line[i];

            if (quote) {
                if (d == quote) quote = 0;
                i++;
                continue;
            }
            if (d == '\'' || d == '"') { quote = d; i++; continue; }
            if (d == '\\' && i + 1 < len) { i += 2; continue; }
            if (d == ' ' || d == '\t' || d == '\n' || d == '\r') break;
            if (d == '>' || d == '<' || d == '&' || d == '|' || d == ';') break;
            i++;
        }

        if (quote) return SH_PARSE_QUOTE;

        if (n >= max_toks) return SH_PARSE_TOOMANY;
        toks[n].type = SH_TOK_WORD;
        toks[n].text = line + start;
        toks[n].len  = i - start;
        n++;
    }

    return n;
}

/* Name of an operator, for diagnostics. */
static const char *sh_tok_name(int type)
{
    switch (type) {
    case SH_TOK_GT:     return ">";
    case SH_TOK_GGT:    return ">>";
    case SH_TOK_LT:     return "<";
    case SH_TOK_AMP:    return "&";
    case SH_TOK_PIPE:   return "|";
    case SH_TOK_SEMI:   return ";";
    case SH_TOK_ANDAND: return "&&";
    case SH_TOK_OROR:   return "||";
    default:            return "word";
    }
}

#endif /* AURALITE_SH_PARSE_H */

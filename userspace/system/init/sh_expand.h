/*
 * sh_expand.h — parameter and variable expansion for the AuraLite shell.
 *
 * SELFHOST SH6a introduced positional parameters ($0..$9, $#, $?, $$).
 * SELFHOST SH6b adds the two things a build script needs on top of them:
 *
 *   - named variables, `set CC=tcc` then `$CC`;
 *   - quote awareness, because `'...'` must suppress expansion entirely and
 *     `"..."` must allow it.  Without this, `echo '$CC'` and `echo "$CC"`
 *     would be the same command, and a script could not print a literal
 *     dollar sign except by the `$$` special case.
 *
 * The single entry point is sh_expand_word().  The old sh_expand_positional()
 * is gone rather than kept as a wrapper: two expanders with different quoting
 * rules is exactly the kind of pair that drifts, and the only caller wanted
 * the quote-aware one.
 *
 * Expansion runs per WORD, after sh_parse.h has split the line.  That order
 * matters.  Expanding the whole line first would mean a variable whose value
 * contains `>` or a space could inject an operator or an extra argument --
 * the classic shell injection bug.  Expanding each token separately means a
 * value can only ever produce more text inside the argument it landed in.
 *
 * Still pure and dependency-free, so tests/unit/test_sh_expand.c compiles
 * THIS file and calls the shipped body.
 */

#ifndef AURALITE_SH_EXPAND_H
#define AURALITE_SH_EXPAND_H

#include <stddef.h>

/* Return codes for sh_expand_word(). */
#define SH_EXP_OK       0    /* the whole expansion fit in dst */
#define SH_EXP_OVERFLOW (-1) /* dst was too small; dst is still NUL-terminated */

/* A shell variable.  Pointers are borrowed, not copied. */
struct sh_var {
    const char *name;
    const char *value;
};

/* Write an unsigned value in decimal into the end of dst, returning the new
 * length and setting *over when the value did not fully fit.  Keeps the
 * expander free of printf, which init.c must not assume is cheap to pull into
 * a pure helper.
 *
 * The *over report is not optional: without it a numeric substitution that
 * only half fits (say "$?" == 255 into a 2-byte buffer) looks like success
 * and hands back "2".  A silently wrong number is worse than an error --
 * ledger SH-36 is that exact bug. */
static inline size_t sh_expand_putnum(char *dst, size_t len, size_t cap,
                                      unsigned long v, int *over)
{
    char tmp[24];
    size_t n = 0;

    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);

    while (n > 0) {
        if (len + 1 >= cap) { *over = 1; break; }   /* leave room for the NUL */
        dst[len++] = tmp[--n];
    }
    return len;
}

/* Copy a NUL-terminated string into dst without overflowing.  Returns the new
 * length; `over` is set when the copy had to be cut short so the caller can
 * report SH_EXP_OVERFLOW instead of silently truncating. */
static inline size_t sh_expand_putstr(char *dst, size_t len, size_t cap,
                                      const char *s, int *over)
{
    if (!s) return len;
    while (*s) {
        if (len + 1 >= cap) { *over = 1; break; }
        dst[len++] = *s++;
    }
    return len;
}

/* Copy exactly `n` bytes of a non-NUL-terminated run. */
static inline size_t sh_expand_putmem(char *dst, size_t len, size_t cap,
                                      const char *s, size_t n, int *over)
{
    while (n--) {
        if (len + 1 >= cap) { *over = 1; break; }
        dst[len++] = *s++;
    }
    return len;
}

static inline int sh_is_name_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static inline int sh_is_name_char(char c)
{
    return sh_is_name_start(c) || (c >= '0' && c <= '9');
}

/* Find a variable by name.  Returns its value, or NULL when unset. */
static inline const char *sh_var_find(const struct sh_var *vars, int nvars,
                                      const char *name, size_t namelen)
{
    for (int i = 0; vars && i < nvars; i++) {
        const char *n = vars[i].name;
        size_t k = 0;
        if (!n) continue;
        while (k < namelen && n[k] && n[k] == name[k]) k++;
        if (k == namelen && n[k] == '\0') return vars[i].value;
    }
    return 0;
}

/* Expand one '$' sequence.  On entry src[*pi] == '$'; on return *pi points at
 * the first character after the sequence.
 *
 * Recognised:
 *   $$            a literal '$'
 *   $#            argument count, excluding $0
 *   $?            exit status of the previous command
 *   $0 .. $9      positional parameters
 *   $NAME         a shell variable; unset expands to nothing, as in POSIX
 *
 * Anything else leaves the '$' literal and does not consume the next byte, so
 * `$@`, `$*`, `$-`, `${x}` and a trailing `$` all survive for a later phase
 * instead of being eaten here. */
static inline void sh_expand_dollar(const char *src, size_t srclen, size_t *pi,
                                    char *dst, size_t *dlen, size_t cap,
                                    const struct sh_var *vars, int nvars,
                                    char *const *pos, int npos,
                                    int last_status, int *over)
{
    size_t i = *pi + 1;   /* skip the '$' */
    size_t len = *dlen;

    if (i >= srclen || src[i] == '\0') {
        /* A trailing '$' is literal. */
        if (len + 1 >= cap) *over = 1; else dst[len++] = '$';
        *pi = i;
        *dlen = len;
        return;
    }

    char k = src[i];

    if (k == '$') {
        if (len + 1 >= cap) *over = 1; else dst[len++] = '$';
        *pi = i + 1;
    } else if (k == '#') {
        unsigned long n = (npos > 0) ? (unsigned long)(npos - 1) : 0;
        len = sh_expand_putnum(dst, len, cap, n, over);
        *pi = i + 1;
    } else if (k == '?') {
        /* $? prints the status as an unsigned byte, the way every shell does:
         * a signal-killed child shows 128+n, never "-11". */
        len = sh_expand_putnum(dst, len, cap,
                               (unsigned long)(last_status & 0xFF), over);
        *pi = i + 1;
    } else if (k >= '0' && k <= '9') {
        int idx = k - '0';
        const char *v = (pos && idx < npos) ? pos[idx] : 0;
        len = sh_expand_putstr(dst, len, cap, v ? v : "", over);
        *pi = i + 1;
    } else if (sh_is_name_start(k)) {
        size_t start = i;
        while (i < srclen && src[i] != '\0' && sh_is_name_char(src[i])) i++;
        /* Unset expands to nothing rather than to the literal text.  That is
         * the POSIX rule, and it is what makes `$VERBOSE` usable as a flag:
         * unset and empty behave the same. */
        const char *v = sh_var_find(vars, nvars, src + start, i - start);
        len = sh_expand_putstr(dst, len, cap, v ? v : "", over);
        *pi = i;
    } else {
        /* Not a parameter we know: keep the '$' literal and let the main loop
         * handle the next byte, so `${CC}` and `$@` reach SH6c+ unchanged. */
        if (len + 1 >= cap) *over = 1; else dst[len++] = '$';
        *pi = i;
    }

    *dlen = len;
}

/* Expand one word.
 *
 *   src/srclen   the raw token, quotes still present (sh_tokenize preserves
 *                them so this function can decide what they suppress)
 *   vars/nvars   shell variables; may be NULL/0
 *   pos/npos     positional parameters; pos[0] is $0
 *   last_status  value substituted for $?
 *
 * Quote rules:
 *   '...'   everything literal, including '$' and '\'
 *   "..."   '$' sequences expand; '\' escapes $ ` " \ and newline
 *   \x      outside quotes, a literal x
 *
 * dst is always NUL-terminated.  Returns SH_EXP_OK, or SH_EXP_OVERFLOW if the
 * result did not fit -- callers must treat that as an error, because a
 * silently truncated command line is how ledger SH-14's argv cap turned a
 * link line into "unresolved reference to '__libc_start_main'".
 */
static inline int sh_expand_word(const char *src, size_t srclen,
                                 char *dst, size_t cap,
                                 const struct sh_var *vars, int nvars,
                                 char *const *pos, int npos,
                                 int last_status)
{
    size_t len = 0;
    size_t i   = 0;
    int over   = 0;
    int quote  = 0;

    if (!dst || cap == 0) return SH_EXP_OVERFLOW;
    dst[0] = '\0';
    if (!src) return SH_EXP_OK;

    while (i < srclen && src[i] != '\0') {
        char c = src[i];

        if (quote == '\'') {
            /* Single quotes: no escapes, no expansion, not even for '\'. */
            if (c == '\'') { quote = 0; i++; continue; }
            if (len + 1 >= cap) { over = 1; break; }
            dst[len++] = c;
            i++;
            continue;
        }

        if (quote == '"') {
            if (c == '"') { quote = 0; i++; continue; }
            if (c == '\\' && i + 1 < srclen) {
                char nx = src[i + 1];
                /* Inside double quotes a backslash is only special before the
                 * characters that could otherwise be interpreted; before
                 * anything else it stays literal, as POSIX requires. */
                if (nx == '$' || nx == '"' || nx == '\\' || nx == '`' ||
                    nx == '\n') {
                    if (nx == '\n') { i += 2; continue; }   /* line continuation */
                    if (len + 1 >= cap) { over = 1; break; }
                    dst[len++] = nx;
                    i += 2;
                    continue;
                }
            }
            if (c == '$') {
                sh_expand_dollar(src, srclen, &i, dst, &len, cap,
                                 vars, nvars, pos, npos, last_status, &over);
                continue;
            }
            if (len + 1 >= cap) { over = 1; break; }
            dst[len++] = c;
            i++;
            continue;
        }

        /* Unquoted. */
        if (c == '\'' || c == '"') { quote = c; i++; continue; }
        if (c == '\\' && i + 1 < srclen) {
            if (src[i + 1] == '\n') { i += 2; continue; }
            if (len + 1 >= cap) { over = 1; break; }
            dst[len++] = src[i + 1];
            i += 2;
            continue;
        }
        if (c == '$') {
            sh_expand_dollar(src, srclen, &i, dst, &len, cap,
                             vars, nvars, pos, npos, last_status, &over);
            continue;
        }
        if (len + 1 >= cap) { over = 1; break; }
        dst[len++] = c;
        i++;
    }

    dst[len] = '\0';
    return over ? SH_EXP_OVERFLOW : SH_EXP_OK;
}

#endif /* AURALITE_SH_EXPAND_H */

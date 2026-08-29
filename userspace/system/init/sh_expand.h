/*
 * sh_expand.h — positional-parameter expansion for the AuraLite shell.
 *
 * SELFHOST SH6a.  The shell gained a script runner (`sh <file> [args...]`),
 * and a script that cannot see its own arguments cannot be driven:
 * `sh build.sh kernel` has to know that $1 is "kernel".
 *
 * This lives in a header on purpose, not inline in init.c:
 *
 *   - it is pure (no syscalls, no globals, no allocation), so the host
 *     unit test tests/unit/test_sh_expand.c includes THIS file and calls
 *     the real body.  A test that re-implemented the expander would test
 *     the test, which is exactly the trap the SH5d memchr test fell into
 *     before it was rewritten to link the generated impl;
 *   - init.c is compiled freestanding for the guest, so anything that must
 *     be verified on the host has to be reachable without dragging in
 *     unistd.h/sys/wait.h.
 *
 * Scope, deliberately narrow (SH6a only):
 *
 *   $0        script name            ("" when there is no script)
 *   $1 .. $9  positional arguments   ("" when absent)
 *   $#        number of arguments, excluding $0
 *   $?        exit status of the previous command
 *   $$        a literal '$'
 *
 * Anything else after '$' — a name, a brace, nothing — is left verbatim, so
 * `$PATH` survives untouched until named variables arrive in SH6b.  There is
 * no quoting and no word splitting here either: expansion happens on the raw
 * line before the tokenizer runs, which is the same place POSIX shells do it,
 * and quoting is SH6b's job.
 */

#ifndef AURALITE_SH_EXPAND_H
#define AURALITE_SH_EXPAND_H

#include <stddef.h>

/* Return codes for sh_expand_positional(). */
#define SH_EXP_OK       0    /* the whole expansion fit in dst */
#define SH_EXP_OVERFLOW (-1) /* dst was too small; dst is still NUL-terminated */

/* Write an unsigned value in decimal into the end of dst, returning the new
 * length and setting *over when the value did not fully fit.  Keeps the
 * expander free of printf, which init.c must not assume is cheap to pull into
 * a pure helper.
 *
 * The *over report is not optional: without it a numeric substitution that
 * only half fits (say "$?" == 255 into a 2-byte buffer) looks like success
 * and hands back "2".  A silently wrong number is worse than an error. */
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

/* Copy at most `want` bytes from src into dst without overflowing.  Returns
 * the new length; `over` is set when the copy had to be cut short so the
 * caller can report SH_EXP_OVERFLOW instead of silently truncating. */
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

/* Expand the positional parameters in src[0..srclen) into dst.
 *
 *   pos   argv-like array; pos[0] is $0, pos[1] is $1, ...
 *   npos  number of valid entries in pos (0 when there is no script)
 *   last_status  value substituted for $?
 *
 * dst is always NUL-terminated.  Returns SH_EXP_OK, or SH_EXP_OVERFLOW if the
 * result did not fit — callers must treat that as an error, because a
 * silently truncated command line is how SH-14's argv cap turned a link line
 * into "unresolved reference to '__libc_start_main'".
 */
static inline int sh_expand_positional(const char *src, size_t srclen,
                                       char *dst, size_t cap,
                                       char *const *pos, int npos,
                                       int last_status)
{
    size_t len = 0;
    size_t i   = 0;
    int over   = 0;

    if (!dst || cap == 0) return SH_EXP_OVERFLOW;
    dst[0] = '\0';
    if (!src) return SH_EXP_OK;

    while (i < srclen && src[i] != '\0') {
        char c = src[i];

        if (c != '$') {
            if (len + 1 >= cap) { over = 1; break; }
            dst[len++] = c;
            i++;
            continue;
        }

        /* '$' — look at the next byte to decide what it names. */
        i++;
        if (i >= srclen || src[i] == '\0') {
            /* A trailing '$' is literal. */
            if (len + 1 >= cap) { over = 1; break; }
            dst[len++] = '$';
            break;
        }

        char k = src[i];
        i++;

        if (k == '$') {
            if (len + 1 >= cap) { over = 1; break; }
            dst[len++] = '$';
        } else if (k == '#') {
            unsigned long n = (npos > 0) ? (unsigned long)(npos - 1) : 0;
            len = sh_expand_putnum(dst, len, cap, n, &over);
        } else if (k == '?') {
            /* $? prints the status as an unsigned byte, the way every shell
             * does: a signal-killed child shows 128+n, never "-11". */
            len = sh_expand_putnum(dst, len, cap,
                                   (unsigned long)(last_status & 0xFF), &over);
        } else if (k >= '0' && k <= '9') {
            int idx = k - '0';
            const char *v = (pos && idx < npos) ? pos[idx] : "";
            len = sh_expand_putstr(dst, len, cap, v ? v : "", &over);
        } else {
            /* Not a parameter we know.  Emit the '$' and the byte verbatim so
             * `$PATH`, `${x}` and `$-` reach a later phase unchanged rather
             * than being eaten here. */
            if (len + 1 >= cap) { over = 1; break; }
            dst[len++] = '$';
            if (len + 1 >= cap) { over = 1; break; }
            dst[len++] = k;
        }
    }

    dst[len] = '\0';
    return over ? SH_EXP_OVERFLOW : SH_EXP_OK;
}

#endif /* AURALITE_SH_EXPAND_H */

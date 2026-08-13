/* w32/src/w32_argv.c — WIN32_PLAN.md phase W32-6.
 *
 * Turning a Win32 command line into argc/argv.
 *
 * Windows passes a process ONE STRING and makes each program split it
 * itself.  That is the opposite of the POSIX convention, where the kernel
 * hands over an already-split vector, and it means the splitting rules are
 * part of the platform's observable behaviour: a program invoked as
 *
 *     prog "a b" c\"d  e\\\\"f
 *
 * must see exactly the same argv on AuraLite as it would on Windows, or
 * anything that takes a path with a space in it breaks in a way the user
 * will blame on the file, not on the loader.
 *
 * The rules implemented here are the documented ones (see the block comment
 * on parse_arg() for the exact statement, and w32/PROVENANCE.md for where
 * they came from -- published documentation, never a disassembler):
 *
 *   - whitespace separates arguments, except inside quotes;
 *   - backslashes are literal UNLESS they immediately precede a quote;
 *   - 2n backslashes + quote  -> n backslashes, quote toggles quoting;
 *   - 2n+1 backslashes + quote -> n backslashes, then a LITERAL quote;
 *   - "" while already inside quotes produces one literal quote and stays
 *     inside (the rule that lets a quote survive without a backslash).
 *
 * argv[0] is deliberately NOT parsed by those rules.  Windows treats the
 * program name specially: backslashes in it are always literal, because a
 * path is full of them and C:\dir\ must not turn into C:dir.  Only quoting
 * applies.  Getting this wrong is invisible until a program is run from a
 * directory whose name ends in a backslash-quote sequence, which is exactly
 * the kind of bug that survives for years, so it is a separate function with
 * its own tests.
 *
 * This file is pure string handling: no syscalls, no allocation of its own
 * beyond the caller's buffers, so the whole thing is exercised on the host
 * under sanitizers rather than only in QEMU.
 */

#ifndef AURALITE_W32_HOST_TEST
#include <stddef.h>
#endif

#include "w32/w32_argv.h"

/* Whitespace that separates arguments.  Windows uses space and tab only --
 * a newline inside a command line is an ordinary character, not a
 * separator. */
static int is_sep(char c) {
    return c == ' ' || c == '\t';
}

/* Emit one byte into the output buffer if there is room.
 *
 * The measuring pass (buf == NULL) is the same code path as the real one, so
 * the count this returns and the bytes the real pass writes cannot drift
 * apart -- the usual failure mode of a "compute the size, then fill it"
 * pair.  This mirrors the convention w32_utf.h already uses. */
static void emit(char *buf, size_t cap, size_t *len, char c) {
    if (buf && *len < cap) buf[*len] = c;
    (*len)++;
}

/* Parse one ordinary (non-argv[0]) argument starting at *p.
 *
 * Advances *p past the argument and its trailing separator, writing the
 * unescaped bytes through emit().  Returns the number of bytes the argument
 * occupies including its NUL terminator.
 *
 * The backslash rule is the subtle one.  Backslashes are counted but not
 * emitted until we know what follows them: only a quote makes them
 * meaningful.  That is why the run is buffered rather than emitted as we go.
 */
static size_t parse_arg(const char **p, char *buf, size_t cap, size_t *len) {
    size_t start = *len;
    int in_quotes = 0;
    const char *s = *p;

    for (;;) {
        char c = *s;

        if (c == '\0') break;
        if (!in_quotes && is_sep(c)) break;

        if (c == '\\') {
            /* Count the whole run before deciding what it means. */
            size_t nslash = 0;
            while (*s == '\\') { nslash++; s++; }

            if (*s == '"') {
                /* Every PAIR of backslashes is one literal backslash. */
                for (size_t i = 0; i < nslash / 2; i++) emit(buf, cap, len, '\\');
                if (nslash % 2) {
                    /* Odd: the last backslash escapes the quote, so the
                     * quote is data and quoting state does not change. */
                    emit(buf, cap, len, '"');
                    s++;
                } else {
                    /* Even: the quote is a delimiter. */
                    in_quotes = !in_quotes;
                    s++;
                }
            } else {
                /* Not before a quote: all of them are literal. */
                for (size_t i = 0; i < nslash; i++) emit(buf, cap, len, '\\');
            }
            continue;
        }

        if (c == '"') {
            if (in_quotes && s[1] == '"') {
                /* "" inside quotes: one literal quote, still inside.  This
                 * is what lets a program receive a quote without needing a
                 * backslash, and it is why the check is s[1] rather than a
                 * simple toggle. */
                emit(buf, cap, len, '"');
                s += 2;
            } else {
                in_quotes = !in_quotes;
                s++;
            }
            continue;
        }

        emit(buf, cap, len, c);
        s++;
    }

    emit(buf, cap, len, '\0');

    while (is_sep(*s)) s++;
    *p = s;
    return *len - start;
}

/* Parse argv[0], where backslashes are always literal.
 *
 * Only a quote is special, and it only groups: "C:\my dir\a.exe" is one
 * argument and the backslashes inside survive untouched. */
static void parse_argv0(const char **p, char *buf, size_t cap, size_t *len) {
    const char *s = *p;
    int in_quotes = 0;

    while (*s) {
        char c = *s;
        if (c == '"') { in_quotes = !in_quotes; s++; continue; }
        if (!in_quotes && is_sep(c)) break;
        emit(buf, cap, len, c);
        s++;
    }
    emit(buf, cap, len, '\0');

    while (is_sep(*s)) s++;
    *p = s;
}

int w32_cmdline_to_argv(const char *cmdline,
                        char **argv, size_t max_argv,
                        char *buf, size_t buf_cap,
                        size_t *out_argc, size_t *out_bytes) {
    if (!cmdline || !out_argc || !out_bytes) return W32_ARGV_EINVAL;

    size_t len = 0;      /* bytes written (or that would be written) */
    size_t argc = 0;
    const char *p = cmdline;

    /* Leading whitespace is not an empty argument. */
    while (is_sep(*p)) p++;

    /* An entirely empty command line yields argc == 0, not an argv[0] of "".
     * A caller that needs a program name supplies one; inventing one here
     * would hide the fact that it was missing. */
    if (*p == '\0') {
        *out_argc = 0;
        *out_bytes = 0;
        return W32_ARGV_OK;
    }

    /* argv[0] first, under its own rules. */
    if (argv && argc < max_argv) argv[argc] = buf ? buf + len : (char *)0;
    size_t before = len;
    parse_argv0(&p, buf, buf_cap, &len);
    (void)before;
    argc++;

    while (*p) {
        char *slot = buf ? buf + len : (char *)0;
        if (argv && argc < max_argv) argv[argc] = slot;
        parse_arg(&p, buf, buf_cap, &len);
        argc++;
        if (argc > max_argv && argv) {
            /* Report the overflow rather than writing past the vector.  The
             * byte count is still accurate, so a caller can retry with a
             * bigger vector instead of guessing. */
            *out_argc = argc;
            *out_bytes = len;
            return W32_ARGV_ETOOMANY;
        }
    }

    *out_argc = argc;
    *out_bytes = len;

    if (buf && len > buf_cap) return W32_ARGV_ENOSPACE;
    if (argv && argc > max_argv) return W32_ARGV_ETOOMANY;
    return W32_ARGV_OK;
}

/* libc/src/stdio_extra.c — sprintf / sscanf / scanf / tmpfile family (P10) */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>

/* ---- formatted output to a buffer ---- */

int sprintf(char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(str, (size_t)-1 / 2, fmt, ap);
    va_end(ap);
    return n;
}

int vsprintf(char *str, const char *fmt, va_list ap) {
    return vsnprintf(str, (size_t)-1 / 2, fmt, ap);
}

/* ---- minimal formatted input (vsscanf) ----
 *
 * Supports a useful subset: whitespace skipping, literal chars, and the
 * conversions %d %i %u %x %c %s %f/%g/%e (via strtod) and an optional field
 * width.  Returns the number of input items successfully assigned. */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

int vsscanf(const char *str, const char *fmt, va_list ap) {
    const char *s = str;
    int assigned = 0;

    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) {
            s = skip_ws(s);
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) break;
            s++;
            continue;
        }

        /* parse: %[*][width][conv] */
        fmt++;
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }
        int width = 0;
        while (isdigit((unsigned char)*fmt)) { width = width * 10 + (*fmt - '0'); fmt++; }

        char conv = *fmt;
        if (conv == 0) break;

        if (conv != 'c') s = skip_ws(s);
        if (*s == 0 && conv != 'n') break;

        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            int base = (conv == 'x' || conv == 'X') ? 16 : (conv == 'o' ? 8 : 10);
            char *end = NULL;
            long v;
            if (conv == 'u') v = (long)strtoul(s, &end, base);
            else             v = strtol(s, &end, base);
            if (end == s) goto done;
            s = end;
            if (!suppress) {
                if (conv == 'u') *va_arg(ap, unsigned int *) = (unsigned int)v;
                else             *va_arg(ap, int *) = (int)v;
                assigned++;
            }
            break;
        }
        case 'l': {
            /* %ld / %lu / %lx */
            char sub = *++fmt;
            int base = (sub == 'x' || sub == 'X') ? 16 : (sub == 'o' ? 8 : 10);
            char *end = NULL;
            if (sub == 'u') {
                unsigned long v = strtoul(s, &end, base);
                if (end == s) goto done;
                s = end;
                if (!suppress) { *va_arg(ap, unsigned long *) = v; assigned++; }
            } else {
                long v = strtol(s, &end, base);
                if (end == s) goto done;
                s = end;
                if (!suppress) { *va_arg(ap, long *) = v; assigned++; }
            }
            break;
        }
        case 'f': case 'g': case 'e': case 'F': case 'G': case 'E': {
            char *end = NULL;
            double v = strtod(s, &end);
            if (end == s) goto done;
            s = end;
            if (!suppress) { *va_arg(ap, float *) = (float)v; assigned++; }
            break;
        }
        case 's': {
            char *out = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            while (*s && !isspace((unsigned char)*s) && (width == 0 || n < width)) {
                if (out) out[n] = *s;
                n++; s++;
            }
            if (out) out[n] = '\0';
            if (n == 0) goto done;
            if (!suppress) assigned++;
            break;
        }
        case 'c': {
            int n = width ? width : 1;
            char *out = suppress ? NULL : va_arg(ap, char *);
            for (int i = 0; i < n && *s; i++) { if (out) out[i] = *s; s++; }
            if (!suppress) assigned++;
            break;
        }
        case '%':
            if (*s != '%') goto done;
            s++;
            break;
        default:
            goto done;
        }
    }
done:
    return assigned;
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

int fscanf(FILE *f, const char *fmt, ...) {
    char buf[1024];
    if (!fgets(buf, sizeof(buf), f)) return EOF;
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(buf, fmt, ap);
    va_end(ap);
    return r;
}

int scanf(const char *fmt, ...) {
    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) return EOF;
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(buf, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- temporary files ---- */

static int tmp_counter = 0;

FILE *tmpfile(void) {
    char name[64];
    snprintf(name, sizeof(name), "/tmp/tmpfile_%d", tmp_counter++);
    return fopen(name, "w+");
}

char *tmpnam(char *s) {
    static char buf[64];
    if (!s) s = buf;
    snprintf(s, 64, "/tmp/tmp%d", tmp_counter++);
    return s;
}

int mkstemp(char *tmpl) {
    if (!tmpl) return -1;
    size_t n = strlen(tmpl);
    if (n < 6) return -1;
    for (int i = 0; i < 6; i++) {
        if (tmpl[n - 6 + i] != 'X') return -1;
        tmpl[n - 6 + i] = 'a' + ((tmp_counter + i * 7) % 26);
    }
    tmp_counter++;
    return open(tmpl, O_CREAT | O_RDWR | O_EXCL, 0600);
}

int remove(const char *path) {
    return unlink(path);
}

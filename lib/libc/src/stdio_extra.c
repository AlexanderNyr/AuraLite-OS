/* libc/src/stdio_extra.c — sprintf / sscanf / scanf / tmpfile / stdio extensions (P10 / Q2) */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

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

/* ---- POSIX.1-2024 stdio extensions (Phase Q2) ---- */

/* Q2.1 — getdelim / getline */

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream) {
    if (!lineptr || !n || !stream) { errno = EINVAL; return -1; }
    if (!*lineptr || *n == 0) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) { errno = ENOMEM; return -1; }
    }
    ssize_t total = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if ((size_t)(total + 2) > *n) {
            size_t newn = *n * 2;
            char *p = realloc(*lineptr, newn);
            if (!p) { errno = ENOMEM; return -1; }
            *lineptr = p;
            *n = newn;
        }
        (*lineptr)[total++] = (char)c;
        if (c == delim) break;
    }
    if (total == 0) return -1;
    (*lineptr)[total] = '\0';
    return total;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    return getdelim(lineptr, n, '\n', stream);
}

/* Q2.2 — dprintf / vdprintf */

int vdprintf(int fd, const char *fmt, va_list ap) {
    char buf[4096];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) {
        int w = (n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1;
        write(fd, buf, (size_t)w);
    }
    return n;
}

int dprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vdprintf(fd, fmt, ap);
    va_end(ap);
    return r;
}

/* Q2.3 — asprintf / vasprintf */

int vasprintf(char **strp, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) { *strp = NULL; return -1; }
    *strp = malloc((size_t)n + 1);
    if (!*strp) return -1;
    vsnprintf(*strp, (size_t)n + 1, fmt, ap);
    return n;
}

int asprintf(char **strp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(strp, fmt, ap);
    va_end(ap);
    return r;
}

/* Q2.4 — fmemopen */

FILE *fmemopen(void *buf, size_t size, const char *mode) {
    if (!buf || size == 0 || !mode) { errno = EINVAL; return NULL; }
    int fds[2];
    if (pipe(fds) < 0) return NULL;
    if (mode[0] == 'r') {
        /* Write the buffer content into the pipe for reading. */
        write(fds[1], buf, size);
        close(fds[1]);
        return fdopen(fds[0], "r");
    }
    /* Write mode: keep the write end open. */
    close(fds[0]);
    return fdopen(fds[1], "w");
}

/* Q2.5 — open_memstream */

FILE *open_memstream(char **ptr, size_t *sizeloc) {
    if (!ptr || !sizeloc) { errno = EINVAL; return NULL; }
    int fds[2];
    if (pipe(fds) < 0) return NULL;
    *ptr = NULL;
    *sizeloc = 0;
    /* Write-only stream: close read end. */
    close(fds[0]);
    return fdopen(fds[1], "w");
}

/* Q2.6 — popen / pclose */

#define POPEN_MAX 8
static struct { FILE *f; pid_t pid; } _popen_tab[POPEN_MAX];

FILE *popen(const char *command, const char *type) {
    if (!command || !type || (type[0] != 'r' && type[0] != 'w'))
        { errno = EINVAL; return NULL; }
    int fds[2];
    if (pipe(fds) < 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return NULL; }
    if (pid == 0) {
        /* Child */
        if (type[0] == 'r') {
            dup2(fds[1], 1);  /* child stdout -> pipe write end */
        } else {
            dup2(fds[0], 0);  /* child stdin <- pipe read end */
        }
        close(fds[0]);
        close(fds[1]);
        /* Use /bin/sh -c to execute the command. */
        const char *argv[] = {"/bin/sh", "-c", command, NULL};
        execv("/bin/sh", (char *const *)argv);
        _exit(127);
    }
    /* Parent */
    FILE *f;
    if (type[0] == 'r') {
        close(fds[1]);       /* close write end, read from read end */
        f = fdopen(fds[0], "r");
    } else {
        close(fds[0]);       /* close read end, write to write end */
        f = fdopen(fds[1], "w");
    }
    if (f) {
        for (int i = 0; i < POPEN_MAX; i++) {
            if (!_popen_tab[i].f) {
                _popen_tab[i].f = f;
                _popen_tab[i].pid = pid;
                break;
            }
        }
    }
    return f;
}

int pclose(FILE *stream) {
    if (!stream) return -1;
    pid_t pid = 0;
    for (int i = 0; i < POPEN_MAX; i++) {
        if (_popen_tab[i].f == stream) {
            pid = _popen_tab[i].pid;
            _popen_tab[i].f = NULL;
            break;
        }
    }
    fclose(stream);
    if (!pid) return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

/* Q2.7 — Locking stubs (thread-safety deferred to Q6) */

void flockfile(FILE *f)     { (void)f; }
void funlockfile(FILE *f)   { (void)f; }
int  ftrylockfile(FILE *f)  { (void)f; return 0; }
int  getc_unlocked(FILE *f)      { return fgetc(f); }
int  putc_unlocked(int c, FILE *f) { return fputc(c, f); }
int  fgetc_unlocked(FILE *f)     { return fgetc(f); }

/* ---- file positioning: fseek / ftell / rewind / fgetpos / fsetpos ----
 *
 * Added for the DOOM port (DOOM_PLAN.md D1), but these are a plain C89 gap
 * rather than anything game-specific: a WAD file is read by seeking to a
 * directory at the end and then to each lump, which is exactly what any
 * random-access file format does.
 *
 * The subtlety is the buffer.  FILE here stages bytes in f->buf, so the
 * kernel's file offset is generally AHEAD of the position the program
 * believes it is at (whatever is still unread in the buffer). Two things
 * follow, and getting either wrong gives silent corruption rather than an
 * error:
 *
 *   - ftell() must SUBTRACT the unread remainder, or it reports the
 *     read-ahead position instead of the logical one;
 *   - fseek() must DISCARD the buffer, or the next read returns bytes from
 *     wherever the stream used to be.
 */

int fseek(FILE *f, long offset, int whence) {
    if (!f) { errno = EINVAL; return -1; }

    /* A pending write must reach the fd before the offset moves. */
    if (f->dir == 2 && f->bufpos > 0) {
        if (fflush(f) != 0) return -1;
    }

    /* SEEK_CUR is relative to the LOGICAL position, so it has to be
     * resolved against the buffered view before the buffer is dropped --
     * otherwise the seek lands wherever the read-ahead happened to stop. */
    if (whence == SEEK_CUR) {
        long here = ftell(f);
        if (here < 0) return -1;
        offset = here + offset;
        whence = SEEK_SET;
    }

    int64_t pos = lseek(f->fd, (int64_t)offset, whence);
    if (pos < 0) { f->flags |= FILE_ERR; return -1; }

    /* Drop buffered read state and any pushed-back character; both describe
     * the old position. */
    f->bufpos  = 0;
    f->bufcap  = 0;
    f->readpos = 0;
    f->ungot   = -1;
    f->dir     = 0;
    f->flags  &= ~FILE_EOF;    /* seeking clears EOF, per C89 */
    return 0;
}

long ftell(FILE *f) {
    if (!f) { errno = EINVAL; return -1; }

    int64_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) { f->flags |= FILE_ERR; return -1; }

    if (f->dir == 1) {
        /* Reading: the fd is ahead by whatever is still unread in the
         * buffer, plus a pushed-back character if there is one. */
        pos -= (int64_t)(f->bufcap - f->readpos);
        if (f->ungot >= 0) pos -= 1;
    } else if (f->dir == 2) {
        /* Writing: the fd is behind by whatever is still staged. */
        pos += (int64_t)f->bufpos;
    }
    return (long)pos;
}

void rewind(FILE *f) {
    if (!f) return;
    (void)fseek(f, 0, SEEK_SET);
    f->flags &= ~(FILE_EOF | FILE_ERR);   /* rewind also clears the error */
}

int fgetpos(FILE *f, fpos_t *pos) {
    if (!f || !pos) { errno = EINVAL; return -1; }
    long p = ftell(f);
    if (p < 0) return -1;
    *pos = (fpos_t)p;
    return 0;
}

int fsetpos(FILE *f, const fpos_t *pos) {
    if (!f || !pos) { errno = EINVAL; return -1; }
    return fseek(f, (long)*pos, SEEK_SET);
}

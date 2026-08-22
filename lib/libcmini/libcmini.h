/* lib/libcmini/libcmini.h — the shared bring-up libc (PARITY_PLAN.md
 * P8).
 *
 * One source of truth for what used to be three byte-identical
 * headers with different name suffixes (measured before the cut:
 * libc32.h/libcrv.h/libca64.h diffed clean modulo the suffix and
 * the include guard — 326 lines saying the same thing three times).
 * Each port header now declares its trap symbol, defines
 * AURA_SYSCALL, and includes THIS file; the suffixed helper names
 * the init programs use are two-line back-compat defines there.
 *
 * Scope is the FLOOR, not the ceiling (plan D-line, verbatim): the
 * eleven syscall wrappers over the D4 one-number table, errno set
 * from negative returns (raw returns preserved — callers check
 * rc < 0 exactly as before), the string family a freestanding
 * program cannot live without, and a fixed-buffer printf into
 * SYS_WRITE.  The full libc port (TLS, malloc-over-mmap, stdio
 * buffering, the POSIX surface) remains the NAMED NON-GOAL it has
 * been since RISCV_PLAN V8.
 */

#ifndef AURALITE_LIBCMINI_H
#define AURALITE_LIBCMINI_H

#ifndef AURA_SYSCALL
#error "port header must define AURA_SYSCALL before including libcmini.h"
#endif

/* ---- the one number table (D4) -------------------------------------- */

#define SYS_READ         0
#define SYS_WRITE        1
#define SYS_OPEN         2
#define SYS_CLOSE        3
#define SYS_STAT         4
#define SYS_LSEEK        8
#define SYS_BRK         12
#define SYS_GETPID      39
#define SYS_EXIT        60
#define SYS_READDIR     78
#define SYS_SPAWN       81
#define SYS_SCHED_YIELD 158

#include "lib/abi/fsabi.h"

/* ---- errno: set from negative returns, returns stay raw ------------- */

#define EPERM    1
#define ENOENT   2
#define EIO      5
#define EBADF    9
#define EFAULT  14
#define ENODEV  19
#define ENOTDIR 20
#define EINVAL  22
#define EMFILE  24

static int aura_errno;

static inline long __aura_ret(long rc)
{
    if (rc < 0)
        aura_errno = (int)-rc;
    return rc;
}

/* ---- the eleven wrappers --------------------------------------------- */

static inline long read(int fd, void *buf, unsigned long len)
{
    return __aura_ret(AURA_SYSCALL(SYS_READ, fd, (long)buf, (long)len, 0, 0));
}

static inline long write(int fd, const void *buf, unsigned long len)
{
    return __aura_ret(AURA_SYSCALL(SYS_WRITE, fd, (long)buf, (long)len, 0, 0));
}

static inline long open(const char *path, int flags)
{
    return __aura_ret(AURA_SYSCALL(SYS_OPEN, (long)path, flags, 0, 0, 0));
}

static inline long close(int fd)
{
    return __aura_ret(AURA_SYSCALL(SYS_CLOSE, fd, 0, 0, 0, 0));
}

static inline long lseek(int fd, long off, int whence)
{
    return __aura_ret(AURA_SYSCALL(SYS_LSEEK, fd, off, whence, 0, 0));
}

static inline long stat(const char *path, struct aura_stat *st)
{
    return __aura_ret(AURA_SYSCALL(SYS_STAT, (long)path, (long)st, 0, 0, 0));
}

/* One entry per call: readdir(fd, index, out).  1 = entry filled,
 * 0 = end of directory, negative = error. */
static inline long readdir(int fd, long index, struct aura_dirent *de)
{
    return __aura_ret(AURA_SYSCALL(SYS_READDIR, fd, index, (long)de, 0, 0));
}

/* Non-standard, same number at all widths: run an initrd program to
 * completion, return its exit code. */
static inline long spawn(const char *path)
{
    return __aura_ret(AURA_SYSCALL(SYS_SPAWN, (long)path, 0, 0, 0, 0));
}

static inline long getpid(void)
{
    return AURA_SYSCALL(SYS_GETPID, 0, 0, 0, 0, 0);
}

static inline void exit(int code)
{
    AURA_SYSCALL(SYS_EXIT, code, 0, 0, 0, 0);
    __builtin_unreachable();
}

static inline long sched_yield(void)
{
    return AURA_SYSCALL(SYS_SCHED_YIELD, 0, 0, 0, 0, 0);
}

/* ---- the string family ----------------------------------------------- */

static inline unsigned long aura_strlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static inline int aura_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static inline void *aura_memcpy(void *dst, const void *src,
                                unsigned long n)
{
    char *d = dst;
    const char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

static inline void *aura_memset(void *dst, int c, unsigned long n)
{
    char *d = dst;
    while (n--) *d++ = (char)c;
    return dst;
}

static inline void aura_puts(const char *s)
{
    write(1, s, aura_strlen(s));
}

/* ---- the fixed-buffer printf ------------------------------------------ */
/* %s %c %d %u %x %% — the receipt-printing subset, 256 bytes, one
 * write.  Anything fancier belongs to the full libc, which is the
 * named non-goal.  Output beyond the buffer is truncated, honestly. */

static inline void aura_printf(const char *fmt, ...)
{
    char out[256];
    unsigned long o = 0;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (const char *p = fmt; *p && o < sizeof(out) - 1; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        p++;
        if (*p == '%') { out[o++] = '%'; continue; }
        if (*p == 'c') { out[o++] = (char)__builtin_va_arg(ap, int); continue; }
        if (*p == 's') {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && o < sizeof(out) - 1) out[o++] = *s++;
            continue;
        }
        if (*p == 'd' || *p == 'u' || *p == 'x') {
            long v = (*p == 'd') ? (long)__builtin_va_arg(ap, int)
                                 : (long)(unsigned)__builtin_va_arg(ap, unsigned);
            unsigned long u = (unsigned long)v;
            if (*p == 'd' && v < 0 && o < sizeof(out) - 1) {
                out[o++] = '-';
                u = (unsigned long)-v;
            }
            char tmp[24];
            int t = 0;
            unsigned base = (*p == 'x') ? 16u : 10u;
            do {
                unsigned d = (unsigned)(u % base);
                tmp[t++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                u /= base;
            } while (u && t < (int)sizeof(tmp));
            while (t-- && o < sizeof(out) - 1) out[o++] = tmp[t];
            continue;
        }
        out[o++] = '?';             /* unknown verb, visibly */
    }
    __builtin_va_end(ap);
    write(1, out, o);
}

/* ---- R6: the heap ------------------------------------------------------
 * brk-backed malloc/free with a first-fit free list (K&R shape).
 * The FLOOR contract: no thread safety (single-threaded ports), no
 * trimming back to the kernel, 16-byte aligned blocks. */

static inline long brk_(unsigned long end)
{
    return AURA_SYSCALL(SYS_BRK, (long)end, 0, 0, 0, 0);
}

struct aura_blk {
    unsigned long   size;          /* payload bytes */
    struct aura_blk *next;         /* next FREE block (free list) */
};

static struct aura_blk *aura_freelist;
static unsigned long aura_brk_cur;

static inline void *malloc(unsigned long n)
{
    if (n == 0)
        n = 1;
    n = (n + 15ul) & ~15ul;
    struct aura_blk **pp = &aura_freelist;
    for (struct aura_blk *b = aura_freelist; b; pp = &b->next, b = b->next) {
        if (b->size >= n) {
            *pp = b->next;
            return (char *)b + sizeof(struct aura_blk);
        }
    }
    if (aura_brk_cur == 0)
        aura_brk_cur = (unsigned long)brk_(0);
    unsigned long need = sizeof(struct aura_blk) + n;
    long r = brk_(aura_brk_cur + need);
    if (r < 0 || (unsigned long)r < aura_brk_cur + need)
        return 0;                   /* honest OOM */
    struct aura_blk *b = (struct aura_blk *)aura_brk_cur;
    aura_brk_cur = (unsigned long)r;
    b->size = n;
    b->next = 0;
    return (char *)b + sizeof(struct aura_blk);
}

static inline void free(void *p)
{
    if (!p)
        return;
    struct aura_blk *b =
        (struct aura_blk *)((char *)p - sizeof(struct aura_blk));
    b->next = aura_freelist;
    aura_freelist = b;
}

/* ---- R6: stdio-lite -----------------------------------------------------
 * FILE is an fd plus an error flag; four static slots per program
 * (single-file bring-up binaries).  fopen modes: "r" opens, "w"
 * opens-or-CREATES (the ports' O_CREAT-lite bit) and truncates by
 * ignoring old length -- honest note: no O_TRUNC exists yet, so a
 * shorter rewrite leaves a longer file's tail; fsio sizes its data
 * accordingly.  fprintf carries the aura_printf verb subset. */

typedef struct {
    int fd;
    int used;
    int err;
} FILE;

static FILE aura_files[4];

static inline FILE *fopen(const char *path, const char *mode)
{
    int flags = (mode && mode[0] == 'w') ? 1 : 0;
    long fd = open(path, flags);
    if (fd < 0)
        return 0;
    for (unsigned i = 0; i < sizeof(aura_files)/sizeof(aura_files[0]); i++) {
        if (!aura_files[i].used) {
            aura_files[i].fd = (int)fd;
            aura_files[i].used = 1;
            aura_files[i].err = 0;
            return &aura_files[i];
        }
    }
    close((int)fd);
    return 0;
}

static inline int fclose(FILE *f)
{
    if (!f || !f->used)
        return -1;
    f->used = 0;
    return (int)close(f->fd);
}

static inline unsigned long fwrite(const void *p, unsigned long sz,
                                   unsigned long n, FILE *f)
{
    if (!f || !f->used)
        return 0;
    unsigned long total = sz * n, done = 0;
    const char *s = p;
    while (done < total) {
        long w = write(f->fd, s + done, total - done);
        if (w <= 0) { f->err = 1; break; }
        done += (unsigned long)w;
    }
    return sz ? done / sz : 0;
}

static inline unsigned long fread(void *p, unsigned long sz,
                                  unsigned long n, FILE *f)
{
    if (!f || !f->used)
        return 0;
    unsigned long total = sz * n, done = 0;
    char *d = p;
    while (done < total) {
        long r = read(f->fd, d + done, total - done);
        if (r <= 0) break;
        done += (unsigned long)r;
    }
    return sz ? done / sz : 0;
}

static inline char *fgets(char *dst, int cap, FILE *f)
{
    if (!f || !f->used || cap < 2)
        return 0;
    int i = 0;
    while (i < cap - 1) {
        char c;
        long r = read(f->fd, &c, 1);
        if (r <= 0) break;
        dst[i++] = c;
        if (c == '\n') break;
    }
    if (i == 0)
        return 0;
    dst[i] = 0;
    return dst;
}

#endif /* AURALITE_LIBCMINI_H */

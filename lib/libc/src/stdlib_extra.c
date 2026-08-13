/* libc/src/stdlib_extra.c — qsort, bsearch, abort, atexit, stdlib extensions, sysconf (P10 / Q4) */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    /* Простая реализация пузырьковой сортировки (достаточно для тестов) */
    char *b = base;
    for (size_t i = 0; i < nmemb; i++) {
        for (size_t j = 0; j < nmemb - 1; j++) {
            if (compar(b + j * size, b + (j + 1) * size) > 0) {
                char tmp[256];
                if (size > sizeof(tmp)) return; /* safety */
                memcpy(tmp, b + j * size, size);
                memcpy(b + j * size, b + (j + 1) * size, size);
                memcpy(b + (j + 1) * size, tmp, size);
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *)) {
    const char *b = base;
    size_t low = 0, high = nmemb;
    while (low < high) {
        size_t mid = (low + high) / 2;
        int cmp = compar(key, b + mid * size);
        if (cmp == 0) return (void *)(b + mid * size);
        if (cmp < 0) high = mid;
        else low = mid + 1;
    }
    return NULL;
}

static void (*atexit_funcs[32])(void);
static int atexit_count = 0;

int atexit(void (*func)(void)) {
    if (atexit_count >= 32) return -1;
    atexit_funcs[atexit_count++] = func;
    return 0;
}

void __run_atexit(void) {
    for (int i = atexit_count - 1; i >= 0; i--) {
        atexit_funcs[i]();
    }
}

/* ---- POSIX.1-2024 stdlib extensions (Phase Q4) ---- */

void *reallocarray(void *ptr, size_t nmemb, size_t size) {
    if (nmemb && size > (size_t)(-1) / nmemb) { errno = ENOMEM; return NULL; }
    return realloc(ptr, nmemb * size);
}

char *realpath(const char *path, char *resolved) {
    static char buf[4096];
    char *out = resolved ? resolved : buf;
    if (!path) { errno = EINVAL; return NULL; }
    /* Prepend cwd if relative. */
    if (path[0] != '/') {
        if (!getcwd(out, 4096)) return NULL;
        size_t l = strlen(out);
        out[l] = '/';
        strncpy(out + l + 1, path, 4096 - l - 2);
    } else {
        strncpy(out, path, 4095);
        out[4095] = '\0';
    }
    /* Collapse //, /./, /.. */
    char tmp[4096];
    int ti = 0;
    for (int i = 0; out[i]; ) {
        if (out[i] == '/' && out[i+1] == '/') { i++; continue; }
        if (out[i] == '/' && out[i+1] == '.' && (!out[i+2] || out[i+2] == '/'))
            { i += 2; continue; }
        if (out[i] == '/' && out[i+1] == '.' && out[i+2] == '.' &&
            (!out[i+3] || out[i+3] == '/')) {
            i += 3;
            while (ti > 0 && tmp[ti-1] != '/') ti--;
            if (ti > 1) ti--;
            continue;
        }
        tmp[ti++] = out[i++];
    }
    if (ti == 0) tmp[ti++] = '/';
    tmp[ti] = '\0';
    strncpy(out, tmp, 4095);
    return out;
}

/* ---- mkdtemp / mkostemp / mkstemps ---- */

static const char _rchars[] = "abcdefghijklmnopqrstuvwxyz0123456789";

/* Forward declaration of getentropy for mkdtemp/mkostemp (defined in libc.c) */
extern int getentropy(void *buffer, size_t length);

char *mkdtemp(char *tmpl) {
    size_t len = strlen(tmpl);
    if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0)
        { errno = EINVAL; return NULL; }
    for (int tries = 0; tries < 100; tries++) {
        uint8_t rnd[6]; getentropy(rnd, 6);
        for (int i = 0; i < 6; i++) tmpl[len-6+i] = _rchars[rnd[i] % 36];
        if (mkdir(tmpl, 0700) == 0) return tmpl;
        if (errno != EEXIST) return NULL;
    }
    errno = EEXIST; return NULL;
}

int mkostemp(char *tmpl, int flags) {
    size_t len = strlen(tmpl);
    if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0)
        { errno = EINVAL; return -1; }
    for (int tries = 0; tries < 100; tries++) {
        uint8_t rnd[6]; getentropy(rnd, 6);
        for (int i = 0; i < 6; i++) tmpl[len-6+i] = _rchars[rnd[i] % 36];
        int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL | flags, 0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST; return -1;
}

int mkstemps(char *tmpl, int suffixlen) {
    size_t len = strlen(tmpl);
    if ((size_t)suffixlen + 6 > len) { errno = EINVAL; return -1; }
    char *x = tmpl + len - suffixlen - 6;
    for (int tries = 0; tries < 100; tries++) {
        uint8_t rnd[6]; getentropy(rnd, 6);
        for (int i = 0; i < 6; i++) x[i] = _rchars[rnd[i] % 36];
        int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST; return -1;
}

/* ---- sysconf / confstr / pathconf / fpathconv ---- */

long sysconf(int name) {
    switch (name) {
    case 30:   /* _SC_PAGE_SIZE */
        return 4096;
    case 85:   /* _SC_PHYS_PAGES */
        return 65536;
    case 84:   /* _SC_NPROCESSORS_ONLN */
        return 1;
    case 83:   /* _SC_NPROCESSORS_CONF */
        return 4;
    case  4:   /* _SC_OPEN_MAX */
        return 64;
    case  0:   /* _SC_ARG_MAX */
        return 131072;
    case 71:   /* _SC_LOGIN_NAME_MAX */
        return 256;
    case 180:  /* _SC_HOST_NAME_MAX */
        return 255;
    case 70:   /* _SC_GETPW_R_SIZE_MAX */
        return 1024;
    case 69:   /* _SC_GETGR_R_SIZE_MAX */
        return 1024;
    case 46:   /* _SC_PTHREAD_KEYS_MAX */
        return 64;
    case 75:   /* _SC_THREAD_STACK_MIN */
        return 16384;
    case 149:  /* _SC_MONOTONIC_CLOCK */
        return 200809L;
    case  2:   /* _SC_CLK_TCK */
        return 100;
    case  3:   /* _SC_NGROUPS_MAX */
        return 32;
    case  5:   /* _SC_STREAM_MAX */
        return 8;
    case  6:   /* _SC_TZNAME_MAX */
        return 6;
    case  7:   /* _SC_JOB_CONTROL */
        return 1;
    case  8:   /* _SC_SAVED_IDS */
        return 1;
    case  9:   /* _SC_REALTIME_SIGNALS */
        return 1;
    case 10:   /* _SC_PRIORITY_SCHEDULING */
        return -1;
    case 11:   /* _SC_TIMERS */
        return 1;
    case 12:   /* _SC_ASYNCHRONOUS_IO */
        return -1;
    case 21:   /* _SC_SEMAPHORES */
        return 1;
    case 22:   /* _SC_SHARED_MEMORY_OBJECTS */
        return 1;
    case 67:   /* _SC_THREADS */
        return 1;
    /* POSIX.1-2024 version reporting */
    case 89:   /* _SC_POSIX_VERSION */
        return 202405L;
    case 90:   /* _SC_XOPEN_VERSION */
        return 800;
    default:
        errno = EINVAL;
        return -1;
    }
}

size_t confstr(int name, char *buf, size_t len) {
    const char *val = "";
    switch (name) {
    case 0:    /* _CS_PATH */
        val = "/bin";
        break;
    default:
        errno = EINVAL;
        return 0;
    }
    size_t vlen = strlen(val) + 1;
    if (buf && len > 0) { strncpy(buf, val, len - 1); buf[len-1] = '\0'; }
    return vlen;
}

long pathconf(const char *path, int name) {
    (void)path;
    switch (name) {
    case 4:    /* _PC_PATH_MAX */
        return 4096;
    case 3:    /* _PC_NAME_MAX */
        return 255;
    case 5:    /* _PC_PIPE_BUF */
        return 4096;
    default:
        errno = EINVAL;
        return -1;
    }
}

long fpathconf(int fd, int name) {
    (void)fd;
    return pathconf(NULL, name);
}
/* abs / labs / llabs — C89/C99 integer absolute value.
 *
 * Missing until now.  DOOM uses abs() heavily in the renderer, but this is
 * a plain standard-library gap.
 *
 * INT_MIN has no positive counterpart in two's complement, so -INT_MIN
 * overflows and is undefined behaviour.  The standard leaves abs(INT_MIN)
 * undefined for exactly that reason; returning the argument unchanged is
 * what real implementations do and is at least deterministic, rather than
 * inviting the optimiser to conclude the branch is unreachable.
 */
int abs(int v) {
    if (v == (-2147483647 - 1)) return v;   /* INT_MIN: see above */
    return v < 0 ? -v : v;
}

long labs(long v) {
    if (v == (-9223372036854775807L - 1L)) return v;
    return v < 0 ? -v : v;
}

long long llabs(long long v) {
    if (v == (-9223372036854775807LL - 1LL)) return v;
    return v < 0 ? -v : v;
}

/* system() — command execution.
 *
 * AuraLite has no /bin/sh: the shell is built into init and is not an
 * executable a process can spawn.  There is therefore nothing to hand a
 * command string to.
 *
 * The C standard defines exactly this case: system(NULL) returns zero when
 * no command processor is available, and that is the honest answer here.
 * For a non-NULL command it returns -1 with errno set, rather than
 * pretending the command ran and succeeded -- a caller that checks the
 * result learns the truth, and one that does not is no worse off than with
 * a silent no-op.
 *
 * This exists because portable code tests for a helper by running it (DOOM
 * probes for `zenity --help` before using it for error dialogs). Such code
 * needs system() to LINK and to say "no", which it now does, instead of
 * failing to build.
 */
int system(const char *command) {
    if (command == NULL) return 0;   /* no command processor available */
    errno = ENOSYS;
    return -1;
}

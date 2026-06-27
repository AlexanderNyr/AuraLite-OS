/*
 * libc.c — C library implementations for AuraLite OS user programs.
 *
 * Compiled with the host compiler and linked into user binaries. Provides
 * syscall wrappers, string functions, stdlib, and printf.
 */

#include <stdint.h>
#include <stdarg.h>
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "errno.h"
#include "ctype.h"
#include "math.h"
#include "fcntl.h"
#include "sys/uio.h"
#include "signal.h"

/* ---- errno storage ----
 *
 * Single global for now (the system is effectively single-threaded at the
 * libc level).  Phase P9 replaces __errno_location() with a TLS-backed cell;
 * no caller changes because errno is a macro over this accessor. */
static int __errno_storage = 0;

int *__errno_location(void) {
    return &__errno_storage;
}

/* ---- in-band syscall return decoding ----
 *
 * The kernel returns results in RAX using the Linux negative-errno convention:
 * the unsigned range [(unsigned long)-MAX_ERRNO, (unsigned long)-1] is reserved
 * for errors.  Decode that band into errno + a -1 return; everything else is a
 * successful value (including legitimately huge values such as large file
 * offsets or mmap addresses).  Comparing as unsigned avoids the classic
 * `ret < 0` bug. */
#define SYSCALL_MAX_ERRNO 4095UL

static long syscall_ret(int64_t raw) {
    if ((unsigned long)raw >= (unsigned long)-SYSCALL_MAX_ERRNO) {
        errno = (int)(-raw);
        return -1;
    }
    return (long)raw;
}

/* ---- Stack protector runtime ---- */

uintptr_t __stack_chk_guard = 0x6B1F2D4CA53E9071ULL;

__attribute__((noreturn)) void __stack_chk_fail(void) {
    const char msg[] = "stack corruption detected\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(127);
    for (;;) { }
}

/* ---- Syscall wrappers ---- */

ssize_t write(int fd, const void *buf, size_t count) {
    return (ssize_t)syscall_ret(syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)buf,
                                        (uint64_t)count, 0, 0, 0));
}

ssize_t read(int fd, void *buf, size_t count) {
    return (ssize_t)syscall_ret(syscall(SYS_READ, (uint64_t)fd, (uint64_t)buf,
                                        (uint64_t)count, 0, 0, 0));
}

int open(const char *path, int flags, ...) {
    /* mode is only consulted when O_CREAT is set (POSIX). */
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return (int)syscall_ret(syscall(SYS_OPEN, (uint64_t)path, (uint64_t)flags,
                                    (uint64_t)mode, 0, 0, 0));
}

int creat(const char *path, int mode) {
    return open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

int close(int fd) {
    return (int)syscall_ret(syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0));
}

int64_t lseek(int fd, int64_t offset, int whence) {
    return (int64_t)syscall_ret(syscall(SYS_LSEEK, (uint64_t)fd,
                                        (uint64_t)offset, (uint64_t)whence,
                                        0, 0, 0));
}

ssize_t pread(int fd, void *buf, size_t count, int64_t offset) {
    return (ssize_t)syscall_ret(syscall(SYS_PREAD64, (uint64_t)fd,
                                        (uint64_t)buf, (uint64_t)count,
                                        (uint64_t)offset, 0, 0));
}

ssize_t pwrite(int fd, const void *buf, size_t count, int64_t offset) {
    return (ssize_t)syscall_ret(syscall(SYS_PWRITE64, (uint64_t)fd,
                                        (uint64_t)buf, (uint64_t)count,
                                        (uint64_t)offset, 0, 0));
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    return (ssize_t)syscall_ret(syscall(SYS_READV, (uint64_t)fd,
                                        (uint64_t)iov, (uint64_t)iovcnt,
                                        0, 0, 0));
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    return (ssize_t)syscall_ret(syscall(SYS_WRITEV, (uint64_t)fd,
                                        (uint64_t)iov, (uint64_t)iovcnt,
                                        0, 0, 0));
}

/* ---- signals ---- */

/* The kernel pushes this trampoline as the handler's return address; on
 * handler `ret` it invokes SYS_SIGRETURN.  Defined in libc/crt/sigreturn.asm. */
extern void __sigreturn(void);

int sigemptyset(sigset_t *set) { if (set) *set = 0; return 0; }
int sigfillset(sigset_t *set)  { if (set) *set = 0xFFFFFFFFu; return 0; }
int sigaddset(sigset_t *set, int signo) {
    if (!set || signo < 1 || signo >= NSIG) { errno = EINVAL; return -1; }
    *set |= (1u << (signo - 1)); return 0;
}
int sigdelset(sigset_t *set, int signo) {
    if (!set || signo < 1 || signo >= NSIG) { errno = EINVAL; return -1; }
    *set &= ~(1u << (signo - 1)); return 0;
}
int sigismember(const sigset_t *set, int signo) {
    if (!set || signo < 1 || signo >= NSIG) { errno = EINVAL; return -1; }
    return (*set & (1u << (signo - 1))) ? 1 : 0;
}

int sigaction(int signo, const struct sigaction *act, struct sigaction *old) {
    struct sigaction kact;
    const struct sigaction *pass = act;
    if (act) {
        kact = *act;
        kact.sa_restorer = __sigreturn;   /* libc supplies the trampoline */
        pass = &kact;
    }
    return (int)syscall_ret(syscall(SYS_SIGACTION, (uint64_t)signo,
                                    (uint64_t)pass, (uint64_t)old, 0, 0, 0));
}

void (*signal(int signo, void (*handler)(int)))(int) {
    struct sigaction act, old;
    act.sa_handler = handler;
    act.sa_mask = 0;
    act.sa_flags = SA_RESTART;            /* BSD/glibc signal() semantics */
    act.sa_restorer = __sigreturn;
    if (sigaction(signo, &act, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}

int kill(int64_t pid, int signo) {
    return (int)syscall_ret(syscall(SYS_KILL, (uint64_t)pid, (uint64_t)signo,
                                    0, 0, 0, 0));
}

int raise(int signo) {
    return kill((int64_t)getpid(), signo);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    return (int)syscall_ret(syscall(SYS_SIGPROCMASK, (uint64_t)how,
                                    (uint64_t)set, (uint64_t)old, 0, 0, 0));
}

int sigpending(sigset_t *set) {
    return (int)syscall_ret(syscall(SYS_SIGPENDING, (uint64_t)set, 0, 0, 0, 0, 0));
}

unsigned alarm(unsigned seconds) {
    /* alarm() does not report errors; the syscall returns the remaining time. */
    return (unsigned)syscall(SYS_ALARM, (uint64_t)seconds, 0, 0, 0, 0, 0);
}

int pause(void) {
    /* pause() always returns -1 with errno=EINTR once a signal is delivered. */
    return (int)syscall_ret(syscall(SYS_PAUSE, 0, 0, 0, 0, 0, 0));
}

int sigsuspend(const sigset_t *mask) {
    return (int)syscall_ret(syscall(SYS_SIGSUSPEND, (uint64_t)mask, 0, 0, 0, 0, 0));
}

void _exit(int code) {
    syscall(SYS_EXIT, (uint64_t)code, 0, 0, 0, 0, 0);
}

pid_t getpid(void) {
    return (pid_t)syscall(SYS_GETPID, 0, 0, 0, 0, 0, 0);
}

void listdir(const char *path) {
    syscall(80, (uint64_t)path, 0, 0, 0, 0, 0); // 80 = SYS_LISTDIR
}

int readdir(const char *path, void *out, int max) {
    return (int)syscall_ret(syscall(80, (uint64_t)path, (uint64_t)out, max,
                                    0, 0, 0));
}

uint32_t dns_resolve(const char *hostname) {
    /* Returns a raw IPv4 address (0 on failure); not an in-band errno. */
    return (uint32_t)syscall(SYS_DNS, (uint64_t)hostname, 0, 0, 0, 0, 0);
}

int net_connect(uint32_t ip, uint16_t port) {
    return (int)syscall_ret(syscall(SYS_NET_CONNECT, ip, port, 0, 0, 0, 0));
}

int net_send(const void *data, uint32_t len) {
    return (int)syscall_ret(syscall(SYS_NET_SEND, (uint64_t)data, len,
                                    0, 0, 0, 0));
}

int net_recv(void *buf, uint32_t bufsize) {
    return (int)syscall_ret(syscall(SYS_NET_RECV, (uint64_t)buf, bufsize,
                                    0, 0, 0, 0));
}

int net_close(void) {
    return (int)syscall_ret(syscall(SYS_NET_CLOSE, 0, 0, 0, 0, 0, 0));
}

int net_ping(uint32_t ip) {
    return (int)syscall_ret(syscall(SYS_NET_PING, ip, 0, 0, 0, 0, 0));
}

int socket(int domain, int type, int protocol) {
    return (int)syscall_ret(syscall(SYS_SOCKET, (uint64_t)domain, (uint64_t)type,
                                    (uint64_t)protocol, 0, 0, 0));
}

int connect(int sock, uint32_t ip, uint16_t port) {
    return (int)syscall_ret(syscall(SYS_SOCKET_CONNECT, (uint64_t)sock, ip,
                                    port, 0, 0, 0));
}

int send(int sock, const void *data, uint32_t len) {
    return (int)syscall_ret(syscall(SYS_SOCKET_SEND, (uint64_t)sock,
                                    (uint64_t)data, len, 0, 0, 0));
}

int recv(int sock, void *buf, uint32_t bufsize) {
    return (int)syscall_ret(syscall(SYS_SOCKET_RECV, (uint64_t)sock,
                                    (uint64_t)buf, bufsize, 0, 0, 0));
}

int closesocket(int sock) {
    return (int)syscall_ret(syscall(SYS_SOCKET_CLOSE, (uint64_t)sock,
                                    0, 0, 0, 0, 0));
}

int mkdir(const char *path) {
    return (int)syscall_ret(syscall(SYS_MKDIR, (uint64_t)path, 0, 0, 0, 0, 0));
}
int rmdir(const char *path) {
    return (int)syscall_ret(syscall(SYS_RMDIR, (uint64_t)path, 0, 0, 0, 0, 0));
}
int unlink(const char *path) {
    return (int)syscall_ret(syscall(SYS_UNLINK, (uint64_t)path, 0, 0, 0, 0, 0));
}
int rename(const char *from, const char *to) {
    return (int)syscall_ret(syscall(SYS_RENAME, (uint64_t)from, (uint64_t)to,
                                    0, 0, 0, 0));
}
int truncate(const char *path, uint64_t new_size) {
    return (int)syscall_ret(syscall(SYS_TRUNCATE, (uint64_t)path, new_size,
                                    0, 0, 0, 0));
}
int stat(const char *path, struct stat *out) {
    return (int)syscall_ret(syscall(SYS_STAT, (uint64_t)path, (uint64_t)out,
                                    0, 0, 0, 0));
}

pid_t fork(void) {
    return (pid_t)syscall_ret(syscall(SYS_FORK, 0, 0, 0, 0, 0, 0));
}

int execve(const char *path) {
    return (int)syscall_ret(syscall(SYS_EXECVE, (uint64_t)path, 0, 0, 0, 0, 0));
}

pid_t wait(int *status) {
    return (pid_t)syscall_ret(syscall(SYS_WAIT4, (uint64_t)status,
                                      0, 0, 0, 0, 0));
}

pid_t spawn(const char *path) {
    return (pid_t)syscall_ret(syscall(SYS_SPAWN, (uint64_t)path, 0, 0, 0, 0, 0));
}

pid_t waitpid(pid_t pid, int *status) {
    int64_t s = 0;
    long r = syscall_ret(syscall(SYS_WAIT4, (uint64_t)pid, (uint64_t)&s,
                                 0, 0, 0, 0));
    if (status) *status = (int)s;
    return (pid_t)r;
}

int dup(int oldfd) {
    return (int)syscall_ret(syscall(SYS_DUP, (uint64_t)oldfd, 0, 0, 0, 0, 0));
}
int dup2(int oldfd, int newfd) {
    return (int)syscall_ret(syscall(SYS_DUP2, (uint64_t)oldfd, (uint64_t)newfd,
                                    0, 0, 0, 0));
}
int pipe(int fds[2]) {
    return (int)syscall_ret(syscall(SYS_PIPE, (uint64_t)fds, 0, 0, 0, 0, 0));
}
int pipe2(int fds[2], int flags) {
    return (int)syscall_ret(syscall(SYS_PIPE2, (uint64_t)fds, (uint64_t)flags,
                                    0, 0, 0, 0));
}
int fcntl(int fd, int cmd, ...) {
    /* The third argument is an int for every command we implement (F_SETFD,
     * F_SETFL, F_DUPFD*); F_GETFD/F_GETFL ignore it.  Always fetch it: passing
     * an unused variadic int is harmless. */
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);
    return (int)syscall_ret(syscall(SYS_FCNTL, (uint64_t)fd, (uint64_t)cmd,
                                    (uint64_t)arg, 0, 0, 0));
}

void* mmap(void *addr, size_t length, int prot, int flags, int fd, uint64_t offset) {
    /* mmap reports failure via MAP_FAILED (not -1) and sets errno from the
     * in-band error band; a successful mapping address is never in that band. */
    int64_t raw = syscall(SYS_MMAP, (uint64_t)addr, (uint64_t)length,
                          (uint64_t)prot, (uint64_t)flags,
                          (uint64_t)fd, offset);
    if ((unsigned long)raw >= (unsigned long)-SYSCALL_MAX_ERRNO) {
        errno = (int)(-raw);
        return MAP_FAILED;
    }
    return (void *)raw;
}

int munmap(void *addr, size_t length) {
    return (int)syscall_ret(syscall(SYS_MUNMAP, (uint64_t)addr, (uint64_t)length,
                                    0, 0, 0, 0));
}

void* sbrk(intptr_t increment) {
    /* SYS_BRK returns the (new) break, or the unchanged break on failure; it
     * does not use the in-band errno band, so decode it by hand. */
    int64_t cur_raw = syscall(12, 0, 0, 0, 0, 0, 0); // 12 = SYS_BRK
    if ((unsigned long)cur_raw >= (unsigned long)-SYSCALL_MAX_ERRNO) {
        errno = ENOMEM;
        return (void*)-1;
    }
    uint64_t cur_brk = (uint64_t)cur_raw;
    if (increment == 0) return (void*)cur_brk;

    uint64_t new_brk = (uint64_t)syscall(12, cur_brk + increment, 0, 0, 0, 0, 0);
    if (new_brk == cur_brk) { errno = ENOMEM; return (void*)-1; }

    return (void*)cur_brk;
}

/* ---- strerror / perror ----
 *
 * POSIX.1-2017 strerror(): maps an errno value to a message string.  Need not
 * be thread-safe; for an unknown code it formats into a static buffer that a
 * subsequent strerror() call may overwrite, and sets errno to EINVAL (a
 * permitted "may fail" behaviour). */

/* Indexed by errno value.  Aliases (EWOULDBLOCK, EDEADLOCK, ENOTSUP) are NOT
 * listed separately: they would be duplicate designated initializers for an
 * index already covered by their canonical name. */
static const char *const errmsgs[] = {
    [0]               = "Success",
    [EPERM]           = "Operation not permitted",
    [ENOENT]          = "No such file or directory",
    [ESRCH]           = "No such process",
    [EINTR]           = "Interrupted system call",
    [EIO]             = "Input/output error",
    [ENXIO]           = "No such device or address",
    [E2BIG]           = "Argument list too long",
    [ENOEXEC]         = "Exec format error",
    [EBADF]           = "Bad file descriptor",
    [ECHILD]          = "No child processes",
    [EAGAIN]          = "Resource temporarily unavailable",
    [ENOMEM]          = "Cannot allocate memory",
    [EACCES]          = "Permission denied",
    [EFAULT]          = "Bad address",
    [ENOTBLK]         = "Block device required",
    [EBUSY]           = "Device or resource busy",
    [EEXIST]          = "File exists",
    [EXDEV]           = "Invalid cross-device link",
    [ENODEV]          = "No such device",
    [ENOTDIR]         = "Not a directory",
    [EISDIR]          = "Is a directory",
    [EINVAL]          = "Invalid argument",
    [ENFILE]          = "Too many open files in system",
    [EMFILE]          = "Too many open files",
    [ENOTTY]          = "Inappropriate ioctl for device",
    [ETXTBSY]         = "Text file busy",
    [EFBIG]           = "File too large",
    [ENOSPC]          = "No space left on device",
    [ESPIPE]          = "Illegal seek",
    [EROFS]           = "Read-only file system",
    [EMLINK]          = "Too many links",
    [EPIPE]           = "Broken pipe",
    [EDOM]            = "Numerical argument out of domain",
    [ERANGE]          = "Numerical result out of range",
    [EDEADLK]         = "Resource deadlock avoided",
    [ENAMETOOLONG]    = "File name too long",
    [ENOLCK]          = "No locks available",
    [ENOSYS]          = "Function not implemented",
    [ENOTEMPTY]       = "Directory not empty",
    [ELOOP]           = "Too many levels of symbolic links",
    [ENOMSG]          = "No message of desired type",
    [EIDRM]           = "Identifier removed",
    [EOVERFLOW]       = "Value too large for defined data type",
    [EILSEQ]          = "Invalid or incomplete multibyte or wide character",
    [EOPNOTSUPP]      = "Operation not supported",
    [EADDRINUSE]      = "Address already in use",
    [EADDRNOTAVAIL]   = "Cannot assign requested address",
    [ENETDOWN]        = "Network is down",
    [ENETUNREACH]     = "Network is unreachable",
    [ECONNRESET]      = "Connection reset by peer",
    [ENOTCONN]        = "Transport endpoint is not connected",
    [ETIMEDOUT]       = "Connection timed out",
    [ECONNREFUSED]    = "Connection refused",
    [EHOSTUNREACH]    = "No route to host",
    [EALREADY]        = "Operation already in progress",
    [EINPROGRESS]     = "Operation now in progress",
    [ECANCELED]       = "Operation canceled",
};

#define ERRMSGS_COUNT ((int)(sizeof(errmsgs) / sizeof(errmsgs[0])))

/* Format a base-10 int into dst (caller guarantees room: <= 12 chars). */
static char *fmt_int(char *dst, int value) {
    char tmp[12];
    int n = 0;
    unsigned int u;

    if (value < 0) {
        *dst++ = '-';
        u = (unsigned int)(-(value + 1)) + 1U;   /* avoid INT_MIN overflow */
    } else {
        u = (unsigned int)value;
    }
    do {
        tmp[n++] = (char)('0' + (u % 10U));
        u /= 10U;
    } while (u != 0U);
    while (n > 0) *dst++ = tmp[--n];
    *dst = '\0';
    return dst;
}

char *strerror(int errnum) {
    static char unknown[32];
    char *p;

    if (errnum >= 0 && errnum < ERRMSGS_COUNT && errmsgs[errnum]) {
        return (char *)errmsgs[errnum];
    }

    errno = EINVAL;   /* permitted by POSIX for an unknown error number */
    p = unknown;
    /* "Unknown error " + signed int */
    const char *prefix = "Unknown error ";
    while (*prefix) *p++ = *prefix++;
    fmt_int(p, errnum);
    return unknown;
}

void perror(const char *s) {
    /* Capture errno before any call (strerror may itself set it). */
    int save = errno;
    const char *msg = strerror(save);
    if (s && *s) {
        write(2, s, strlen(s));
        write(2, ": ", 2);
    }
    write(2, msg, strlen(msg));
    write(2, "\n", 1);
    errno = save;     /* do not perturb errno across a successful perror() */
}

/* ---- math (double precision) ---- */

double fabs(double x) { return __builtin_fabs(x); }

double sqrt(double x) {
    /* x86_64 has SSE2 in the baseline ABI; the builtin lowers to sqrtsd. */
    if (x < 0.0) return NAN;
    return __builtin_sqrt(x);
}

double floor(double x) {
    double t = (double)(long long)x;          /* truncate toward zero */
    return (t > x) ? t - 1.0 : t;
}

double ceil(double x) {
    double t = (double)(long long)x;
    return (t < x) ? t + 1.0 : t;
}

double exp(double x) {
    /* Range-reduce x = k*ln2 + r, then Taylor-series e^r for |r| <= ln2/2. */
    const double LN2 = 0.69314718055994530942;
    if (x != x) return x;                      /* NaN */
    if (x >  709.0) return HUGE_VAL;
    if (x < -745.0) return 0.0;
    long long k = (long long)(x / LN2 + (x >= 0 ? 0.5 : -0.5));
    double r = x - (double)k * LN2;
    double term = 1.0, sum = 1.0;
    for (int n = 1; n < 18; n++) {
        term *= r / (double)n;
        sum  += term;
    }
    /* Scale by 2^k via repeated multiply (k is small after reduction). */
    double scale = 1.0;
    double base  = (k >= 0) ? 2.0 : 0.5;
    long long kk = (k >= 0) ? k : -k;
    while (kk--) scale *= base;
    return sum * scale;
}

double log(double x) {
    /* log(x): reduce x = m * 2^e with m in [1,2), use atanh series on
     * s = (m-1)/(m+1):  log(m) = 2*(s + s^3/3 + s^5/5 + ...). */
    if (x != x || x < 0.0) return NAN;
    if (x == 0.0) return -HUGE_VAL;
    const double LN2 = 0.69314718055994530942;
    int e = 0;
    while (x >= 2.0) { x *= 0.5; e++; }
    while (x <  1.0) { x *= 2.0; e--; }
    double s = (x - 1.0) / (x + 1.0);
    double s2 = s * s;
    double term = s, sum = 0.0;
    for (int n = 1; n < 40; n += 2) {
        sum  += term / (double)n;
        term *= s2;
    }
    return 2.0 * sum + (double)e * LN2;
}

double log2(double x) {
    const double LN2 = 0.69314718055994530942;
    return log(x) / LN2;
}

double pow(double base, double e) {
    if (e == 0.0) return 1.0;
    if (base == 0.0) return (e > 0.0) ? 0.0 : HUGE_VAL;
    /* Integer exponent: exact repeated multiply. */
    double ip = (double)(long long)e;
    if (ip == e) {
        long long n = (long long)e;
        int neg = n < 0;
        if (neg) n = -n;
        double r = 1.0;
        while (n--) r *= base;
        return neg ? 1.0 / r : r;
    }
    /* General case: base^e = exp(e * log(base)); negative base undefined. */
    if (base < 0.0) return NAN;
    return exp(e * log(base));
}

double sin(double x) {
    /* Reduce to [-pi, pi] then 9-term Taylor series. */
    const double TWO_PI = 6.28318530717958647692;
    const double PI = M_PI;
    while (x >  PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    double x2 = x * x, term = x, sum = x;
    for (int n = 1; n < 13; n++) {
        term *= -x2 / (double)((2 * n) * (2 * n + 1));
        sum  += term;
    }
    return sum;
}

double cos(double x) {
    const double TWO_PI = 6.28318530717958647692;
    const double PI = M_PI;
    while (x >  PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    double x2 = x * x, term = 1.0, sum = 1.0;
    for (int n = 1; n < 13; n++) {
        term *= -x2 / (double)((2 * n - 1) * (2 * n));
        sum  += term;
    }
    return sum;
}

/* ---- ctype (C locale, ASCII) ----
 *
 * Each predicate returns non-zero (true) or 0 (false).  Arguments outside the
 * unsigned char / EOF range are treated as "not a member of any class". */

int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isalpha(int c)  { return isupper(c) || islower(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isspace(int c)  {
    return c == ' '  || c == '\t' || c == '\n' ||
           c == '\v' || c == '\f' || c == '\r';
}
int isblank(int c)  { return c == ' ' || c == '\t'; }
int iscntrl(int c)  { return (c >= 0 && c <= 0x1F) || c == 0x7F; }
int isprint(int c)  { return c >= 0x20 && c <= 0x7E; }
int isgraph(int c)  { return c > 0x20 && c <= 0x7E; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }
int isxdigit(int c) {
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int tolower(int c)  { return isupper(c) ? c + ('a' - 'A') : c; }
int toupper(int c)  { return islower(c) ? c - ('a' - 'A') : c; }

/* ---- stdlib ---- */

void exit(int status) {
    _exit(status);
    for (;;) { }   /* _exit does not return; keep the compiler happy */
}

void abort(void) {
    static const char msg[] = "abort\n";
    write(2, msg, sizeof(msg) - 1);
    /* POSIX abort() would raise SIGABRT; until P4 we exit with 128 + SIGABRT
     * (SIGABRT == 6), matching the shell's convention for signal deaths. */
    _exit(134);
    for (;;) { }
}

/* Backing routine for the assert() macro (libc/include/assert.h). */
void __assert_fail(const char *expr, const char *file, int line,
                   const char *func) {
    printf("%s:%d: %s: Assertion `%s' failed.\n",
           file ? file : "?", line, func ? func : "?", expr ? expr : "?");
    abort();
}

int atoi(const char *s) {
    int sign = 1, result = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result * sign;
}

long strtol(const char *s, char **end, int base) {
    long result = 0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    }
    for (;;) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        s++;
    }
    if (end) *end = (char *)s;
    return result * sign;
}

static unsigned int rng_seed = 1;

void srand(unsigned int seed) {
    rng_seed = seed ? seed : 1;
}

int rand(void) {
    rng_seed ^= rng_seed << 13;
    rng_seed ^= rng_seed >> 17;
    rng_seed ^= rng_seed << 5;
    return (int)(rng_seed & 0x7FFFFFFF);
}

/* ---- String functions ---- */

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = a, *pb = b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *p = dst;
    while (*p) p++;
    while ((*p++ = *src++));
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0;
}

static char *strtok_save = 0;

char *strtok(char *s, const char *delim) {
    if (s) strtok_save = s;
    if (!strtok_save || !*strtok_save) return 0;
    char *p = strtok_save;
    while (*p) {
        const char *d = delim;
        while (*d) { if (*p == *d) break; d++; }
        if (!*d) break;
        p++;
    }
    if (!*p) { strtok_save = p; return 0; }
    char *tok = p;
    while (*p) {
        const char *d = delim;
        while (*d) { if (*p == *d) { *p = '\0'; strtok_save = p + 1; return tok; } d++; }
        p++;
    }
    strtok_save = p;
    return tok;
}

/* ---- stdio ---- */

int putchar(int c) {
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int puts(const char *s) {
    write(1, s, strlen(s));
    putchar('\n');
    return 0;
}

static void print_uint(uint64_t val, unsigned base, int upper, int width, int zero) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[32];
    int i = 0;
    if (val == 0) buf[i++] = '0';
    while (val) { buf[i++] = digits[val % base]; val /= base; }
    int pad = width - i;
    while (pad-- > 0) putchar(zero ? '0' : ' ');
    while (i--) putchar(buf[i]);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { putchar(*fmt); continue; }
        fmt++;
        int zero = 0, width = 0;
        while (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        /* Parse length modifiers (l, ll, h). */
        int is_long = 0;
        while (*fmt == 'l') { is_long++; fmt++; }
        while (*fmt == 'h') { fmt++; }
        switch (*fmt) {
        case '%': putchar('%'); break;
        case 'c': putchar(va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            write(1, s, strlen(s));
            break;
        }
        case 'd': {
            int64_t v = is_long ? va_arg(ap, int64_t)
                                : (int64_t)(int)va_arg(ap, int);
            if (v < 0) { putchar('-'); print_uint((uint64_t)(-(v+1))+1, 10, 0, width, zero); }
            else print_uint((uint64_t)v, 10, 0, width, zero);
            break;
        }
        case 'u': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)(unsigned)va_arg(ap, unsigned);
            print_uint(v, 10, 0, width, zero);
            break;
        }
        case 'o': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)(unsigned)va_arg(ap, unsigned);
            print_uint(v, 8, 0, width, zero);
            break;
        }
        case 'x': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)(unsigned)va_arg(ap, unsigned);
            print_uint(v, 16, 0, width, zero);
            break;
        }
        case 'X': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)(unsigned)va_arg(ap, unsigned);
            print_uint(v, 16, 1, width, zero);
            break;
        }
        case '\0': fmt--; break;
        default: putchar('%'); putchar(*fmt); break;
        }
    }
    va_end(ap);
    return 0;
}

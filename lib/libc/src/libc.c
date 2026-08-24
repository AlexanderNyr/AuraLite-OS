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
#include "sched.h"
#include "sys/resource.h"
#include "sys/uio.h"
#include "sys/wait.h"
#include "signal.h"
#include "termios.h"
#include "sys/ioctl.h"
#include "time.h"   /* P8 */
#include "sys/select.h"
#include "sys/socket.h"

/* ---- errno storage ----
 *
 * FIX_R3 (the P9 follow-up this comment predicted): errno is thread-local.
 * Two storage sites exist:
 *
 *   - __errno_storage, a plain global: the pre-TLS fallback.  It serves the
 *     very start of every process (the crt0 -> __libc_start_main window,
 *     before the main thread's TCB is installed) and, defensively, any
 *     process whose TLS install fails at startup — such a process degrades
 *     to exactly the pre-R3 single-global behaviour instead of faulting.
 *
 *   - pthread_tcb.errno_cell: used from the moment __errno_tls_ready is
 *     set.  The main thread's cell lives in the static __main_tcb below;
 *     pthread children get their own blocks at the top of their stacks via
 *     CLONE_SETTLS.  The kernel keeps FS.base == the running thread's
 *     tls_base at every context switch and every user-mode entry, so
 *     ptls_self() is valid in every thread after start-up.
 *
 * __errno_tls_ready is a process-wide flag, which is sound because the
 * install happens exactly once, single-threaded, before main() — hence
 * before any thread can exist.  No caller changes: errno stays a macro
 * over this accessor. */
#include "pthread_tls.h"

static int __errno_storage = 0;
static struct pthread_tcb __main_tcb;
static volatile int __errno_tls_ready = 0;

int *__errno_location(void) {
    if (__errno_tls_ready)
        return &ptls_self()->errno_cell;
    return &__errno_storage;
}

/* Install the MAIN thread's TLS block, at the very top of
 * __libc_start_main(): before main(), hence before any thread exists, and
 * before any libc code that could observe the pre-TLS window through the
 * per-thread path.  Any errno value set in the pre-install window survives
 * the cut-over (copied into the main cell).  On failure the process keeps
 * the global fallback — the pre-R3 behaviour, and never a fault: reading
 * %fs:0 with an uninstalled FS would dereference address 0. */
static void __tls_errno_install_main(void) {
    __main_tcb.self       = &__main_tcb;
    __main_tcb.errno_cell = __errno_storage;
    /* syscall() is declared in <unistd.h>; SYS_ARCH_PRCTL is 158. */
    long rc = syscall(158, PTLS_ARCH_SET_FS,
                      (uint64_t)(uintptr_t)&__main_tcb, 0, 0, 0, 0);
    if (rc == 0)
        __errno_tls_ready = 1;
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

/* ---- termios / ioctl ---- */

int ioctl(int fd, unsigned long request, void *arg) {
    return (int)syscall_ret(syscall(SYS_IOCTL, (uint64_t)fd, (uint64_t)request,
                                    (uint64_t)arg, 0, 0, 0));
}

int tcgetattr(int fd, struct termios *t) {
    return ioctl(fd, TCGETS, t);
}

int tcsetattr(int fd, int optional_actions, const struct termios *t) {
    unsigned long cmd;
    switch (optional_actions) {
    case TCSANOW:   cmd = TCSETS;  break;
    case TCSADRAIN: cmd = TCSETSW; break;
    case TCSAFLUSH: cmd = TCSETSF; break;
    default: errno = EINVAL; return -1;
    }
    /* ioctl takes a non-const pointer; the SET commands only read it. */
    return ioctl(fd, cmd, (void *)(uintptr_t)t);
}

void cfmakeraw(struct termios *t) {
    /* Matches the glibc/BSD definition (including VMIN/VTIME, which the real
     * glibc cfmakeraw.c does set despite the manual historically omitting it). */
    t->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                              INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~(tcflag_t)OPOST;
    t->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    t->c_cflag |=  (tcflag_t)CS8;
    t->c_cc[VMIN]  = 1;
    t->c_cc[VTIME] = 0;
}

/* Baud handling is a no-op on AuraLite's console/UART (fixed rate). */
speed_t cfgetispeed(const struct termios *t) { (void)t; return 0; }
speed_t cfgetospeed(const struct termios *t) { (void)t; return 0; }
int cfsetispeed(struct termios *t, speed_t speed) { (void)t; (void)speed; return 0; }
int cfsetospeed(struct termios *t, speed_t speed) { (void)t; (void)speed; return 0; }

int isatty(int fd) {
    struct termios t;
    if (ioctl(fd, TCGETS, &t) == 0) return 1;
    /* ioctl set errno (ENOTTY/EBADF); isatty returns 0 on non-tty. */
    return 0;
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

int aura_readdir(const char *path, void *out, int max) {
    return (int)syscall_ret(syscall(80, (uint64_t)path, (uint64_t)out, max,
                                    0, 0, 0));
}

/* ---- P10: working directory + multiplexing wrappers ---- */

char *getcwd(char *buf, size_t size) {
    long r = syscall_ret(syscall(SYS_GETCWD, (uint64_t)buf, (uint64_t)size,
                                 0, 0, 0, 0));
    return r < 0 ? (char *)0 : buf;
}

int chdir(const char *path) {
    return (int)syscall_ret(syscall(SYS_CHDIR, (uint64_t)path, 0, 0, 0, 0, 0));
}

int fchdir(int fd) {
    return (int)syscall_ret(syscall(SYS_FCHDIR, (uint64_t)fd, 0, 0, 0, 0, 0));
}

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout) {
    return (int)syscall_ret(syscall(SYS_SELECT, (uint64_t)nfds,
                                    (uint64_t)readfds, (uint64_t)writefds,
                                    (uint64_t)exceptfds, (uint64_t)timeout, 0));
}

/* Q16: pselect — the mask travels to the kernel and is applied for the
 * duration of the block (pselect6 ABI: a6 = &{ sigset_t*, size_t }). */
int pselect(int nfds, fd_set *readfds, fd_set *writefds,
            fd_set *exceptfds, const struct timespec *timeout,
            const sigset_t *sigmask) {
    struct { const sigset_t *ss; size_t ss_len; } sig;
    uint64_t sig_ptr = 0;
    if (sigmask) {
        sig.ss = sigmask;
        sig.ss_len = sizeof(*sigmask);
        sig_ptr = (uint64_t)(uintptr_t)&sig;
    }
    return (int)syscall_ret(syscall(SYS_PSELECT6, (uint64_t)nfds,
                                    (uint64_t)readfds, (uint64_t)writefds,
                                    (uint64_t)exceptfds, (uint64_t)timeout,
                                    sig_ptr));
}

uint32_t dns_resolve(const char *hostname) {
    /* Returns a raw IPv4 address (0 on failure); not an in-band errno. */
    return (uint32_t)syscall(SYS_DNS, (uint64_t)hostname, 0, 0, 0, 0, 0);
}

int dnsctl(int op, void *buf, uint32_t len) {
    return (int)syscall_ret(syscall(SYS_DNSCTL, (uint64_t)op,
                                    (uint64_t)buf, (uint64_t)len, 0, 0, 0));
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

/* H2: Returns the PMM free-frame count. Used to verify address-space reaping. */
uint64_t get_free_frames(void) {
    return (uint64_t)syscall(SYS_MEMINFO, 0, 0, 0, 0, 0, 0);
}

int net_close(void) {
    return (int)syscall_ret(syscall(SYS_NET_CLOSE, 0, 0, 0, 0, 0, 0));
}

int net_ping(uint32_t ip) {
    return (int)syscall_ret(syscall(SYS_NET_PING, ip, 0, 0, 0, 0, 0));
}

/* X7: ICMPv6 echo.  The address is passed as 16 raw bytes (big-endian on the
 * wire), so the caller formats the text address; the kernel validates bounds. */
int net_ping6(const uint8_t addr[16]) {
    return (int)syscall_ret(syscall(SYS_PING6, (uint64_t)addr, 0, 0, 0, 0, 0));
}

int socket(int domain, int type, int protocol) {
    return (int)syscall_ret(syscall(SYS_SOCKET, (uint64_t)domain, (uint64_t)type,
                                    (uint64_t)protocol, 0, 0, 0));
}

int connect(int sock, uint32_t ip, uint16_t port) {
    return (int)syscall_ret(syscall(SYS_SOCKET_CONNECT, (uint64_t)sock, ip,
                                    port, 0, 0, 0));
}

int connectaddr(int sock, const struct sockaddr *addr, unsigned addrlen) {
    if (!addr) { errno = EFAULT; return -1; }
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *sin;
        uint32_t ip;
        uint16_t port;
        if (addrlen < sizeof(struct sockaddr_in)) { errno = EINVAL; return -1; }
        sin = (const struct sockaddr_in *)addr;
        ip = ntohl(sin->sin_addr.s_addr);
        port = ntohs(sin->sin_port);
        return connect(sock, ip, port);
    }
    if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6;
        uint16_t port;
        if (addrlen < sizeof(struct sockaddr_in6)) { errno = EINVAL; return -1; }
        s6 = (const struct sockaddr_in6 *)addr;
        port = ntohs(s6->sin6_port);
        return (int)syscall_ret(syscall(SYS_SOCKET_CONNECT6, (uint64_t)sock,
                                        (uint64_t)s6->sin6_addr.s6_addr,
                                        port, 0, 0, 0));
    }
    errno = EAFNOSUPPORT;
    return -1;
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

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!addr || addrlen < sizeof(struct sockaddr_in)) return -1;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    uint32_t ip = ntohl(sin->sin_addr.s_addr);
    uint16_t port = ntohs(sin->sin_port);
    return (int)syscall_ret(syscall(SYS_SOCKET_BIND, (uint64_t)sockfd, ip, port, 0, 0, 0));
}

int listen(int sockfd, int backlog) {
    return (int)syscall_ret(syscall(SYS_SOCKET_LISTEN, (uint64_t)sockfd, (uint64_t)backlog, 0, 0, 0, 0));
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    int ret = (int)syscall_ret(syscall(SYS_SOCKET_ACCEPT, (uint64_t)sockfd, (uint64_t)&peer_ip, (uint64_t)&peer_port, 0, 0, 0));
    if (ret >= 0 && addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *sin = (struct sockaddr_in *)addr;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(peer_port);
        sin->sin_addr.s_addr = htonl(peer_ip);
        *addrlen = sizeof(struct sockaddr_in);
    }
    return ret;
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    (void)flags;
    return (ssize_t)syscall_ret(syscall(SYS_SENDTO, (uint64_t)sockfd,
                                        (uint64_t)buf, (uint64_t)len,
                                        0, (uint64_t)dest_addr,
                                        (uint64_t)addrlen));
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
    (void)flags;
    return (ssize_t)syscall_ret(syscall(SYS_RECVFROM, (uint64_t)sockfd,
                                        (uint64_t)buf, (uint64_t)len,
                                        0, (uint64_t)src_addr,
                                        (uint64_t)addrlen));
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    (void)sockfd; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    (void)sockfd; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

int mkdir(const char *path, mode_t mode) {
    return (int)syscall_ret(syscall(SYS_MKDIR, (uint64_t)path, (uint64_t)mode, 0, 0, 0, 0));
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
int lstat(const char *path, struct stat *out) {
    return (int)syscall_ret(syscall(SYS_LSTAT, (uint64_t)path, (uint64_t)out,
                                    0, 0, 0, 0));
}
int fstat(int fd, struct stat *out) {
    return (int)syscall_ret(syscall(SYS_FSTAT, (uint64_t)fd, (uint64_t)out,
                                    0, 0, 0, 0));
}
int mkfifo(const char *path, mode_t mode) {
    return (int)syscall_ret(syscall(SYS_MKFIFO, (uint64_t)path, (uint64_t)mode,
                                    0, 0, 0, 0));
}
int symlink(const char *target, const char *linkpath) {
    return (int)syscall_ret(syscall(SYS_SYMLINK, (uint64_t)target,
                                    (uint64_t)linkpath, 0, 0, 0, 0));
}
int64_t readlink(const char *path, char *buf, size_t bufsiz) {
    return (int64_t)syscall_ret(syscall(SYS_READLINK, (uint64_t)path,
                                        (uint64_t)buf, (uint64_t)bufsiz,
                                        0, 0, 0));
}
int access(const char *path, int mode) {
    return (int)syscall_ret(syscall(SYS_ACCESS, (uint64_t)path, (uint64_t)mode, 0, 0, 0, 0));
}
int chmod(const char *path, mode_t mode) {
    return (int)syscall_ret(syscall(SYS_CHMOD, (uint64_t)path, (uint64_t)mode, 0, 0, 0, 0));
}
int fchmod(int fd, mode_t mode) {
    return (int)syscall_ret(syscall(SYS_FCHMOD, (uint64_t)fd, (uint64_t)mode, 0, 0, 0, 0));
}
int chown(const char *path, uid_t owner, gid_t group) {
    return (int)syscall_ret(syscall(SYS_CHOWN, (uint64_t)path, (uint64_t)owner, (uint64_t)group, 0, 0, 0));
}
int fchown(int fd, uid_t owner, gid_t group) {
    return (int)syscall_ret(syscall(SYS_FCHOWN, (uint64_t)fd, (uint64_t)owner, (uint64_t)group, 0, 0, 0));
}
mode_t umask(mode_t mask) {
    return (mode_t)syscall(SYS_UMASK, (uint64_t)mask, 0, 0, 0, 0, 0);
}
uid_t getuid(void) { return (uid_t)syscall(SYS_GETUID, 0, 0, 0, 0, 0, 0); }
uid_t geteuid(void) { return (uid_t)syscall(SYS_GETEUID, 0, 0, 0, 0, 0, 0); }
gid_t getgid(void) { return (gid_t)syscall(SYS_GETGID, 0, 0, 0, 0, 0, 0); }
gid_t getegid(void) { return (gid_t)syscall(SYS_GETEGID, 0, 0, 0, 0, 0, 0); }
int setuid(uid_t uid) { return (int)syscall_ret(syscall(SYS_SETUID, (uint64_t)uid, 0, 0, 0, 0, 0)); }
int setgid(gid_t gid) { return (int)syscall_ret(syscall(SYS_SETGID, (uint64_t)gid, 0, 0, 0, 0, 0)); }
int setreuid(uid_t ruid, uid_t euid) { return (int)syscall_ret(syscall(SYS_SETREUID, (uint64_t)ruid, (uint64_t)euid, 0, 0, 0, 0)); }
int setregid(gid_t rgid, gid_t egid) { return (int)syscall_ret(syscall(SYS_SETREGID, (uint64_t)rgid, (uint64_t)egid, 0, 0, 0, 0)); }
int getgroups(int size, gid_t list[]) { return (int)syscall_ret(syscall(SYS_GETGROUPS, (uint64_t)size, (uint64_t)list, 0, 0, 0, 0)); }
int setgroups(size_t size, const gid_t *list) { return (int)syscall_ret(syscall(SYS_SETGROUPS, (uint64_t)size, (uint64_t)list, 0, 0, 0, 0)); }

pid_t fork(void) {
    return (pid_t)syscall_ret(syscall(SYS_FORK, 0, 0, 0, 0, 0, 0));
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)syscall_ret(syscall(SYS_EXECVE, (uint64_t)path,
                                    (uint64_t)argv, (uint64_t)envp, 0, 0, 0));
}

/* execv(): like execve() but inherits the current environment. */
int execv(const char *path, char *const argv[]) {
    extern char **environ;
    return execve(path, argv, environ);
}

/* execvp(): search PATH if @file contains no slash, then execv().  Our minimal
 * implementation honours PATH from the environment, defaulting to "/bin". */
int execvp(const char *file, char *const argv[]) {
    if (!file) { errno = ENOENT; return -1; }
    if (strchr(file, '/')) return execv(file, argv);

    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin";

    char buf[256];
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t seg = colon ? (size_t)(colon - p) : strlen(p);
        if (seg + 1 + strlen(file) + 1 < sizeof(buf)) {
            memcpy(buf, p, seg);
            buf[seg] = '/';
            strcpy(buf + seg + 1, file);
            execv(buf, argv);   /* returns only on failure */
        }
        if (!colon) break;
        p = colon + 1;
    }
    errno = ENOENT;
    return -1;
}

pid_t wait(int *status) {
    return waitpid((pid_t)-1, status, 0);
}

pid_t spawn(const char *path) {
    return (pid_t)syscall_ret(syscall(SYS_SPAWN, (uint64_t)path, 0, 0, 0, 0, 0));
}

/* spawnv() — spawn with arguments (SDK_PLAN phase S3).
 *
 * @argv is a NULL-terminated vector, conventionally with argv[0] naming the
 * program.  Passing NULL is identical to spawn().
 *
 * This exists because spawn() could not forward arguments, and the workaround
 * was a convention: write them to a file the child agrees to read
 * (/tmp/apm.args and friends).  That is fine between programs in one
 * repository that agreed on it; it is not something to teach a third party. */
pid_t spawnv(const char *path, char *const argv[]) {
    return (pid_t)syscall_ret(syscall(SYS_SPAWN, (uint64_t)path,
                                      (uint64_t)argv, 0, 0, 0, 0));
}

pid_t waitpid(pid_t pid, int *status, int options) {
    /* The kernel writes a 32-bit POSIX status word directly to @status, and
     * returns the reaped pid, 0 (WNOHANG: none ready), or a negative errno. */
    return (pid_t)syscall_ret(syscall(SYS_WAIT4, (uint64_t)pid,
                                      (uint64_t)status, (uint64_t)options,
                                      0, 0, 0));
}

pid_t setsid(void) {
    return (pid_t)syscall_ret(syscall(SYS_SETSID, 0, 0, 0, 0, 0, 0));
}
int setpgid(pid_t pid, pid_t pgid) {
    return (int)syscall_ret(syscall(SYS_SETPGID, (uint64_t)pid, (uint64_t)pgid,
                                    0, 0, 0, 0));
}
pid_t getpgid(pid_t pid) {
    return (pid_t)syscall_ret(syscall(SYS_GETPGID, (uint64_t)pid, 0, 0, 0, 0, 0));
}
pid_t getsid(pid_t pid) {
    return (pid_t)syscall_ret(syscall(SYS_GETSID, (uint64_t)pid, 0, 0, 0, 0, 0));
}
pid_t getpgrp(void) {
    return getpgid(0);
}

/* ---- P8: time wrappers ---- */
int clock_gettime(clockid_t clockid, struct timespec *tp) {
    return (int)syscall_ret(syscall(228, (uint64_t)clockid, (uint64_t)tp, 0, 0, 0, 0));
}

int clock_getres(clockid_t clockid, struct timespec *res) {
    return (int)syscall_ret(syscall(229, (uint64_t)clockid, (uint64_t)res, 0, 0, 0, 0));
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    return (int)syscall_ret(syscall(35, (uint64_t)req, (uint64_t)rem, 0, 0, 0, 0));
}

int gettimeofday(struct timeval *tv, void *tz) {
    return (int)syscall_ret(syscall(96, (uint64_t)tv, (uint64_t)tz, 0, 0, 0, 0));
}

time_t time(time_t *t) {
    return (time_t)syscall(520, (uint64_t)t, 0, 0, 0, 0, 0);
}

int getitimer(int which, struct itimerval *curr) {
    return (int)syscall_ret(syscall(36, (uint64_t)which, (uint64_t)curr, 0, 0, 0, 0));
}

int setitimer(int which, const struct itimerval *new, struct itimerval *old) {
    return (int)syscall_ret(syscall(38, (uint64_t)which, (uint64_t)new, (uint64_t)old, 0, 0, 0));
}

unsigned int sleep(unsigned int seconds) {
    struct timespec ts = {seconds, 0};
    nanosleep(&ts, NULL);
    return 0;
}

int usleep(unsigned long usec) {
    struct timespec ts = {0, (long)usec * 1000L};
    return nanosleep(&ts, NULL);
}

/* P9 pthread symbols (thin wrappers) */
#include "pthread.h"
pid_t tcgetpgrp(int fd) {
    int pgid = 0;
    if (ioctl(fd, TIOCGPGRP, &pgid) < 0) return -1;
    return (pid_t)pgid;
}
int tcsetpgrp(int fd, pid_t pgid) {
    int p = (int)pgid;
    return ioctl(fd, TIOCSPGRP, &p);
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

/* M9: fsync — flush this descriptor's cached pages to the filesystem.
 * SYS_FSYNC was defined but unhandled before M9, so this returned -ENOSYS. */
int fsync(int fd) {
    return (int)syscall_ret(syscall(74, (uint64_t)fd, 0, 0, 0, 0, 0));
}

/* A6: msync — write a shared file mapping back through the page cache. */
int msync(void *addr, size_t length, int flags) {
    return (int)syscall_ret(syscall(26, (uint64_t)addr, (uint64_t)length,
                                    (uint64_t)flags, 0, 0, 0));
}

/* M4: madvise — advisory hints about memory usage patterns. */
int madvise(void *addr, size_t length, int advice) {
    return (int)syscall_ret(syscall(28, (uint64_t)addr, (uint64_t)length,
                                    (uint64_t)advice, 0, 0, 0));
}

/* M4: mincore — report which pages are resident in memory. */
int mincore(void *addr, size_t length, unsigned char *vec) {
    return (int)syscall_ret(syscall(27, (uint64_t)addr, (uint64_t)length,
                                    (uint64_t)vec, 0, 0, 0));
}

/* M4: mlock/munlock — advisory stubs (no eviction mechanism yet). */
int mlock(const void *addr, size_t length) {
    return (int)syscall_ret(syscall(149, (uint64_t)addr, (uint64_t)length,
                                    0, 0, 0, 0));
}
int munlock(const void *addr, size_t length) {
    return (int)syscall_ret(syscall(150, (uint64_t)addr, (uint64_t)length,
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
    [EAFNOSUPPORT]    = "Address family not supported by protocol",
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

void __stdio_cleanup(void);   /* defined with the FILE* layer below */
void __run_atexit(void);      /* defined in stdlib_extra.c */

void exit(int status) {
    __run_atexit();      /* run atexit() handlers in reverse order (C11 §7.22.4.4) */
    __stdio_cleanup();   /* then flush all buffered output streams */
    _exit(status);
    for (;;) { }   /* _exit does not return; keep the compiler happy */
}

/* C runtime bootstrap, invoked by crt0 with the decoded initial stack.
 * Publishes the environment to `environ`, runs the .init_array constructors,
 * then main(argc, argv, envp), then the .fini_array destructors in reverse,
 * then exit()s with main's return value.  Programs that declare
 * `int main(void)` simply ignore the extra arguments.  The array bounds come
 * from user.ld; with no constructors/destructors they link as start == end
 * and the walks loop zero times. */
extern char **environ;
extern int main(int argc, char **argv, char **envp);

typedef void (*init_fini_fn_t)(void);
extern init_fini_fn_t __init_array_start[], __init_array_end[];
extern init_fini_fn_t __fini_array_start[], __fini_array_end[];

/* Constructor order is link order (forward), destructor order unwinds it
 * (reverse) — the pairing glibc documents for .init_array/.fini_array. */
static void run_init_array(void) {
    for (init_fini_fn_t *fn = __init_array_start; fn < __init_array_end; fn++) {
        (*fn)();
    }
}

static void run_fini_array(void) {
    init_fini_fn_t *fn = __fini_array_end;
    while (fn > __fini_array_start) {
        (*--fn)();
    }
}

/* M5: the auxiliary vector, located by __libc_start_main from the envp
 * pointer on the initial process stack.  getauxval() scans it. */
unsigned long *__libc_auxv = NULL;

void __libc_start_main(int argc, char **argv, char **envp) {
    /* FIX_R3 first thing: install the main thread's TCB so errno is
     * per-thread from main() onwards, in every program, threaded or not.
     * Must precede anything that could set/read errno through the TLS
     * path; crt0 (pure asm) does not touch errno. */
    __tls_errno_install_main();
    environ = envp;
    /* M5: locate the auxv -- it follows envp's NULL terminator on the initial
     * process stack; each entry is an (a_type, a_val) pair ended by AT_NULL. */
    {
        char **e = envp;
        if (e) { while (*e) e++; }
        __libc_auxv = (unsigned long *)(e + 1);
    }
    run_init_array();   /* ctors may use environ and the TLS errno */
    int rc = main(argc, argv, envp);
    run_fini_array();
    exit(rc);
    for (;;) { }   /* exit() does not return */
}

/* M5: getauxval(3) -- return the value of auxiliary-vector entry @type, or 0
 * if absent (0 is also AT_NULL's value, but no real entry has type 0). */
unsigned long getauxval(unsigned long type) {
    unsigned long *a = __libc_auxv;
    if (!a) return 0;
    while (a[0] != 0) {       /* AT_NULL terminates with a_type == 0 */
        if (a[0] == type) return a[1];
        a += 2;
    }
    return 0;
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

unsigned long strtoul(const char *s, char **end, int base) {
    unsigned long result = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '+') s++;
    else if (*s == '-') s++;   /* strtoul accepts a sign; we ignore it */
    if (base == 0) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    for (;;) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * (unsigned long)base + (unsigned long)digit;
        s++;
    }
    if (end) *end = (char *)s;
    return result;
}

long long strtoll(const char *s, char **end, int base) {
    return (long long)strtol(s, end, base);
}

unsigned long long strtoull(const char *s, char **end, int base) {
    return (unsigned long long)strtoul(s, end, base);
}

double strtod(const char *s, char **end) {
    const char *start = s;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;

    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;

    int any = 0;
    double value = 0.0;
    while (*s >= '0' && *s <= '9') { value = value * 10.0 + (*s - '0'); s++; any = 1; }

    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            value += (*s - '0') * frac;
            frac *= 0.1;
            s++; any = 1;
        }
    }

    if (any && (*s == 'e' || *s == 'E')) {
        s++;
        int esign = 1;
        if (*s == '-') { esign = -1; s++; }
        else if (*s == '+') s++;
        int exp = 0;
        while (*s >= '0' && *s <= '9') { exp = exp * 10 + (*s - '0'); s++; }
        double p = 1.0;
        for (int i = 0; i < exp; i++) p *= 10.0;
        if (esign < 0) value /= p; else value *= p;
    }

    if (!any) { if (end) *end = (char *)start; return 0.0; }
    if (end) *end = (char *)s;
    return sign * value;
}

float strtof(const char *s, char **end) {
    return (float)strtod(s, end);
}

long double strtold(const char *s, char **end) {
    return (long double)strtod(s, end);
}

double atof(const char *s) {
    return strtod(s, (char **)0);
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

/* OPT_PLAN.md O1: memset/memcpy mirror the kernel's rep-string backend
 * (kernel/arch/x86_64/string_fast.c — same shapes, same movsq-bulk +
 * movsb-tail split, same 64-byte scalar crossover; the reasoning and the
 * TCG measurement that forced movsq over movsb live in that file's
 * header comment, and membench is the before/after record).  The
 * portable byte loops remain the non-x86_64 fallback. */

#ifdef __x86_64__

#define AURA_STR_SMALL 64

void *memset(void *dst, int c, size_t n) {
    if (n < AURA_STR_SMALL) {
        unsigned char *d = dst;
        while (n--) *d++ = (unsigned char)c;
        return dst;
    }
    void    *d = dst;
    size_t   q = n >> 3;
    size_t   r = n & 7;
    uint64_t v = (unsigned char)c;
    v *= 0x0101010101010101ULL;
    __asm__ volatile ("rep stosq"
                      : "+D"(d), "+c"(q)
                      : "a"(v)
                      : "memory");
    __asm__ volatile ("rep stosb"
                      : "+D"(d), "+c"(r)
                      : "a"(v)
                      : "memory");
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    if (n < AURA_STR_SMALL) {
        unsigned char *d = dst;
        const unsigned char *s = src;
        while (n--) *d++ = *s++;
        return dst;
    }
    void  *d = dst;
    size_t q = n >> 3;
    size_t r = n & 7;
    __asm__ volatile ("rep movsq"
                      : "+D"(d), "+S"(src), "+c"(q)
                      :
                      : "memory");
    __asm__ volatile ("rep movsb"
                      : "+D"(d), "+S"(src), "+c"(r)
                      :
                      : "memory");
    return dst;
}

#else /* !__x86_64__ */

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

#endif /* __x86_64__ */

/* Word-wide memcmp/strlen (OPT_PLAN O1) — same portable shapes as
 * kernel/lib/string.c, see the comments there. */
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = a, *pb = b;
    while (n >= 8) {
        uint64_t wa, wb;
        __builtin_memcpy(&wa, pa, 8);
        __builtin_memcpy(&wb, pb, 8);
        if (wa != wb) {
            for (int i = 0; i < 8; i++) {
                if (pa[i] != pb[i]) return pa[i] - pb[i];
            }
        }
        pa += 8; pb += 8; n -= 8;
    }
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (((uintptr_t)p & 7) != 0) {
        if (*p == '\0') return (size_t)(p - s);
        p++;
    }
    for (;;) {
        uint64_t w;
        __builtin_memcpy(&w, p, 8);
        uint64_t zero = (w - 0x0101010101010101ULL) & ~w &
                        0x8080808080808080ULL;
        if (zero != 0)
            return (size_t)(p - s) + ((size_t)__builtin_ctzll(zero) >> 3);
        p += 8;
    }
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

/* ---- formatting core (callback-based, shared by printf/fprintf/snprintf) ---- */

struct fmt_sink {
    void (*emit)(struct fmt_sink *s, char c);
    int   count;        /* total chars emitted (or that would be emitted) */
    /* For snprintf: */
    char *out;
    size_t cap;         /* capacity including the NUL */
    size_t len;         /* chars actually stored */
};

/* @left requests left-justification ('-'); it takes precedence over @zero,
 * which POSIX says is ignored when '-' is present. */
static void fmt_emit_uint_pad(struct fmt_sink *s, uint64_t val, unsigned base,
                              int upper, int width, int zero, int left) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[32];
    int i = 0;
    if (val == 0) buf[i++] = '0';
    while (val) { buf[i++] = digits[val % base]; val /= base; }
    int pad = width - i;
    if (!left) { while (pad-- > 0) s->emit(s, zero ? '0' : ' '); }
    int n = i;
    while (n--) s->emit(s, buf[n]);
    if (left)  { while (pad-- > 0) s->emit(s, ' '); }
}

static void fmt_emit_uint(struct fmt_sink *s, uint64_t val, unsigned base,
                          int upper, int width, int zero) {
    fmt_emit_uint_pad(s, val, base, upper, width, zero, 0);
}

static int format_to_sink(struct fmt_sink *s, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { s->emit(s, *fmt); continue; }
        fmt++;
        /* Flags.  '-' (left-justify) was not parsed at all before, so a
         * format like "%-12s" fell through and printed the specifier
         * LITERALLY -- every column-aligned table in every program came out
         * as "%-12s %-8s ...".  apm's package listing is what surfaced it. */
        int zero = 0, width = 0, left = 0;
        for (;;) {
            if (*fmt == '-')      { left = 1; fmt++; }
            else if (*fmt == '0') { zero = 1; fmt++; }
            else break;
        }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        /* Precision.  Parsed for every conversion (and ignored by the
         * integer ones) so that "%.2f" and "%.3s" consume their digits
         * instead of leaving them to be printed as literal text. */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
        }
        /* Left-justification and zero-padding are mutually exclusive; POSIX
         * says '0' is ignored when '-' is present. */
        if (left) zero = 0;
        int is_long = 0;
        while (*fmt == 'l') { is_long++; fmt++; }
        while (*fmt == 'h') { fmt++; }
        switch (*fmt) {
        case '%': s->emit(s, '%'); break;
        case 'c': s->emit(s, (char)va_arg(ap, int)); break;
        /* %f / %F -- fixed-point, the default precision of 6.
         *
         * Absent until now, so printf("%f") emitted the literal text "%f".
         * DOOM writes floats into its config file with it, reads them back
         * on the next run, and would have been parsing the string "%f" as a
         * number for ever after.
         *
         * This is a deliberately simple conversion: split off the integer
         * part, then emit `prec` scaled fractional digits with rounding.
         * It does not attempt shortest-round-trip formatting, and %e/%g are
         * still absent -- doing those properly means Grisu/Ryu, which is a
         * project of its own and not one to do halfway. */
        case 'F':
        case 'f': {
            double d = va_arg(ap, double);
            if (prec < 0) prec = 6;
            if (prec > 17) prec = 17;   /* beyond double's precision */

            /* The sign has to come from the SIGN BIT, not from `d < 0`:
             * a comparison is false for every NaN, and negative zero is
             * not less than zero either.  So read the bit directly --
             * that is also what makes "%f" of 0.0/0.0 print "-nan" here
             * exactly as it does on glibc. */
            union { double d; uint64_t u; } fbits;
            fbits.d = d;
            int neg = (int)(fbits.u >> 63);

            if (d != d) {                       /* NaN: compares unequal */
                if (neg) s->emit(s, '-');
                s->emit(s, 'n'); s->emit(s, 'a'); s->emit(s, 'n');
                break;
            }
            if (neg) d = -d;
            /* Infinity, without needing <math.h> here. */
            if (d > 1.7976931348623157e308) {
                if (neg) s->emit(s, '-');
                s->emit(s, 'i'); s->emit(s, 'n'); s->emit(s, 'f');
                break;
            }

            /* Round at the requested precision BEFORE splitting, so that
             * 0.999 at prec 2 gives "1.00" rather than "0.99". */
            double scale = 1.0;
            for (int i = 0; i < prec; i++) scale *= 10.0;
            d += 0.5 / scale;

            uint64_t ip = (uint64_t)d;

            /* Field width counts the sign, the integer digits, the point
             * and the fraction together, so it has to be computed before
             * anything is emitted -- "%08.2f" of 3.5 is "00003.50". */
            int idigits = 1;
            for (uint64_t t = ip; t >= 10; t /= 10) idigits++;
            int total = (neg ? 1 : 0) + idigits + (prec > 0 ? 1 + prec : 0);

            if (!left && !zero) { for (int i = total; i < width; i++) s->emit(s, ' '); }
            if (neg) s->emit(s, '-');
            /* Zero padding goes AFTER the sign, not before it. */
            if (!left && zero) { for (int i = total; i < width; i++) s->emit(s, '0'); }

            fmt_emit_uint_pad(s, ip, 10, 0, 0, 0, 0);

            if (prec > 0) {
                s->emit(s, '.');
                double frac = d - (double)ip;
                for (int i = 0; i < prec; i++) {
                    frac *= 10.0;
                    int digit = (int)frac;
                    if (digit < 0) digit = 0;
                    if (digit > 9) digit = 9;
                    s->emit(s, (char)('0' + digit));
                    frac -= (double)digit;
                }
            }
            if (left) { for (int i = total; i < width; i++) s->emit(s, ' '); }
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            int slen = 0;
            while (str[slen]) slen++;
            int spad = width - slen;
            if (!left) { while (spad-- > 0) s->emit(s, ' '); }
            for (int i = 0; i < slen; i++) s->emit(s, str[i]);
            if (left)  { while (spad-- > 0) s->emit(s, ' '); }
            break;
        }
        /* %i is identical to %d for OUTPUT (C11 7.21.6.1p8); they differ
         * only in *scanf, where %i accepts a base prefix.  Omitting it made
         * printf("%i") emit the literal text "%i", which is how DOOM's
         * config loader ended up looking for a variable actually named
         * "joystick_physical_button%i". */
        case 'i':
        case 'd': {
            int64_t v = is_long ? va_arg(ap, int64_t)
                                : (int64_t)(int)va_arg(ap, int);
            /* For integers, a precision is a MINIMUM DIGIT COUNT, padded
             * with leading zeros (C11 7.21.6.1p5) -- "%.3d" of 7 is "007".
             * It is not the same as a zero-padded width: "%.3d" pads to 3
             * digits regardless of the field width.
             *
             * Ignoring it printed DOOM's font lump names as the literal
             * "STCFN%.3", because the precision digits were consumed but
             * never applied, and the engine then looked up a lump that
             * cannot exist. */
            uint64_t mag = (v < 0) ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
            int w = width;
            if (v < 0 && w > 0) w--;
            if (prec >= 0) {
                /* Count the digits we are about to emit. */
                int digits = 1;
                for (uint64_t t = mag; t >= 10; t /= 10) digits++;
                /* A zero value with a precision of zero converts to NO
                 * characters (C11 7.21.6.1p8) -- "%.0d" of 0 is "", which
                 * is what makes "%.0d" usable for optional fields. */
                if (v == 0 && prec == 0) {
                    for (int i = 0; i < w; i++) s->emit(s, ' ');
                    break;
                }
                int lead = prec - digits;
                /* A precision disables the '0' flag (C11 7.21.6.1p6). */
                int pad = (lead > 0) ? lead : 0;
                int total = digits + pad;
                if (!left) { for (int i = total; i < w; i++) s->emit(s, ' '); }
                if (v < 0) s->emit(s, '-');
                for (int i = 0; i < pad; i++) s->emit(s, '0');
                fmt_emit_uint_pad(s, mag, 10, 0, 0, 0, 0);
                if (left) { for (int i = total; i < w; i++) s->emit(s, ' '); }
            } else if (v < 0) {
                /* The sign must sit next to the digits, so with a SPACE-padded
                 * width the padding comes first: "%5d" of -42 is "  -42", not
                 * "-  42".  Only zero padding goes between sign and digits.
                 * Emitting '-' unconditionally first got this backwards for
                 * every negative number in a width -- a pre-existing bug this
                 * differential test caught, not one the DOOM port introduced. */
                int digits = 1;
                for (uint64_t t = mag; t >= 10; t /= 10) digits++;
                if (!left && !zero) { for (int i = digits; i < w; i++) s->emit(s, ' '); }
                s->emit(s, '-');
                fmt_emit_uint_pad(s, mag, 10, 0, (!left && zero) ? w : 0, zero, 0);
                if (left) { for (int i = digits; i < w; i++) s->emit(s, ' '); }
            } else {
                fmt_emit_uint_pad(s, mag, 10, 0, width, zero, left);
            }
            break;
        }
        case 'u': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)(unsigned)va_arg(ap, unsigned);
            fmt_emit_uint_pad(s, v, 10, 0, width, zero, left);
            break;
        }
        case 'o': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)(unsigned)va_arg(ap, unsigned);
            fmt_emit_uint(s, v, 8, 0, width, zero);
            break;
        }
        case 'p': {
            void *pv = va_arg(ap, void *);
            s->emit(s, '0'); s->emit(s, 'x');
            fmt_emit_uint(s, (uint64_t)(uintptr_t)pv, 16, 0, 0, 0);
            break;
        }
        case 'x': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)(unsigned)va_arg(ap, unsigned);
            fmt_emit_uint_pad(s, v, 16, 0, width, zero, left);
            break;
        }
        case 'X': {
            uint64_t v = is_long ? va_arg(ap, uint64_t)
                                 : (uint64_t)(unsigned)va_arg(ap, unsigned);
            fmt_emit_uint_pad(s, v, 16, 1, width, zero, left);
            break;
        }
        case '\0': fmt--; break;
        default: s->emit(s, '%'); s->emit(s, *fmt); break;
        }
    }
    return s->count;
}

/* snprintf sink: store into a bounded buffer, always counting. */
static void sink_str_emit(struct fmt_sink *s, char c) {
    if (s->len + 1 < s->cap) s->out[s->len++] = c;
    s->count++;
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    struct fmt_sink s;
    s.emit = sink_str_emit;
    s.count = 0; s.out = str; s.cap = size; s.len = 0;
    int n = format_to_sink(&s, fmt, ap);
    if (size > 0) str[(s.len < size) ? s.len : size - 1] = '\0';
    return n;
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return n;
}

int putchar(int c) { return fputc(c, stdout); }

int puts(const char *s) {
    fputs(s, stdout);
    fputc('\n', stdout);
    return 0;
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

/* ==================================================================== */
/* FILE* stdio streams                                                   */
/* ==================================================================== */

static FILE s_stdin  = { 0, 0, _IOLBF, 0, 0, 0, 0, 0, 0, -1, {0} };
static FILE s_stdout = { 1, 0, _IOLBF, 0, 0, 0, 0, 0, 0, -1, {0} };
static FILE s_stderr = { 2, 0, _IONBF, 0, 0, 0, 0, 0, 0, -1, {0} };

FILE *stdin  = &s_stdin;
FILE *stdout = &s_stdout;
FILE *stderr = &s_stderr;

/* Registry of open streams for exit()-time flushing. */
#define STDIO_MAX_STREAMS 32
static FILE *s_open_streams[STDIO_MAX_STREAMS];
static int   s_streams_inited = 0;

static void stdio_init_once(void) {
    if (s_streams_inited) return;
    s_streams_inited = 1;
    s_stdin.buf  = s_stdin.ibuf;  s_stdin.bufsz  = BUFSIZ;
    s_stdout.buf = s_stdout.ibuf; s_stdout.bufsz = BUFSIZ;
    s_stderr.buf = s_stderr.ibuf; s_stderr.bufsz = BUFSIZ;
    /* Serial/stdout in AuraLite is consumed by the integration harness; keep
     * stdout line-buffered unconditionally so user processes emit progress
     * markers promptly even before the TTY layer is fully available. stdin can
     * stay line-buffered as well. */
    s_stdout.bufmode = _IOLBF;
    s_stdin.bufmode  = _IOLBF;
    s_open_streams[0] = &s_stdin;
    s_open_streams[1] = &s_stdout;
    s_open_streams[2] = &s_stderr;
}

static void stream_register(FILE *f) {
    stdio_init_once();
    for (int i = 0; i < STDIO_MAX_STREAMS; i++) {
        if (s_open_streams[i] == 0) { s_open_streams[i] = f; return; }
    }
}

static void stream_unregister(FILE *f) {
    for (int i = 0; i < STDIO_MAX_STREAMS; i++) {
        if (s_open_streams[i] == f) { s_open_streams[i] = 0; return; }
    }
}

/* Flush buffered write data to the fd. */
int fflush(FILE *f) {
    stdio_init_once();
    if (f == 0) {
        /* fflush(NULL): flush all output streams. */
        int rc = 0;
        for (int i = 0; i < STDIO_MAX_STREAMS; i++) {
            FILE *s = s_open_streams[i];
            if (s && s->dir == 2 && s->bufpos > 0) { if (fflush(s) != 0) rc = -1; }
        }
        return rc;
    }
    if (f->dir == 2 && f->bufpos > 0) {
        int off = 0;
        while (off < f->bufpos) {
            ssize_t w = write(f->fd, f->buf + off, (size_t)(f->bufpos - off));
            if (w <= 0) { f->flags |= FILE_ERR; f->bufpos = 0; return EOF; }
            off += (int)w;
        }
        f->bufpos = 0;
    }
    return 0;
}

static void fputc_buffered(FILE *f, char c) {
    if (!f->buf) { f->buf = f->ibuf; f->bufsz = BUFSIZ; }
    f->dir = 2;
    if (f->bufmode == _IONBF) {
        write(f->fd, &c, 1);
        return;
    }
    f->buf[f->bufpos++] = c;
    if (f->bufpos >= f->bufsz) fflush(f);
    else if (f->bufmode == _IOLBF && c == '\n') fflush(f);
}

int fputc(int c, FILE *f) {
    stdio_init_once();
    fputc_buffered(f, (char)c);
    if (f->flags & FILE_ERR) return EOF;
    return (unsigned char)c;
}
int putc(int c, FILE *f) { return fputc(c, f); }

int fputs(const char *s, FILE *f) {
    stdio_init_once();
    while (*s) { fputc_buffered(f, *s++); }
    return (f->flags & FILE_ERR) ? EOF : 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    stdio_init_once();
    if (size == 0 || nmemb == 0) return 0;
    const char *p = (const char *)ptr;
    size_t total = size * nmemb;
    for (size_t i = 0; i < total; i++) {
        fputc_buffered(f, p[i]);
        if (f->flags & FILE_ERR) return i / size;
    }
    return nmemb;
}

/* Refill the read buffer from the fd.  Returns bytes available, 0 on EOF. */
static int stream_fill(FILE *f) {
    if (!f->buf) { f->buf = f->ibuf; f->bufsz = BUFSIZ; }
    f->dir = 1;
    ssize_t n = read(f->fd, f->buf, (size_t)f->bufsz);
    if (n <= 0) { if (n == 0) f->flags |= FILE_EOF; else f->flags |= FILE_ERR; f->bufcap = 0; f->readpos = 0; return 0; }
    f->bufcap = (int)n;
    f->readpos = 0;
    return (int)n;
}

int fgetc(FILE *f) {
    stdio_init_once();
    if (f->ungot != -1) { int c = f->ungot; f->ungot = -1; return c; }
    if (f->readpos >= f->bufcap) {
        if (stream_fill(f) == 0) return EOF;
    }
    return (unsigned char)f->buf[f->readpos++];
}
int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
    if (c == EOF) return EOF;
    f->ungot = (unsigned char)c;
    f->flags &= ~FILE_EOF;     /* ungetc clears the EOF indicator */
    return (unsigned char)c;
}

char *fgets(char *s, int size, FILE *f) {
    stdio_init_once();
    if (size <= 0) return 0;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return 0;       /* EOF with no data */
    s[i] = '\0';
    return s;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    stdio_init_once();
    if (size == 0 || nmemb == 0) return 0;
    char *p = (char *)ptr;
    size_t total = size * nmemb;
    size_t got = 0;

    /* Block copy, not a byte-at-a-time fgetc() loop.
     *
     * This used to be `while (got < total) p[got++] = fgetc(f);`, which is
     * correct and catastrophically slow: one function call, one ungot
     * check, one buffer-bounds check and one refill test PER BYTE.  With
     * BUFSIZ at 1024 that is ~29 million iterations to read a 28 MB file,
     * and it is why DOOM took 63 seconds to start -- 37 of them inside
     * R_Init, which pulls every wall texture out of the IWAD one lump at
     * a time.
     *
     * Three paths, in the order they pay off:
     *   1. Drain whatever is already buffered with a single memcpy.
     *   2. For a request still larger than the buffer, read() straight
     *      into the caller's memory and skip the bounce entirely -- this
     *      is the one that matters for WAD lumps, which are far bigger
     *      than BUFSIZ.
     *   3. Otherwise refill the buffer and go round again.
     */
    if (f->ungot != -1) {
        p[got++] = (char)f->ungot;
        f->ungot = -1;
    }

    while (got < total) {
        /* 1. Anything already in the buffer. */
        int avail = f->bufcap - f->readpos;
        if (avail > 0) {
            size_t want = total - got;
            size_t take = ((size_t)avail < want) ? (size_t)avail : want;
            memcpy(p + got, f->buf + f->readpos, take);
            f->readpos += (int)take;
            got += take;
            continue;
        }

        /* 2. Big remainder: bypass the buffer completely. */
        size_t want = total - got;
        if (!f->buf) { f->buf = f->ibuf; f->bufsz = BUFSIZ; }
        if (want >= (size_t)f->bufsz) {
            f->dir = 1;
            ssize_t n = read(f->fd, p + got, want);
            if (n <= 0) {
                if (n == 0) f->flags |= FILE_EOF;
                else        f->flags |= FILE_ERR;
                break;
            }
            got += (size_t)n;
            continue;
        }

        /* 3. Small remainder: refill and loop. */
        if (stream_fill(f) == 0) break;
    }

    return got / size;
}

/* vfprintf sink: emit through the stream's buffered path. */
static FILE *s_vf_target;
static void sink_file_emit(struct fmt_sink *s, char c) {
    fputc_buffered(s_vf_target, c);
    s->count++;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    stdio_init_once();
    struct fmt_sink s;
    s.emit = sink_file_emit; s.count = 0; s.out = 0; s.cap = 0; s.len = 0;
    s_vf_target = f;
    return format_to_sink(&s, fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

static int parse_mode(const char *mode, int *o_flags) {
    /* Returns 0 on success, fills O_* flags. */
    if (!mode || !*mode) return -1;
    int rw = 0;
    switch (mode[0]) {
    case 'r': rw = O_RDONLY; break;
    case 'w': rw = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 'a': rw = O_WRONLY | O_CREAT | O_APPEND; break;
    default: return -1;
    }
    if (mode[1] == '+' || (mode[1] == 'b' && mode[2] == '+')) {
        rw = (rw & ~O_ACCMODE) | O_RDWR;
    }
    *o_flags = rw;
    return 0;
}

FILE *fdopen(int fd, const char *mode) {
    stdio_init_once();
    (void)mode;
    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    f->fd = fd;
    f->bufmode = isatty(fd) ? _IOLBF : _IOFBF;
    f->buf = f->ibuf; f->bufsz = BUFSIZ;
    f->ungot = -1;
    stream_register(f);
    return f;
}

FILE *fopen(const char *path, const char *mode) {
    int oflags;
    if (parse_mode(mode, &oflags) != 0) { errno = EINVAL; return 0; }
    int fd = open(path, oflags, 0644);
    if (fd < 0) return 0;
    FILE *f = fdopen(fd, mode);
    if (!f) { close(fd); return 0; }
    f->flags |= FILE_ALLOC;
    return f;
}

int fclose(FILE *f) {
    if (!f) return EOF;
    fflush(f);
    int fd = f->fd;
    stream_unregister(f);
    int alloc = f->flags & FILE_ALLOC;
    if (alloc) free(f);
    return (close(fd) == 0) ? 0 : EOF;
}

int feof(FILE *f) { return (f->flags & FILE_EOF) ? 1 : 0; }
int ferror(FILE *f) { return (f->flags & FILE_ERR) ? 1 : 0; }
void clearerr(FILE *f) { f->flags &= ~(FILE_EOF | FILE_ERR); }
int fileno(FILE *f) { return f->fd; }

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    stdio_init_once();
    f->bufmode = mode;
    if (buf && size > 0) { f->buf = buf; f->bufsz = (int)size; f->bufpos = 0; }
    return 0;
}

/* Called by exit() (libc) to flush+close all streams before _exit. */
void __stdio_cleanup(void) {
    for (int i = 0; i < STDIO_MAX_STREAMS; i++) {
        FILE *s = s_open_streams[i];
        if (s) fflush(s);
    }
}

/* getentropy(2) — wrapper for syscall 318 */
int getentropy(void *buffer, size_t length) {
    long ret = syscall(318, (long)buffer, (long)length, 0, 0, 0, 0);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

/* Q16: getrandom(2) — Linux-compatible wrapper for syscall 319; draws from
 * the same kernel pool as getentropy. */
ssize_t getrandom(void *buf, size_t buflen, unsigned int flags) {
    if (!buf && buflen) { errno = EFAULT; return -1; }
    long ret = syscall(SYS_GETRANDOM, (long)buf, (long)buflen, (long)flags, 0, 0, 0);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return (ssize_t)ret;
}

/* ---- Q16: sig2str / str2sig (POSIX.1-2024 Issue 8) ---- */

/* The AuraLite signal set, in signal-number order.  Index i holds signal
 * i's name (index 0 unused).  Names omit the "SIG" prefix per POSIX. */
static const char *const q16_sig_names[NSIG] = {
    [0] = NULL,
    [SIGHUP]   = "HUP",    [SIGINT]  = "INT",    [SIGQUIT] = "QUIT",
    [SIGILL]   = "ILL",    [SIGTRAP] = "TRAP",   [SIGABRT] = "ABRT",
    [SIGBUS]   = "BUS",    [SIGFPE]  = "FPE",    [SIGKILL] = "KILL",
    [SIGUSR1]  = "USR1",   [SIGSEGV] = "SEGV",   [SIGUSR2] = "USR2",
    [SIGPIPE]  = "PIPE",   [SIGALRM] = "ALRM",   [SIGTERM] = "TERM",
    [SIGCHLD]  = "CHLD",   [SIGCONT] = "CONT",   [SIGSTOP] = "STOP",
    [SIGTSTP]  = "TSTP",   [SIGTTIN] = "TTIN",   [SIGTTOU] = "TTOU",
    [SIGURG]   = "URG",    [SIGXCPU] = "XCPU",   [SIGXFSZ] = "XFSZ",
    [SIGVTALRM]= "VTALRM", [SIGPROF] = "PROF",   [SIGWINCH]= "WINCH",
};

int sig2str(int signum, char *str) {
    if (!str || signum < 1 || signum >= NSIG) { errno = EINVAL; return -1; }
    const char *name = q16_sig_names[signum];
    if (!name) { errno = EINVAL; return -1; }
    size_t n = strlen(name);
    if (n >= SIG2STR_MAX) { errno = EINVAL; return -1; }
    memcpy(str, name, n + 1);
    return 0;
}

int str2sig(const char *restrict str, int *restrict signum) {
    if (!str || !signum) { errno = EINVAL; return -1; }
    const char *p = str;
    if (p[0] == 'S' && p[1] == 'I' && p[2] == 'G' && p[3] != '\0')
        p += 3;                    /* accept both "HUP" and "SIGHUP" */
    for (int s = 1; s < NSIG; s++) {
        if (q16_sig_names[s] && strcmp(p, q16_sig_names[s]) == 0) {
            *signum = s;
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

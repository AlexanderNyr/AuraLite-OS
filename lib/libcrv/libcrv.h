/* lib/libcrv/libcrv.h -- the rv64 bring-up libc (RISCV_PLAN V5).
 *
 * libc32.h's mirror at the third width: the syscalls the V5 kernel
 * implements, and the few string helpers init needs.  The full
 * lib/libc port (errno, TLS, stdio, the POSIX surface) is V8 work;
 * this header exists so init is REAL compiled-from-C U-mode code
 * today, not more hand-assembled bytes.
 *
 * Numbers are AuraLite's own (lib/libc/include/unistd.h) -- one
 * table, three trap mechanisms now (plan D4): syscall on x86_64,
 * int 0x80 on i386, ecall here.
 */

#ifndef AURALITE_LIBCRV_H
#define AURALITE_LIBCRV_H

#define SYS_READ         0
#define SYS_WRITE        1
#define SYS_GETPID      39
#define SYS_EXIT        60
#define SYS_SPAWN       81
#define SYS_SCHED_YIELD 158

long __syscall_rv(long n, long a1, long a2, long a3, long a4, long a5);

static inline long read(int fd, void *buf, unsigned long len)
{
    return __syscall_rv(SYS_READ, fd, (long)buf, (long)len, 0, 0);
}

static inline long write(int fd, const void *buf, unsigned long len)
{
    return __syscall_rv(SYS_WRITE, fd, (long)buf, (long)len, 0, 0);
}

/* Non-standard, same number at all three widths: run an initrd
 * program to completion, return its exit code. */
static inline long spawn(const char *path)
{
    return __syscall_rv(SYS_SPAWN, (long)path, 0, 0, 0, 0);
}

static inline long getpid(void)
{
    return __syscall_rv(SYS_GETPID, 0, 0, 0, 0, 0);
}

static inline void exit(int code)
{
    __syscall_rv(SYS_EXIT, code, 0, 0, 0, 0);
    __builtin_unreachable();
}

static inline long sched_yield(void)
{
    return __syscall_rv(SYS_SCHED_YIELD, 0, 0, 0, 0, 0);
}

static inline unsigned long strlen_rv(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static inline void puts_rv(const char *s)
{
    write(1, s, strlen_rv(s));
}

#endif /* AURALITE_LIBCRV_H */

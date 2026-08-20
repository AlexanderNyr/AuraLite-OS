/* lib/libca64/libca64.h -- the aarch64 bring-up libc (ARM64_PLAN A5b).
 *
 * libcrv.h's mirror at the fourth trap mechanism: the same syscalls,
 * the same AuraLite numbers (plan D4 -- one table; svc #0 here, ecall
 * on rv64, int 0x80 on i386, syscall on x86_64), the same few string
 * helpers the shared init/shell need.  The full lib/libc port is A8
 * territory; this header exists so /bina64 is REAL compiled-from-C
 * EL0 code, not hand-assembled bytes.
 */

#ifndef AURALITE_LIBCA64_H
#define AURALITE_LIBCA64_H

#define SYS_READ         0
#define SYS_WRITE        1
#define SYS_GETPID      39
#define SYS_EXIT        60
#define SYS_SPAWN       81
#define SYS_SCHED_YIELD 158

long __syscall_a64(long n, long a1, long a2, long a3, long a4, long a5);

static inline long read(int fd, void *buf, unsigned long len)
{
    return __syscall_a64(SYS_READ, fd, (long)buf, (long)len, 0, 0);
}

static inline long write(int fd, const void *buf, unsigned long len)
{
    return __syscall_a64(SYS_WRITE, fd, (long)buf, (long)len, 0, 0);
}

/* Non-standard, same number at all four ISAs: run an initrd program
 * to completion, return its exit code. */
static inline long spawn(const char *path)
{
    return __syscall_a64(SYS_SPAWN, (long)path, 0, 0, 0, 0);
}

static inline long getpid(void)
{
    return __syscall_a64(SYS_GETPID, 0, 0, 0, 0, 0);
}

static inline void exit(int code)
{
    __syscall_a64(SYS_EXIT, code, 0, 0, 0, 0);
    __builtin_unreachable();
}

static inline long sched_yield(void)
{
    return __syscall_a64(SYS_SCHED_YIELD, 0, 0, 0, 0, 0);
}

static inline unsigned long strlen_a64(const char *s)
{
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static inline void puts_a64(const char *s)
{
    write(1, s, strlen_a64(s));
}

#endif /* AURALITE_LIBCA64_H */

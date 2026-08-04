#ifndef AURALITE_LIBC_FCNTL_H
#define AURALITE_LIBC_FCNTL_H

#include <sys/types.h>   /* Q12: mode_t for openat/mkdirat varargs */

/*
 * fcntl.h — file control options (POSIX.1-2017).  Values match the
 * Linux/asm-generic ABI and the kernel-side kernel/fs/vfs.h; keep both in sync.
 *
 * NOTE: O_RDONLY is the value 0.  The access mode is a 2-bit enumerated field,
 * not a set of independent flags — extract it with O_ACCMODE and compare,
 * never `flags & O_RDONLY`.
 */

/* Access modes (the O_ACCMODE field). */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003

/* Creation / status flags. */
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_NOCTTY    0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000

/* fcntl() commands. */
#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_GETLK          5
#define F_SETLK          6
#define F_SETLKW         7
#define F_DUPFD_CLOEXEC  1030

/* File-descriptor flags (F_GETFD/F_SETFD). */
#define FD_CLOEXEC  1

/* Q12 (POSIX2024_PLAN.md phase Q12): AT_* flag constants for the AT-family
 * functions.  Values match the kernel's AT handling in syscall.c:
 *   - AT_FDCWD         -100  (openat(..., AT_FDCWD, rel) resolves vs cwd)
 *   - AT_SYMLINK_NOFOLLOW 0x100 (fstatat lstat-vs-stat)
 *   - AT_REMOVEDIR        0x200 (unlinkat rmdir-vs-unlink)
 *   - AT_EACCESS          0x200 (same value as AT_REMOVEDIR on Linux: the
 *     two flags are mutually exclusive by call site; faccessat ignores it)
 *   - AT_SYMLINK_FOLLOW   0x400 (accepted by the dispatcher, no-op today)
 *   - AT_EMPTY_PATH       0x1000 (fexecve via execveat)
 * These also live (idempotently) in <unistd.h> for Q5 compatibility. */
#ifndef AT_FDCWD
#define AT_FDCWD           -100
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_EACCESS          0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EMPTY_PATH       0x1000
#endif

/* mode argument is variadic and consulted only when O_CREAT is set. */
int open(const char *path, int flags, ...);
int creat(const char *path, int mode);
int fcntl(int fd, int cmd, ...);

#endif /* AURALITE_LIBC_FCNTL_H */

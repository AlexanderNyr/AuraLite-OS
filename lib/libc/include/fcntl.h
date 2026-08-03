#ifndef AURALITE_LIBC_FCNTL_H
#define AURALITE_LIBC_FCNTL_H

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

/* mode argument is variadic and consulted only when O_CREAT is set. */
int open(const char *path, int flags, ...);
int creat(const char *path, int mode);
int fcntl(int fd, int cmd, ...);

#endif /* AURALITE_LIBC_FCNTL_H */

#ifndef AURALITE_LIBC_UNISTD_H
#define AURALITE_LIBC_UNISTD_H

#include <stdint.h>
#include <stddef.h>

/* Syscall numbers (Linux-compatible subset + AuraLite extensions). */
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_GETPID 39
#define SYS_EXIT   60
#define SYS_LISTDIR 80   /* non-standard: list a directory */

typedef int64_t ssize_t;
typedef int64_t pid_t;

/* Generic syscall: num in the first argument, up to 6 more arguments. */
int64_t syscall(int64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5, uint64_t a6);

/* POSIX-style wrappers. */
ssize_t write(int fd, const void *buf, size_t count);
ssize_t read(int fd, void *buf, size_t count);
int     open(const char *path);
int     close(int fd);
void    _exit(int code);
pid_t   getpid(void);

/* AuraLite extension: list files in a directory path. */
void    listdir(const char *path);

#endif /* AURALITE_LIBC_UNISTD_H */

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
#define SYS_FORK   57
#define SYS_EXECVE 59
#define SYS_WAIT4  61
#define SYS_SPAWN  81   /* non-standard: spawn in new address space */
#define SYS_DNS    82   /* non-standard: resolve a hostname */
#define SYS_NET_CONNECT 83
#define SYS_NET_SEND    84
#define SYS_NET_RECV    85
#define SYS_NET_CLOSE   86
#define SYS_NET_PING    87
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
pid_t   fork(void);
int     execve(const char *path);
pid_t   wait(int *status);
pid_t   spawn(const char *path);

/* AuraLite extension: list files in a directory path. */
void    listdir(const char *path);
uint32_t dns_resolve(const char *hostname);

/* ---- Network syscalls ---- */
int     net_connect(uint32_t ip, uint16_t port);
int     net_send(const void *data, uint32_t len);
int     net_recv(void *buf, uint32_t bufsize);
int     net_close(void);
int     net_ping(uint32_t ip);

#endif /* AURALITE_LIBC_UNISTD_H */

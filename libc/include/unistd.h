#ifndef AURALITE_LIBC_UNISTD_H
#define AURALITE_LIBC_UNISTD_H

#include <stdint.h>
#include <stddef.h>

/* Syscall numbers (Linux-compatible subset + AuraLite extensions). */
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_MMAP    9
#define SYS_MUNMAP  11
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
#define SYS_MKDIR    100
#define SYS_RMDIR    101
#define SYS_UNLINK   102
#define SYS_RENAME   103
#define SYS_TRUNCATE 104
#define SYS_STAT     105
#define SYS_SOCKET         300
#define SYS_SOCKET_CONNECT 301
#define SYS_SOCKET_SEND    302
#define SYS_SOCKET_RECV    303
#define SYS_SOCKET_CLOSE   304

/* File-descriptor extensions. */
#define SYS_DUP    32
#define SYS_DUP2   33
#define SYS_PIPE   22
#define SYS_PIPE2  293
#define SYS_FCNTL  72

/* Open flags and fcntl command/FD_CLOEXEC constants live in <fcntl.h>. */

#define AF_INET      2
#define SOCK_STREAM  1

#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define PROT_EXEC    0x4
#define PROT_NONE    0x0
#define MAP_PRIVATE  0x02
#define MAP_FIXED    0x10
#define MAP_ANON     0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED   ((void *)-1)

/* Subset of struct stat we expose to user space.  Field layout must match
 * `struct vfs_stat` in the kernel (kernel/fs/vfs.h). */
struct stat {
    uint32_t st_type;
    uint32_t st_mode;
    uint64_t st_size;
    uint64_t st_inode;
    uint32_t st_nlink;
    uint32_t st_blocks;
    uint64_t st_mtime;
    uint64_t st_ctime;
    uint64_t st_atime;
};
#define ST_TYPE_FILE 1
#define ST_TYPE_DIR  2

#ifndef AURALITE_TYPE_SSIZE_T
#define AURALITE_TYPE_SSIZE_T
typedef int64_t ssize_t;
#endif
#ifndef AURALITE_TYPE_PID_T
#define AURALITE_TYPE_PID_T
typedef int64_t pid_t;
#endif

/* Generic syscall: num in the first argument, up to 6 more arguments. */
int64_t syscall(int64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5, uint64_t a6);

/* POSIX-style wrappers. */
ssize_t write(int fd, const void *buf, size_t count);
ssize_t read(int fd, void *buf, size_t count);
int     open(const char *path, int flags, ...);
int     creat(const char *path, int mode);
int     close(int fd);
void    _exit(int code);
pid_t   getpid(void);
pid_t   fork(void);
int     execve(const char *path);
pid_t   wait(int *status);
pid_t   spawn(const char *path);

/* AuraLite extension: list files in a directory path. */
void    listdir(const char *path);
int     readdir(const char *path, void *out, int max);
uint32_t dns_resolve(const char *hostname);

/* ---- Network syscalls ---- */
int     net_connect(uint32_t ip, uint16_t port);
int     net_send(const void *data, uint32_t len);
int     net_recv(void *buf, uint32_t bufsize);
int     net_close(void);
int     net_ping(uint32_t ip);

/* Socket-style network API. */
int     socket(int domain, int type, int protocol);
int     connect(int sock, uint32_t ip, uint16_t port);
int     send(int sock, const void *data, uint32_t len);
int     recv(int sock, void *buf, uint32_t bufsize);
int     closesocket(int sock);

struct dirent {
    char     name[256];
    uint32_t type;
    uint64_t size;
    uint64_t inode;
};

/* Filesystem extensions. */
int     mkdir(const char *path);
int     rmdir(const char *path);
int     unlink(const char *path);
int     rename(const char *from, const char *to);
int     truncate(const char *path, uint64_t new_size);
int     stat(const char *path, struct stat *out);

/* File-descriptor management. */
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);
int     pipe(int fds[2]);
int     pipe2(int fds[2], int flags);
/* fcntl() is declared (variadic) in <fcntl.h>. */

/* waitpid: wait for a specific child (or any if pid<0). */
pid_t   waitpid(pid_t pid, int *status);

void*   sbrk(intptr_t increment);
void*   mmap(void *addr, size_t length, int prot, int flags, int fd, uint64_t offset);
int     munmap(void *addr, size_t length);

#endif /* AURALITE_LIBC_UNISTD_H */

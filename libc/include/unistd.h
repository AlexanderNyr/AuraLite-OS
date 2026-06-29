#ifndef AURALITE_LIBC_UNISTD_H
#define AURALITE_LIBC_UNISTD_H

#include <stdint.h>
#include <stddef.h>
#include "libc/include/sys/types.h"

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
#define SYS_MEMINFO        600   /* non-standard: returns pmm_get_free_frames() to userspace */

/* File-descriptor extensions. */
#define SYS_DUP    32
#define SYS_DUP2   33
#define SYS_PIPE   22
#define SYS_PIPE2  293

/* P7: User & Group Credentials */
#define SYS_GETUID    500
#define SYS_GETEUID   501
#define SYS_GETGID    502
#define SYS_GETEGID   503
#define SYS_SETUID    504
#define SYS_SETGID    505
#define SYS_SETREUID  506
#define SYS_SETREGID  507
#define SYS_GETGROUPS 508
#define SYS_SETGROUPS 509
#define SYS_CHMOD     510
#define SYS_CHOWN     511
#define SYS_UMASK     512
#define SYS_ACCESS    513
#define SYS_FCHMOD    514
#define SYS_FCHOWN    515

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#define SYS_FCNTL  72
#define SYS_SELECT   23
#define SYS_POLL      7
#define SYS_GETCWD  540
#define SYS_CHDIR   541
#define SYS_FCHDIR  542
#define SYS_UNAME    63
#define SYS_LSEEK    8
#define SYS_IOCTL    16
#define SYS_PREAD64  17
#define SYS_PWRITE64 18
#define SYS_READV    19
#define SYS_WRITEV   20
#define SYS_SIGACTION   13
#define SYS_SIGPROCMASK 14
#define SYS_SIGRETURN   15
#define SYS_KILL        62
#define SYS_SIGPENDING 127
#define SYS_PAUSE       34
#define SYS_ALARM       37
#define SYS_SIGSUSPEND 130
#define SYS_SETPGID    109
#define SYS_GETPGID    121
#define SYS_SETSID     112
#define SYS_GETSID     124

/* lseek whence values. */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Open flags and fcntl command/FD_CLOEXEC constants live in <fcntl.h>. */

#define AF_INET      2
#define SOCK_STREAM  1

#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define PROT_EXEC    0x4
#define PROT_NONE    0x0
#define MAP_SHARED   0x01
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
    uint32_t st_uid;
    uint32_t st_gid;
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
int64_t lseek(int fd, int64_t offset, int whence);
ssize_t pread(int fd, void *buf, size_t count, int64_t offset);
ssize_t pwrite(int fd, const void *buf, size_t count, int64_t offset);
int     open(const char *path, int flags, ...);
int     creat(const char *path, int mode);
int     close(int fd);
int     isatty(int fd);
void    _exit(int code);
pid_t   getpid(void);
pid_t   fork(void);
int     execve(const char *path, char *const argv[], char *const envp[]);
int     execv(const char *path, char *const argv[]);
int     execvp(const char *file, char *const argv[]);
pid_t   wait(int *status);
pid_t   spawn(const char *path);

/* AuraLite extension: list files in a directory path.  The raw directory
 * lister is named aura_readdir() so the POSIX readdir(DIR*) in <dirent.h>
 * can own the standard name. */
void    listdir(const char *path);
int     aura_readdir(const char *path, void *out, int max);
uint32_t dns_resolve(const char *hostname);

/* ---- Network syscalls ---- */

/* H2: Memory subsystem introspection — returns pmm_get_free_frames() count. */
uint64_t get_free_frames(void);

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

/* AuraLite raw directory entry (matches the kernel's vfs_dirent layout).
 * The POSIX `struct dirent` lives in <dirent.h>. */
struct aura_dirent {
    char     name[256];
    uint32_t type;
    uint64_t size;
    uint64_t inode;
};

/* Filesystem extensions. */
int     mkdir(const char *path, mode_t mode);
int     rmdir(const char *path);
int     unlink(const char *path);
int     rename(const char *from, const char *to);
int     truncate(const char *path, uint64_t new_size);
int     stat(const char *path, struct stat *out);
int     access(const char *path, int mode);

/* P7: Credentials */
uid_t   getuid(void);
uid_t   geteuid(void);
gid_t   getgid(void);
gid_t   getegid(void);
int     setuid(uid_t uid);
int     setgid(gid_t gid);
int     setreuid(uid_t ruid, uid_t euid);
int     setregid(gid_t rgid, gid_t egid);
int     getgroups(int size, gid_t list[]);
int     setgroups(size_t size, const gid_t *list);

/* File-descriptor management. */
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);
int     pipe(int fds[2]);
int     pipe2(int fds[2], int flags);
/* fcntl() is declared (variadic) in <fcntl.h>. */

/* waitpid() is declared in <sys/wait.h> (3-arg POSIX form). */

/* Process groups / sessions (P6). */
pid_t   setsid(void);
int     setpgid(pid_t pid, pid_t pgid);
pid_t   getpgid(pid_t pid);
pid_t   getsid(pid_t pid);
pid_t   getpgrp(void);
pid_t   tcgetpgrp(int fd);
int     tcsetpgrp(int fd, pid_t pgid);

void*   sbrk(intptr_t increment);
void*   mmap(void *addr, size_t length, int prot, int flags, int fd, uint64_t offset);
int     munmap(void *addr, size_t length);

/* Working directory (P10).  select() lives in <sys/select.h>. */
char   *getcwd(char *buf, size_t size);
int     chdir(const char *path);
int     fchdir(int fd);

#endif /* AURALITE_LIBC_UNISTD_H */

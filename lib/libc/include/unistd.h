#ifndef AURALITE_LIBC_UNISTD_H
#define AURALITE_LIBC_UNISTD_H

#include <stdint.h>
#include <stddef.h>
#include "sys/types.h"

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
#define SYS_SENDTO      44
#define SYS_RECVFROM    45
#define SYS_LISTDIR 80   /* non-standard: list a directory */
#define SYS_MKDIR    100
#define SYS_RMDIR    101
#define SYS_UNLINK   102
#define SYS_RENAME   103
#define SYS_TRUNCATE 104
#define SYS_STAT     105
#define SYS_MKFIFO   106
#define SYS_FSTAT      5
#define SYS_LSTAT      6
#define SYS_SYMLINK   88
#define SYS_READLINK  89
#define SYS_SOCKET         300
#define SYS_SOCKET_CONNECT 301
#define SYS_SOCKET_SEND    302
#define SYS_SOCKET_RECV    303
#define SYS_SOCKET_CLOSE   304
#define SYS_SOCKET_BIND    305
#define SYS_SOCKET_LISTEN  306
#define SYS_SOCKET_ACCEPT  307
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
/* Q16: Issue-8 tail syscalls (kernel numbers 319..321; 318 = getentropy). */
#define SYS_GETRANDOM 319
#define SYS_PSELECT6  320
#define SYS_PPOLL     321
/* Q14: System V IPC — Linux syscall numbers. */
#define SYS_SEMGET 64
#define SYS_SEMOP  65
#define SYS_SEMCTL 66
#define SYS_MSGGET 68
#define SYS_MSGSND 69
#define SYS_MSGRCV 70
#define SYS_MSGCTL 71
#define SYS_SHMGET 29
#define SYS_SHMAT  30
#define SYS_SHMCTL 31
#define SYS_SHMDT  67
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
#define ST_TYPE_CHARDEV 3
#define ST_TYPE_SYMLINK 4
#define ST_TYPE_FIFO 5

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
/* spawnv(): like spawn(), but hands the new process an argv vector.
 * @argv is NULL-terminated; argv[0] conventionally names the program.
 * Passing NULL is identical to spawn(). */
pid_t   spawnv(const char *path, char *const argv[]);

/* AuraLite extension: list files in a directory path.  The raw directory
 * lister is named aura_readdir() so the POSIX readdir(DIR*) in <dirent.h>
 * can own the standard name. */
void    listdir(const char *path);

/* Program search path (FSLAYOUT_PLAN phase F2).
 *
 * prog_resolve() turns a command name into a path: a name containing '/' is
 * used as given, otherwise the search directories are tried in order and the
 * first existing entry wins.  Returns 1 and fills @out on success, 0 if
 * nothing was found.
 *
 * prog_path_count()/prog_path_entry() expose the list so a caller can say
 * what it searched — a bare "not found" makes the user guess. */
int         prog_resolve(const char *name, char *out, int out_len);
int         prog_path_count(void);
const char *prog_path_entry(int index);
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
int     lstat(const char *path, struct stat *out);
int     fstat(int fd, struct stat *out);
int     mkfifo(const char *path, mode_t mode);
int     symlink(const char *target, const char *linkpath);
int64_t readlink(const char *path, char *buf, size_t bufsiz);
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

/* POSIX.1-2024 version constants. */
#define _POSIX_VERSION    202405L   /* POSIX.1-2024 */
#define _POSIX2_VERSION   202405L
#define _XOPEN_VERSION    800

/* Working directory (P10).  select() lives in <sys/select.h>. */
char   *getcwd(char *buf, size_t size);
int     chdir(const char *path);
int     fchdir(int fd);

/* ---- POSIX.1-2024 sysconf / confstr / pathconf (Phase Q4) ---- */

/* _SC_* sysconf names (Linux/glibc-compatible values). */
#define _SC_ARG_MAX                0
#define _SC_CLK_TCK                2
#define _SC_NGROUPS_MAX            3
#define _SC_OPEN_MAX               4
#define _SC_STREAM_MAX             5
#define _SC_TZNAME_MAX             6
#define _SC_JOB_CONTROL            7
#define _SC_SAVED_IDS              8
#define _SC_REALTIME_SIGNALS       9
#define _SC_PRIORITY_SCHEDULING   10
#define _SC_TIMERS                11
#define _SC_ASYNCHRONOUS_IO       12
#define _SC_SEMAPHORES            21
#define _SC_SHARED_MEMORY_OBJECTS 22
#define _SC_PAGE_SIZE             30
#define _SC_PAGESIZE              30
#define _SC_PTHREAD_KEYS_MAX      46
#define _SC_THREADS               67
#define _SC_GETGR_R_SIZE_MAX      69
#define _SC_GETPW_R_SIZE_MAX      70
#define _SC_LOGIN_NAME_MAX        71
#define _SC_THREAD_STACK_MIN      75
#define _SC_NPROCESSORS_CONF      83
#define _SC_NPROCESSORS_ONLN      84
#define _SC_PHYS_PAGES            85
#define _SC_MONOTONIC_CLOCK      149
#define _SC_HOST_NAME_MAX        180

#define _CS_PATH                   0

#define _PC_PATH_MAX               4
#define _PC_NAME_MAX               3
#define _PC_PIPE_BUF               5

long   sysconf(int name);
size_t confstr(int name, char *buf, size_t len);
long   pathconf(const char *path, int name);
long   fpathconf(int fd, int name);

/* ---- POSIX.1-2024 AT-family (Phase Q5) ----
 * Q12: the same constants live in <fcntl.h> (their POSIX home).  Both
 * definitions are idempotence-guarded so either include order works and
 * no -Wmacro-redefined fires. */
#ifndef AT_FDCWD
#define AT_FDCWD           (-100)
#define AT_SYMLINK_NOFOLLOW  0x100
#define AT_REMOVEDIR         0x200
#define AT_EACCESS           0x200
#define AT_SYMLINK_FOLLOW    0x400
#define AT_EMPTY_PATH       0x1000
#endif

int    openat(int dirfd, const char *path, int flags, ...);
int    fstatat(int dfd, const char *path, struct stat *buf, int flags);
int    mkdirat(int dfd, const char *path, mode_t mode);
int    unlinkat(int dfd, const char *path, int flags);
int    renameat(int old_dfd, const char *old, int new_dfd, const char *new_);
ssize_t readlinkat(int dfd, const char *path, char *buf, size_t bufsiz);
int    fchownat(int dfd, const char *path, uid_t owner, gid_t group, int flags);
int    fchmodat(int dfd, const char *path, mode_t mode, int flags);
int    faccessat(int dfd, const char *path, int mode, int flags);
int    dup3(int oldfd, int newfd, int flags);
int    fexecve(int fd, char *const argv[], char *const envp[]);

/* Q13: AT-family completion (POSIX2024_PLAN.md phase Q13). */
int     link(const char *old, const char *new);
int     linkat(int old_dfd, const char *old, int new_dfd, const char *new,
               int flags);
int     symlinkat(const char *target, int new_dfd, const char *linkpath);

/* Q11: POSIX.1-2024 new functions */
int     getentropy(void *buffer, size_t length);
int     close_range(unsigned first, unsigned last, int flags);
int     closefrom(int lowfd);

#endif /* AURALITE_LIBC_UNISTD_H */

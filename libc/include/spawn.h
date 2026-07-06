#ifndef _SPAWN_H
#define _SPAWN_H

#include <sys/types.h>
#include <signal.h>
#include <sched.h>

#define POSIX_SPAWN_RESETIDS       0x01
#define POSIX_SPAWN_SETPGROUP      0x02
#define POSIX_SPAWN_SETSIGDEF      0x04
#define POSIX_SPAWN_SETSIGMASK     0x08
#define POSIX_SPAWN_SETSCHEDPARAM  0x10
#define POSIX_SPAWN_SETSCHEDULER   0x20

typedef struct {
    int nactions;
    struct {
        int type;   /* 0=close, 1=dup2, 2=open */
        int fd;
        int fd2;
        char *path;
        int oflag;
        mode_t mode;
    } actions[16];
} posix_spawn_file_actions_t;

typedef struct {
    short flags;
    pid_t pgroup;
    sigset_t sigmask;
    sigset_t sigdefault;
    struct sched_param schedparam;
    int schedpolicy;
} posix_spawnattr_t;

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[]);

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[]);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd,
                                     const char *path, int oflag, mode_t mode);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd);

int posix_spawnattr_init(posix_spawnattr_t *attr);
int posix_spawnattr_destroy(posix_spawnattr_t *attr);
int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags);
int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags);
int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup);
int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup);
int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *sigmask);
int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *sigmask);
int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *sigdefault);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *sigdefault);

#endif /* _SPAWN_H */

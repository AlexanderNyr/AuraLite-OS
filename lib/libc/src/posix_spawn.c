/* libc/src/posix_spawn.c — posix_spawn and posix_spawnp (Phase Q9) */

#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

/* ---- File actions ---- */

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa) {
    if (!fa) return EINVAL;
    fa->nactions = 0;
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa) {
    (void)fa;
    return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd,
                                     const char *path, int oflag, mode_t mode) {
    if (!fa || fa->nactions >= 16) return ENOMEM;
    fa->actions[fa->nactions].type = 2;
    fa->actions[fa->nactions].fd = fd;
    fa->actions[fa->nactions].fd2 = 0;
    fa->actions[fa->nactions].path = (char *)path;
    fa->actions[fa->nactions].oflag = oflag;
    fa->actions[fa->nactions].mode = mode;
    fa->nactions++;
    return 0;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd) {
    if (!fa || fa->nactions >= 16) return ENOMEM;
    fa->actions[fa->nactions].type = 0;
    fa->actions[fa->nactions].fd = fd;
    fa->nactions++;
    return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int fd, int newfd) {
    if (!fa || fa->nactions >= 16) return ENOMEM;
    fa->actions[fa->nactions].type = 1;
    fa->actions[fa->nactions].fd = fd;
    fa->actions[fa->nactions].fd2 = newfd;
    fa->nactions++;
    return 0;
}

/* ---- Attributes ---- */

int posix_spawnattr_init(posix_spawnattr_t *a) {
    if (!a) return EINVAL;
    memset(a, 0, sizeof(*a));
    return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *a) {
    (void)a;
    return 0;
}

int posix_spawnattr_getflags(const posix_spawnattr_t *a, short *f) {
    if (!a || !f) return EINVAL;
    *f = a->flags;
    return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *a, short f) {
    if (!a) return EINVAL;
    a->flags = f;
    return 0;
}

int posix_spawnattr_getpgroup(const posix_spawnattr_t *a, pid_t *p) {
    if (!a || !p) return EINVAL;
    *p = a->pgroup;
    return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t *a, pid_t p) {
    if (!a) return EINVAL;
    a->pgroup = p;
    return 0;
}

int posix_spawnattr_getsigmask(const posix_spawnattr_t *a, sigset_t *s) {
    if (!a || !s) return EINVAL;
    *s = a->sigmask;
    return 0;
}

int posix_spawnattr_setsigmask(posix_spawnattr_t *a, const sigset_t *s) {
    if (!a || !s) return EINVAL;
    a->sigmask = *s;
    return 0;
}

int posix_spawnattr_getsigdefault(const posix_spawnattr_t *a, sigset_t *s) {
    if (!a || !s) return EINVAL;
    *s = a->sigdefault;
    return 0;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t *a, const sigset_t *s) {
    if (!a || !s) return EINVAL;
    a->sigdefault = *s;
    return 0;
}

/* ---- posix_spawn / posix_spawnp ---- */

extern char **environ;

static int spawn_do_exec(const char *path,
                         const posix_spawn_file_actions_t *fa,
                         const posix_spawnattr_t *attr,
                         char *const argv[], char *const envp[]) {
    /* Apply attributes */
    if (attr) {
        if (attr->flags & POSIX_SPAWN_RESETIDS) {
            setuid(getuid());
            setgid(getgid());
        }
        if (attr->flags & POSIX_SPAWN_SETPGROUP)
            setpgid(0, attr->pgroup);
        if (attr->flags & POSIX_SPAWN_SETSIGMASK)
            sigprocmask(SIG_SETMASK, &attr->sigmask, NULL);
    }

    /* Apply file actions */
    if (fa) {
        for (int i = 0; i < fa->nactions; i++) {
            int t = fa->actions[i].type;
            int fd = fa->actions[i].fd;
            if (t == 0) {
                close(fd);
            } else if (t == 1) {
                int fd2 = fa->actions[i].fd2;
                if (fd != fd2) {
                    dup2(fd, fd2);
                    close(fd);
                }
            } else if (t == 2) {
                int newfd = open(fa->actions[i].path,
                                 fa->actions[i].oflag,
                                 fa->actions[i].mode);
                if (newfd != fd) {
                    dup2(newfd, fd);
                    close(newfd);
                }
            }
        }
    }

    execve(path, argv, envp ? envp : environ);
    return errno;
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[]) {
    pid_t child = fork();
    if (child < 0) return errno;

    if (child == 0) {
        /* Child */
        int err __attribute__((unused)) = spawn_do_exec(path, fa, attr, argv, envp);
        _exit(127);
    }

    if (pid) *pid = child;
    return 0;
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[]) {
    if (!file) return ENOENT;
    if (strchr(file, '/'))
        return posix_spawn(pid, file, fa, attr, argv, envp);

    /* Search PATH */
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin";

    char buf[512];
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t seg = colon ? (size_t)(colon - p) : strlen(p);
        if (seg + 1 + strlen(file) + 1 <= sizeof(buf)) {
            memcpy(buf, p, seg);
            buf[seg] = '/';
            strcpy(buf + seg + 1, file);
            /* Try exec, return on success */
            int ret = posix_spawn(pid, buf, fa, attr, argv, envp);
            if (ret == 0) return 0;
            if (ret != ENOENT) return ret;
        }
        if (!colon) break;
        p = colon + 1;
    }
    return ENOENT;
}

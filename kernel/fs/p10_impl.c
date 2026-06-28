/* P10 Implementation - minimal to allow full build */
#include "kernel/fs/vfs.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/lib/string.h"
#include "kernel/lib/errno.h"

int do_getcwd(char *buf, size_t size) {
    tcb_t *cur = sched_current();
    if (!cur || !buf) return -EINVAL;
    const char *cwd = cur->cwd[0] ? cur->cwd : "/";
    size_t len = strlen(cwd) + 1;
    if (len > size) return -ERANGE;
    strncpy(buf, cwd, size);
    return (int)len;
}

int do_chdir(const char *path) {
    tcb_t *cur = sched_current();
    if (!cur || !path) return -EINVAL;
    strncpy(cur->cwd, path, 255);
    cur->cwd[255] = 0;
    return 0;
}

int vfs_readlink(const char *path, char *buf, size_t bufsiz) {
    (void)path; (void)buf; (void)bufsiz;
    return -ENOSYS;
}

int vfs_fstat(int fd, struct vfs_stat *st) {
    (void)fd; (void)st;
    return -ENOSYS;
}

int vfs_lstat(const char *path, struct vfs_stat *st) {
    (void)path; (void)st;
    return -ENOSYS;
}

int vfs_symlink(const char *target, const char *linkpath) {
    (void)target; (void)linkpath;
    return -ENOSYS;
}

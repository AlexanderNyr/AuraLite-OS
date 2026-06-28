#include "kernel/fs/vfs.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/lib/string.h"
#include "kernel/lib/errno.h"

int do_getcwd(char *buf, size_t size) {
    tcb_t *cur = sched_current();
    if (!cur || !buf) return -EINVAL;
    const char *cwd = cur->cwd[0] ? cur->cwd : "/";
    strncpy(buf, cwd, size);
    return 0;
}

int do_chdir(const char *path) {
    tcb_t *cur = sched_current();
    if (!cur || !path) return -EINVAL;
    strncpy(cur->cwd, path, 255);
    return 0;
}

int vfs_readlink(const char *p, char *b, size_t s) { return -ENOSYS; }
int vfs_fstat(int f, struct vfs_stat *s) { return -ENOSYS; }
int vfs_lstat(const char *p, struct vfs_stat *s) { return -ENOSYS; }
int vfs_symlink(const char *t, const char *l) { return -ENOSYS; }

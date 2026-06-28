/* kernel/fs/cwd.c — getcwd / chdir (P10) */

#include "kernel/proc/thread.h"
#include "kernel/proc/scheduler.h"
#include "kernel/fs/vfs.h"
#include "kernel/lib/string.h"
#include "kernel/lib/errno.h"
#include "kernel/proc/usercopy.h"

int do_getcwd(char *buf, size_t size) {
    tcb_t *cur = sched_current();
    if (!cur) return -EINVAL;

    size_t len = strlen(cur->cwd) + 1;
    if (len > size) return -ERANGE;

    if (copy_to_user(buf, cur->cwd, len) != 0)
        return -EFAULT;

    return (int)len;
}

int do_chdir(const char *path) {
    tcb_t *cur = sched_current();
    if (!cur) return -EINVAL;

    char kpath[VFS_PATH_MAX];
    if (copy_string_from_user(kpath, path, VFS_PATH_MAX) != 0)
        return -EFAULT;

    /* Простая проверка существования */
    struct vfs_stat st;
    if (vfs_stat(kpath, &st) != 0)
        return -ENOENT;

    if (st.type != VFS_TYPE_DIR)
        return -ENOTDIR;

    strncpy(cur->cwd, kpath, VFS_PATH_MAX - 1);
    cur->cwd[VFS_PATH_MAX - 1] = 0;
    return 0;
}

int do_fchdir(int fd) {
    (void)fd;
    /* Упрощённая реализация — можно расширить через vnode */
    return -ENOSYS;
}
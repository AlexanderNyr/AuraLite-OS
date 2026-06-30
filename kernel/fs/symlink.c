/* kernel/fs/symlink.c — baseline in-memory symlink, readlink, fstat, lstat.
 *
 * This layer provides POSIX-style symbolic links for the VFS while persistent
 * per-filesystem symlink storage is still future work.  Links are stored in a
 * small kernel registry keyed by absolute path.  stat/open follow final links
 * via vfs.c; lstat/readlink inspect the link object itself.
 */

#include "kernel/fs/vfs.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/string.h"
#include "kernel/mm/slab.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"

#define VFS_MAX_SYMLINKS 32
#define VFS_SYMLINK_TARGET_MAX VFS_PATH_MAX

struct symlink_node {
    int in_use;
    char path[VFS_PATH_MAX];
    char target[VFS_SYMLINK_TARGET_MAX];
    struct vnode *vn;
};

static struct symlink_node symlinks[VFS_MAX_SYMLINKS];

/* Fill a struct vfs_stat from a vnode, mirroring vfs_stat()'s default path. */
static int stat_from_vnode(struct vnode *vn, struct vfs_stat *out) {
    if (!vn || !out) return -EFAULT;
    if (vn->ops && vn->ops->stat) {
        int r = vn->ops->stat(vn, out);
        return r < 0 ? -EIO : 0;
    }
    memset(out, 0, sizeof(*out));
    out->type  = vn->type;
    out->mode  = vn->mode ? vn->mode : (vn->type == VFS_TYPE_DIR ? 0755 : 0644);
    out->uid   = vn->uid;
    out->gid   = vn->gid;
    out->size  = vn->size;
    out->inode = vn->inode_id;
    out->nlink = 1;
    out->mtime = vn->mtime;
    out->ctime = vn->ctime;
    out->atime = vn->atime;
    return 0;
}

static int find_symlink(const char *path) {
    if (!path) return -1;
    for (int i = 0; i < VFS_MAX_SYMLINKS; i++) {
        if (symlinks[i].in_use && strcmp(symlinks[i].path, path) == 0) return i;
    }
    return -1;
}

int vfs_symlink_lookup(const char *path, char *target, size_t target_len) {
    int idx = find_symlink(path);
    if (idx < 0) return -ENOENT;
    if (target && target_len > 0) {
        strncpy(target, symlinks[idx].target, target_len - 1);
        target[target_len - 1] = '\0';
    }
    return 0;
}

struct vnode *vfs_symlink_vnode(const char *path) {
    int idx = find_symlink(path);
    return idx >= 0 ? symlinks[idx].vn : NULL;
}

int vfs_unlink_symlink(const char *path) {
    int idx = find_symlink(path);
    if (idx < 0) return -ENOENT;
    if (symlinks[idx].vn) slab_free(vnode_cache, symlinks[idx].vn);
    memset(&symlinks[idx], 0, sizeof(symlinks[idx]));
    return 0;
}

int vfs_symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath || linkpath[0] != '/' || target[0] == '\0') return -EINVAL;
    if (find_symlink(linkpath) >= 0) return -EEXIST;
    struct vfs_stat st;
    if (vfs_stat(linkpath, &st) == 0) return -EEXIST;

    int slot = -1;
    for (int i = 0; i < VFS_MAX_SYMLINKS; i++) {
        if (!symlinks[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -ENOSPC;

    struct vnode *vn = slab_alloc(vnode_cache);
    if (!vn) return -ENOMEM;
    memset(vn, 0, sizeof(*vn));
    strncpy(vn->name, linkpath, VFS_PATH_MAX - 1);
    vn->type = VFS_TYPE_SYMLINK;
    vn->mode = 0777;
    vn->size = strlen(target);
    vfs_stamp_created(vn);
    tcb_t *cur = sched_current();
    if (cur) { vn->uid = cur->euid; vn->gid = cur->egid; }

    symlinks[slot].in_use = 1;
    strncpy(symlinks[slot].path, linkpath, VFS_PATH_MAX - 1);
    strncpy(symlinks[slot].target, target, VFS_SYMLINK_TARGET_MAX - 1);
    symlinks[slot].vn = vn;
    return 0;
}

int vfs_readlink(const char *path, char *buf, size_t bufsiz) {
    if (!path || !buf) return -EFAULT;
    if (bufsiz == 0) return -EINVAL;
    int idx = find_symlink(path);
    if (idx < 0) return -EINVAL;
    size_t len = strlen(symlinks[idx].target);
    if (len > bufsiz) len = bufsiz;
    memcpy(buf, symlinks[idx].target, len);
    vfs_stamp_accessed(symlinks[idx].vn);
    return (int)len;
}

int vfs_link(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    return -ENOSYS;
}

int vfs_fstat(int fd, struct vfs_stat *st) {
    if (!st) return -EFAULT;
    struct vnode *vn = vfs_get_vnode(fd);
    if (!vn) return -EBADF;
    return stat_from_vnode(vn, st);
}

int vfs_lstat(const char *path, struct vfs_stat *st) {
    struct vnode *link = vfs_symlink_vnode(path);
    if (link) return stat_from_vnode(link, st);
    return vfs_stat(path, st);
}

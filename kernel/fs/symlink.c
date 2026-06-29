/* kernel/fs/symlink.c — symlink, readlink, link, fstat, lstat (P10)
 *
 * The current VFS does not yet support persistent symlink objects, so the
 * symlink/readlink/link operations are honest stubs that return -ENOSYS.
 * fstat/lstat are fully wired to the real VFS API (vfs_get_vnode / vfs_stat).
 */

#include "kernel/fs/vfs.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/string.h"
#include "kernel/proc/usercopy.h"

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
    return 0;
}

int vfs_symlink(const char *target, const char *linkpath) {
    (void)target; (void)linkpath;
    /* Persistent symlink creation is not implemented yet (see TODO.md). */
    return -ENOSYS;
}

int vfs_readlink(const char *path, char *buf, size_t bufsiz) {
    (void)path; (void)buf; (void)bufsiz;
    /* No symlink objects exist to read back yet. */
    return -EINVAL;
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
    /* Without symlink objects, lstat is identical to stat. */
    return vfs_stat(path, st);
}

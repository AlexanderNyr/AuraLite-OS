/* kernel/fs/symlink.c — symlink, readlink, link, fstat, lstat (P10) */

#include "kernel/fs/vfs.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/string.h"
#include "kernel/proc/usercopy.h"

int vfs_symlink(const char *target, const char *linkpath) {
    /* Упрощённая реализация: создаём vnode типа SYMLINK */
    struct vnode *vn = vfs_lookup(linkpath);
    if (vn) return -EEXIST;

    /* В реальной реализации — создаём через create() + специальный тип */
    return -ENOSYS;   /* stub для быстрой интеграции */
}

int vfs_readlink(const char *path, char *buf, size_t bufsiz) {
    struct vnode *vn = vfs_lookup(path);
    if (!vn || vn->type != VFS_TYPE_SYMLINK) return -EINVAL;

    /* stub: возвращаем target как содержимое */
    size_t len = strlen((char*)vn->fs_data);
    if (len > bufsiz) len = bufsiz;
    copy_to_user(buf, vn->fs_data, len);
    return (int)len;
}

int vfs_link(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    return -ENOSYS;
}

int vfs_fstat(int fd, struct vfs_stat *st) {
    struct ofd *o = vfs_get_ofd(fd);
    if (!o || !o->vn) return -EBADF;
    return vfs_stat_by_vnode(o->vn, st);
}

int vfs_lstat(const char *path, struct vfs_stat *st) {
    /* Для простоты — сейчас не следуем symlink */
    return vfs_stat(path, st);
}
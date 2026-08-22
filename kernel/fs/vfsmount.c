/* vfsmount.c — see vfsmount.h.  Bodies moved VERBATIM from vfs.c
 * (vfs_mount / find_mount, PARITY-era text); the only change is the
 * names and the exported table accessors. */

#include "kernel/fs/vfsmount.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"

static struct vfs_mount mounts[VFS_MAX_MOUNTS];

int vfsm_mount(const char *path, const struct vfs_ops *ops,
               void *fs_data)
{
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) {
            strncpy(mounts[i].mount_path, path, VFS_PATH_MAX - 1);
            mounts[i].ops      = ops;
            mounts[i].fs_data  = fs_data;
            mounts[i].in_use   = 1;
            kprintf("[vfs] mounted '%s'\n", path);
            return 0;
        }
    }
    return -ENOSPC;   /* mount table full */
}

int vfsm_find(const char *path, const char **out_rel)
{
    if (path[0] != '/') return -1;
    int best_mount = -1;
    size_t best_len = 0;
    size_t path_len = strlen(path);
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) continue;
        size_t mlen = strlen(mounts[i].mount_path);
        if (mlen > path_len) continue;
        if (memcmp(mounts[i].mount_path, path, mlen) == 0) {
            /* Avoid matching /tmpfile to /tmp. Root is a special prefix. */
            if (mlen > 1 && path[mlen] != '\0' && path[mlen] != '/') continue;
            if (mlen > best_len) {
                best_len = mlen;
                best_mount = i;
            }
        }
    }
    if (best_mount < 0) return -1;
    const char *rel = path + best_len;
    if (*rel == '/') rel++;
    if (out_rel) *out_rel = rel;
    return best_mount;
}

struct vnode *vfsm_lookup(const char *path)
{
    const char *rel = 0;
    int m = vfsm_find(path, &rel);
    if (m < 0) return 0;
    if (!mounts[m].ops || !mounts[m].ops->lookup) return 0;
    return mounts[m].ops->lookup(mounts[m].fs_data, rel);
}

int vfsm_slots(void)
{
    return VFS_MAX_MOUNTS;
}

const struct vfs_mount *vfsm_get(int idx)
{
    if (idx < 0 || idx >= VFS_MAX_MOUNTS || !mounts[idx].in_use)
        return 0;
    return &mounts[idx];
}

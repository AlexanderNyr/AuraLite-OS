/* vfsmount.c — see vfsmount.h.  Bodies moved VERBATIM from vfs.c
 * (vfs_mount / find_mount, PARITY-era text); the only change is the
 * names and the exported table accessors. */

#include "kernel/fs/vfsmount.h"
#include "kernel/lib/errno.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"

static struct vfs_mount mounts[VFS_MAX_MOUNTS];

/* FSMODERN M3: the name of a durability class, for receipts. */
const char *fs_durability_name(enum fs_durability d)
{
    switch (d) {
    case FS_DURABILITY_EXACT:    return "EXACT";
    case FS_DURABILITY_WEAK:     return "WEAK";
    case FS_DURABILITY_VOLATILE: return "VOLATILE";
    }
    return "UNKNOWN";
}

int vfsm_mount(const char *path, const struct vfs_ops *ops,
               void *fs_data)
{
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) {
            strncpy(mounts[i].mount_path, path, VFS_PATH_MAX - 1);
            mounts[i].ops      = ops;
            mounts[i].fs_data  = fs_data;
            mounts[i].in_use   = 1;

            /* FSMODERN M3: the mount receipt names the durability class.
             *
             * The plan's wording is "the mount receipt says so", and it
             * matters that this is the MESSAGE and not a debug line: a user
             * who runs `mount` or reads the boot log should be able to see
             * that /btrfs will lose a recent write and /fat will not,
             * without reading driver source.  Only WEAK is called out per
             * mount; EXACT is the default and printing it for nine mounts
             * would train people to skip the line. */
            if (ops && ops->durability == FS_DURABILITY_WEAK)
                kprintf("[vfs] mounted '%s' -- DURABILITY WEAK: a write that "
                        "returned success may be lost on a power cut; call "
                        "sync before relying on it\n", path);
            else if (ops && ops->durability == FS_DURABILITY_VOLATILE)
                /* Deliberately NOT "call sync": on an in-memory filesystem
                 * sync succeeds and changes nothing about a power cut, and
                 * a receipt that advised it would send a user looking for a
                 * fix that does not exist.  What is true is that the data is
                 * as safe as the RAM holding it. */
                kprintf("[vfs] mounted '%s' -- DURABILITY VOLATILE: this "
                        "filesystem lives in RAM; nothing on it survives a "
                        "power cut, by design (sync cannot change that)\n",
                        path);
            else
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

struct vnode *vfsm_create(const char *path)
{
    const char *rel = 0;
    int m = vfsm_find(path, &rel);
    if (m < 0) return 0;
    if (!mounts[m].ops || !mounts[m].ops->create) return 0;
    return mounts[m].ops->create(mounts[m].fs_data, rel);
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

/* FSMODERN M1: give every mounted filesystem its own sync().
 *
 * SYS_SYNC used to call fs_cache_sync() and stop there.  That is correct for
 * a filesystem whose only pending state is dirty cache buffers -- which is
 * every driver that writes through the cache.  It is NOT correct for a
 * filesystem that keeps its tree in RAM and persists it on demand: btrfs
 * does exactly that (bm.tree_root_lba lives in memory and btrfs_sync()
 * writes it to the superblock), so `sync` flushed the DATA blocks and never
 * the tree that indexes them.  Measured: write a file, read it back in the
 * same boot (works -- RAM), power off, boot again -- the file was gone.
 *
 * The fix belongs here rather than in the syscall: "sync" must mean the
 * whole VFS, and any filesystem mounted tomorrow gets it for free by
 * implementing .sync (or ignoring it, since a NULL .sync is legal). */
/* FSMODERN M3: which mount owns a vnode, by identity of its ops table.
 *
 * The vnode does not carry a pointer to its mount (mount_data exists in the
 * struct but nothing ever fills it -- a grep for it across the tree returns
 * only this file's own use).  Rather than thread a mount pointer through
 * every driver's lookup/create, find the mount whose ops table matches: a
 * vnode's ops IS its filesystem's table, so equality identifies the owner
 * exactly.  If several mounts share a table (two exFAT volumes), any of
 * them answers .sync correctly for the shared cache, and the per-inode
 * .fsync path is unaffected because it takes the vnode.
 *
 * Returns the mount index, or -1. */
int vfsm_owner_of(const struct vnode *vn)
{
    if (!vn || !vn->ops) return -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].in_use && mounts[i].ops == vn->ops) return i;
    }
    return -1;
}

/* FSMODERN M3: run .sync for the filesystem that owns `vn`.
 *
 * The three non-errno answers are POSITIVE so they can never collide with a
 * driver's negative errno: an earlier draft returned -1 for "no owner" AND
 * -2 for "no .sync" AND let a FAILING .sync also become -1, so vfs_fsync
 * could not tell "nothing to do" from "the flush failed" and reported
 * success for a failed sync.  tests/unit/test_fsync_m3.c fails on exactly
 * that confusion (t_mount_wide_fallback), which is why the values are not
 * errnos any more. */
int vfsm_sync_owner_of(const struct vnode *vn)
{
    int i = vfsm_owner_of(vn);
    if (i < 0) return VFSM_SYNC_NO_OWNER;              /* 1 */
    if (!mounts[i].ops || !mounts[i].ops->sync)
        return VFSM_SYNC_NO_HOOK;                      /* 2 */

    int r = mounts[i].ops->sync(mounts[i].fs_data);
    if (r == 0) return 0;
    /* A bare -1 is the historical "generic failure" drivers still return;
     * callers must not mistake it for one of the sentinels above, and it is
     * not an errno a user can act on, so it becomes EIO. */
    return (r < 0 && r != -1) ? r : -EIO;
}

int vfs_sync_all(void)
{
    int synced = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].in_use || !mounts[i].ops) continue;
        if (mounts[i].ops->sync) {
            if (mounts[i].ops->sync(mounts[i].fs_data) == 0) synced++;
        }
    }
    return synced;
}

/* kernel/fs/vfsmount.h — the mount table + path→vnode resolve core
 * (RESIDUE_PLAN.md R2, RES-06/07/09).
 *
 * Split out of vfs.c so the PORTABLE half of the VFS — the mount
 * table, longest-prefix matching, and lookup across mounts — links
 * on every architecture, while vfs.c keeps the fd/OFD/pipe
 * machinery that is honestly coupled to the x86_64 thread layer
 * (tcb fd tables, wait queues, signals).  One copy of the mount
 * logic: vfs.c DELEGATES here, the ports call here directly.
 *
 * Pure by the blkdev.c rule: no allocation, no locking (mounts
 * happen at boot, single-threaded on every port today — the moment
 * that stops being true this file grows the lock, not the callers).
 */

#ifndef AURALITE_KERNEL_FS_VFSMOUNT_H
#define AURALITE_KERNEL_FS_VFSMOUNT_H

#include "kernel/fs/vfs.h"

/* Register a mount.  Prints the '[vfs] mounted' receipt.  Returns
 * 0 or -ENOSPC. */
int vfsm_mount(const char *path, const struct vfs_ops *ops,
               void *fs_data);

/* Longest-prefix mount match.  Returns the mount index (>= 0) and
 * the path remainder after the mount point, or -1. */
int vfsm_find(const char *path, const char **out_rel);

/* Resolve an absolute path to a vnode across the mounts (no symlink
 * following, no fifo overlay — those stay vfs.c's, where the
 * machinery for them lives). */
struct vnode *vfsm_lookup(const char *path);

/* R6: create a regular file through the mounts (ops->create), for
 * the ports' O_CREAT lane.  NULL when no mount matches or the fs
 * cannot create. */
struct vnode *vfsm_create(const char *path);

/* FSMODERN M1: call .sync on every mounted filesystem that has one.
 * Returns how many answered success.  SYS_SYNC calls this AFTER flushing
 * the buffer cache, because a cache flush cannot persist a filesystem whose
 * index lives in RAM (btrfs) -- only its own sync can. */
int vfs_sync_all(void);

/* FSMODERN M3: the mount that owns a vnode (by ops identity), and its
 * .sync.  See vfsmount.c for why identity works where mount_data does not.
 *
 * vfsm_sync_owner_of() answers with POSITIVE sentinels, never with a
 * sentinel that could be an errno:
 *   VFSM_SYNC_NO_OWNER  no mount owns this vnode -- nothing to flush
 *   VFSM_SYNC_NO_HOOK   the owner has no .sync -- caller reports EOPNOTSUPP
 *   0                   .sync ran and succeeded
 *   negative errno      .sync ran and failed (a bare -1 becomes -EIO)
 */
#define VFSM_SYNC_NO_OWNER  1
#define VFSM_SYNC_NO_HOOK   2
int vfsm_owner_of(const struct vnode *vn);
int vfsm_sync_owner_of(const struct vnode *vn);

/* Table access for vfs.c's reverse mapping (vnode → path). */
int vfsm_slots(void);
const struct vfs_mount *vfsm_get(int idx);

#endif /* AURALITE_KERNEL_FS_VFSMOUNT_H */

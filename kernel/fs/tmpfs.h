#ifndef AURALITE_FS_TMPFS_H
#define AURALITE_FS_TMPFS_H

#include "kernel/fs/vfs.h"

/* Simple writable in-memory filesystem.  Three volumes are mounted: /tmp for
 * scratch, /opt for installed packages (FSLAYOUT_PLAN phase F1), and /dev/shm
 * for POSIX shared memory and named-semaphore files (POSIX2024_PLAN.md Q12).
 * They have separate file tables so that scratch traffic cannot evict or
 * crowd out an installed program, and separate ops tables so that
 * vfs_vnode_path() can tell one mount from the other. */
void tmpfs_init(void);
void tmpfs_list(void);
void optfs_list(void);
void tmpfs_self_test(void);

/* The fs_data pointers to hand to vfs_mount(). */
void *tmpfs_volume_tmp(void);
void *tmpfs_volume_opt(void);
void *tmpfs_volume_shm(void);

extern const struct vfs_ops tmpfs_ops;
extern const struct vfs_ops optfs_ops;
extern const struct vfs_ops shmfs_ops;

#endif /* AURALITE_FS_TMPFS_H */

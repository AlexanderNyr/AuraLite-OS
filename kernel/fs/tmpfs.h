#ifndef AURALITE_FS_TMPFS_H
#define AURALITE_FS_TMPFS_H

#include "kernel/fs/vfs.h"

/* Simple writable in-memory filesystem mounted at /tmp. */
void tmpfs_init(void);
void tmpfs_list(void);
void tmpfs_self_test(void);

extern const struct vfs_ops tmpfs_ops;

#endif /* AURALITE_FS_TMPFS_H */

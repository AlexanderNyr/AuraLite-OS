#ifndef AURALITE_FS_PROCFS_H
#define AURALITE_FS_PROCFS_H

#include "kernel/fs/vfs.h"

/* /proc virtual filesystem. */

extern const struct vfs_ops procfs_ops;

void procfs_init(void);

#endif /* AURALITE_FS_PROCFS_H */

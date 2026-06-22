#ifndef AURALITE_FS_DISKFS_H
#define AURALITE_FS_DISKFS_H

#include "kernel/fs/vfs.h"

/* Tiny persistent AHCI-backed read/write filesystem mounted at /disk.
 * Limits: 8 flat files, 4 KiB per file. Intended as a simple persistence layer
 * and AHCI block-I/O demonstration, not a production filesystem. */
int diskfs_init(void);
void diskfs_list(void);
void diskfs_self_test(void);

extern const struct vfs_ops diskfs_ops;

#endif /* AURALITE_FS_DISKFS_H */

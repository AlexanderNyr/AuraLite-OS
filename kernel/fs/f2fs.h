#ifndef AURALITE_FS_F2FS_H
#define AURALITE_FS_F2FS_H

#include <stdint.h>
#include "kernel/fs/vfs.h"

/*
 * f2fs.h — Flash-Friendly File System (F2FS) driver.
 *
 * F2FS is a log-structured filesystem designed for NAND flash storage.
 * Key characteristics:
 *   - Segmented layout: fixed-size segments (2MB default) written sequentially
 *   - Cleaning mechanism migrates live blocks to reclaim free segments
 *   - Node/Data separation: metadata nodes vs file data blocks
 *   - Checkpoint consistency: atomic checkpoint via double-buffering
 *   - Multi-head logging: hot/cold node/data separation
 *
 * On-disk layout:
 *   Block 0          — Boot sector (unused on disk, 0xF2F20210 magic)
 *   Block 1-2        — Superblock (primary + backup copy)
 *   Block 3..N       — Checkpoint (current + previous)
 *   Block N+1..      — Segment Summary Area (SSA)
 *   Block M..        — Main area (node + data segments)
 *
 * Mount point: /f2fs — see kernel.c.
 */

int  f2fs_init(int prefer_port);
void f2fs_self_test(void);

extern const struct vfs_ops f2fs_ops;

#endif /* AURALITE_FS_F2FS_H */
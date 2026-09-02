#ifndef AURALITE_FS_F2FS_H
#define AURALITE_FS_F2FS_H

#include <stdint.h>
#include "kernel/fs/vfs.h"

/*
 * f2fs.h — Flash-Friendly File System (F2FS) driver.
 *
 * F2FS is a log-structured filesystem designed for NAND flash storage.
 * This driver implements the real on-disk structures and a full mutation
 * surface:
 *   - NAT / SIT / SSA tables and the main node+data segment area;
 *   - checkpoint consistency (current + previous checkpoint);
 *   - segment allocation from a free-segment list;
 *   - an internal structural fsck (f2fs_fsck, F4);
 *   - lookup, create, read, write, readdir, mkdir, rmdir, unlink, rename,
 *     link, settimes, truncate, stat, sync (segment flush + checkpoint).
 *
 * On-disk layout:
 *   Block 0          — Boot sector (unused on disk, 0xF2F20210 magic)
 *   Block 1-2        — Superblock (primary + backup copy)
 *   Block 3..N       — Checkpoint (current + previous)
 *   Block N+1..      — Segment Summary Area (SSA)
 *   Block M..        — Main area (node + data segments)
 *
 * Honest scope notes (FSFULL_PLAN.md F4/F6):
 *   - background/foreground cleaning and hot/cold "multi-head logging"
 *     are not implemented; segments are allocated from a free list.
 *   - the driver never needs flash-level garbage collection because the
 *     harness volumes are sized small and reclaimed by free-segment reuse.
 *
 * Mount point: /f2fs — see kernel.c.
 */

int  f2fs_init(int prefer_port);
int  f2fs_self_test(void);
int  f2fs_fsck(void);       /* internal structural fsck (F4); 0 if consistent */

extern const struct vfs_ops f2fs_ops;

#endif /* AURALITE_FS_F2FS_H */

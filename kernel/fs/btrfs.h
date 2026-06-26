#ifndef AURALITE_FS_BTRFS_H
#define AURALITE_FS_BTRFS_H

#include <stdint.h>
#include "kernel/fs/vfs.h"

/*
 * btrfs.h — Copy-on-write (CoW) Btrfs-style filesystem driver.
 *
 * Capabilities:
 *   - Copy-on-write: every block write allocates a new block (never in-place)
 *   - Checksums for data integrity (CRC32C per block, simplified)
 *   - B-tree metadata: tree root, extent tree, FS tree
 *   - Subvolumes and snapshots (basic root items)
 *   - Extent items for file data allocation
 *   - Directory items with name lookup
 *   - Inode items for file metadata
 *
 * On-disk layout (LBA-based):
 *   LBA 65536 (64KB)  — Superblock (4KB)
 *   LBA 131072 (128KB) — Tree Root (B-tree root of all metadata)
 *   LBA 192608 (192KB) — Chunk Tree (device/chunk allocation)
 *   Main area: CoW node and data blocks allocated from free space
 *
 * Key IDs:
 *   1 = ROOT_DIR (root directory)
 *   2 = EXTENT_TREE (extent allocation tree)
 *   3 = ROOT_TREE (subvolume/snapshot tree)
 *   4 = CHUNK_TREE (device allocation)
 *   5 = FS_TREE (main filesystem tree)
 *
 * Mount point: /btrfs — see kernel.c.
 */

int  btrfs_init(int prefer_port);
int btrfs_self_test(void);

extern const struct vfs_ops btrfs_ops;

#endif /* AURALITE_FS_BTRFS_H */
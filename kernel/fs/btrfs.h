#ifndef AURALITE_FS_BTRFS_H
#define AURALITE_FS_BTRFS_H

#include <stdint.h>
#include "kernel/fs/vfs.h"

/*
 * btrfs.h — Copy-on-write (CoW) Btrfs-style filesystem driver.
 *
 * Implemented (full mutation surface through the VFS):
 *   - Copy-on-write: every block write allocates a new block (never in-place),
 *     with tree re-parenting on update;
 *   - SHA-256 checksums on every block (32-byte trailer, RESIDUE2 T3),
 *     computed on write and verified on read (CRC32C at F4b, upgraded);
 *   - B-tree metadata: tree root, extent tree and the FS tree;
 *   - extent items for file data allocation;
 *   - directory items with name lookup, inode items for metadata;
 *   - lookup, create, read, write, readdir, mkdir, rmdir, unlink, rename,
 *     link, settimes, truncate, stat, sync.
 *
 * On-disk layout (LBA-based):
 *   LBA 65536 (64KB)  — Superblock (4KB)
 *   LBA 131072 (128KB) — Tree Root (B-tree root of all metadata)
 *   LBA 192608 (192KB) — Chunk Tree (device/chunk allocation)
 *   Main area: CoW node and data blocks allocated from free space
 *
 * Honest scope notes (FSFULL_PLAN.md F4b/F6):
 *   - subvolumes and snapshots are NOT implemented: the driver manages a
 *     single FS tree and does not create or expose ROOT_TREE subvolumes.
 *   - checksums are per-block SHA-256 in the block trailer (CRC32C as of
 *     F4b, upgraded by RESIDUE2 T3 with the kernel-local kernel/lib/sha256.c);
 *     there is no full Btrfs send/receive, compression or device RAID
 *     handling.
 *
 * Mount point: /btrfs — see kernel.c.
 */

int  btrfs_init(int prefer_port);
int  btrfs_self_test(void);

extern const struct vfs_ops btrfs_ops;

#endif /* AURALITE_FS_BTRFS_H */

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
 * On-disk layout (BYTE offsets, not sector LBAs):
 *   Legacy: 65536 — 4 KiB superblock; 69632+ — append-only CoW blocks.
 *   Explicit fresh-volume dual-v2: checkpoint slots at 65536 and 61440,
 *   distinct _AURA2FS magic. Highest valid generation wins; no migration.
 *   Older kernels do NOT understand v2: never enable their format gate on it.
 *   Cursor, root and volume geometry are checksum-verified before mount.
 *   No block reclamation yet: exhaustion returns ENOSPC, never wraps.
 *
 * Honest scope notes (FSFULL_PLAN.md F4b/F6):
 *   - no persistent snapshots or subvolumes; a volatile read-only root token
 *     is available to serialized kernel callers (see API below).
 *   - checksums are per-block SHA-256 in the block trailer (CRC32C as of
 *     F4b, upgraded by RESIDUE2 T3 with the kernel-local kernel/lib/sha256.c);
 *     there is no full Btrfs send/receive, compression or device RAID
 *     handling.
 *
 * Mount point: /btrfs — see kernel.c.
 */

int  btrfs_init(int prefer_port);
int  btrfs_self_test(void);

/* Volatile, serialized kernel API, NOT persistent or a userspace ABI.
 * Paths are filesystem-relative. Fields are opaque; no authorization boundary
 * against malicious kernel callers. Caller serializes with ALL FS operations.
 * At most AURAFS_MAX_SNAPSHOTS active handles. Create rejects an output address
 * already owning a live handle (no overwrite/leaked pin); output is unchanged
 * on failure and need not be initialized on first use. Keep that address
 * reserved until release. Value copies are aliases, NOT independent refs:
 * releasing any exact alias invalidates them all. Double release is harmless.
 * Every mount attempt clears registrations. IDs are not reused, even across
 * mount attempts; exhausting IDs fails closed until kernel restart.
 * No block reuse yet. Registry enumeration alone is NOT a safe collector:
 * future GC also needs the live/checkpoint roots and complete reachability.
 */
#define AURAFS_MAX_SNAPSHOTS 32
struct aurafs_snapshot {
    uint64_t root_lba;
    uint64_t allocation_end;
    uint64_t mount_epoch;
    uint64_t registration_id;
};
int aurafs_snapshot_create(struct aurafs_snapshot *snapshot);
int64_t aurafs_snapshot_read(const struct aurafs_snapshot *snapshot,
                             const char *path, uint64_t pos, void *buf,
                             uint64_t count);
void aurafs_snapshot_release(struct aurafs_snapshot *snapshot);
/* NULL/0 queries count; otherwise capacity must hold the whole set. Returns
 * count, or -1 with no output changes on error. Returns -1 when unmounted.
 * Enumerated tokens are aliases, not additional pins. No callbacks/reentry. */
int aurafs_snapshot_roots(struct aurafs_snapshot *roots, uint32_t capacity);

/* Read-only bounded diagnostic, serialized with every FS operation.
 * Includes live root, cache-visible checkpoint, registered snapshots and
 * up to 32 since-mount checkpoint candidates (exact root/end pairs).
 * History saturation disables audit until remount, NOT writes/sync. Success
 * does not discard candidates: no FLUSH/dual-super durability proof exists.
 * Max allocated address span 1 GiB; heap cap < 564 KiB. Failure leaves output
 * unchanged. Counts unreferenced append blocks, NEVER certifies them free:
 * cache-visible checkpoint is NOT proof of the durable device root. */
struct aurafs_reachability {
    uint64_t allocated_blocks, metadata_blocks, data_blocks, unreferenced_blocks;
    uint32_t roots;
};
int aurafs_reachability_audit(struct aurafs_reachability *report);

extern const struct vfs_ops btrfs_ops;

#endif /* AURALITE_FS_BTRFS_H */

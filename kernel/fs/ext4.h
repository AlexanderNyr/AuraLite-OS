#ifndef AURALITE_FS_EXT4_H
#define AURALITE_FS_EXT4_H

#include <stdint.h>
#include "kernel/fs/vfs.h"

/*
 * ext4 driver — ext4 on-disk format with extents, per-group block/inode
 * bitmaps and directory entries that carry a file type.  Mounts existing
 * Linux-mkfs.ext4 volumes and formats blank disks in-kernel.
 *
 * Implemented (full mutation surface through the VFS):
 *   - extent-based file allocation (ee_block/ee_len/ee_start extent tree);
 *   - block and inode bitmaps and group descriptors per block group;
 *   - directory entries with file type; readdir parses both linear and
 *     HTree (dx_root) indexed directories;
 *   - an internal write-ahead log/journal for crash-consistency of the
 *     metadata updates this driver makes;
 *   - lookup, create, read, write, readdir, mkdir, rmdir, unlink, rename,
 *     link, settimes, truncate, stat, sync.
 *
 * Honest scope notes (FSFULL_PLAN.md F3/F6):
 *   - "full ext4 journaling" is not claimed: the on-disk journal written
 *     here is this driver's own, and no recovery of foreign JBD2 journals
 *     is implemented.
 *   - HTree is parsed (indexed directories are readable), but writing
 *     HTree indexes is out of scope; directories this driver creates use
 *     linear entries.
 *   - delayed allocation, flex_bg and sparse_super are not implemented.
 *
 * Mount point: /ext4 — see kernel.c.
 */

int  ext4_init(int prefer_port);
int  ext4_self_test(void);

extern const struct vfs_ops ext4_ops;

#endif /* AURALITE_FS_EXT4_H */

#ifndef AURALITE_FS_EXT2_H
#define AURALITE_FS_EXT2_H

#include <stdint.h>
#include "kernel/fs/vfs.h"

/*
 * ext2 driver — read/write support for the second AHCI disk.
 *
 * Capabilities:
 *   - Mounts an existing ext2 filesystem (created by Linux `mkfs.ext2`).
 *   - Formats a fresh filesystem in-place when no ext2 magic is present
 *     (handy for boot tests against a blank QEMU drive).
 *   - Regular files: read, write, truncate, with single + double + triple
 *     indirect block addressing (so files can grow well past 12 blocks).
 *   - Directories: lookup, readdir, mkdir, rmdir, create, unlink, rename.
 *   - Block & inode bitmap allocation; superblock + group-descriptor updates
 *     are written back so the volume survives reboots.
 *
 * Limitations (intentional — keep the driver focused):
 *   - No symlinks / xattrs / journaling (this is ext2, not ext3/4).
 *   - File holes are written as zero blocks rather than sparse.
 *   - No on-disk timestamps beyond a constant mtime/ctime.
 *
 * Mount point: /ext2 — see kernel.c.
 */

/* Initialise the driver against an AHCI port; pass -1 to auto-pick the second
 * disk (or the first if no second exists).  Returns 0 on success. */
int  ext2_init(int prefer_port);
void ext2_self_test(void);

/* Lazy-print the root listing (used by `vfs_list` fall-back). */
void ext2_list(void);

extern const struct vfs_ops ext2_ops;

#endif /* AURALITE_FS_EXT2_H */

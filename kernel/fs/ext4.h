#ifndef AURALITE_FS_EXT4_H
#define AURALITE_FS_EXT4_H

#include <stdint.h>
#include "kernel/fs/vfs.h"

/*
 * ext4 driver — full journaling ext4 filesystem.
 *
 * Capabilities:
 *   - Extent-based file allocation (replaces ext2 direct/indirect blocks)
 *   - JBD2-compatible journal (write-ahead logging for consistency)
 *   - Block and inode bitmaps per block group
 *   - Sparse superblock (flex_bg layout)
 *   - Large file support (i_size_high for files > 4GB)
 *   - Delayed allocation (allocate on write, not on open)
 *   - Directory entries with file type (HTree-ready structure)
 *
 * Mount point: /ext4 — see kernel.c.
 */

int  ext4_init(int prefer_port);
void ext4_self_test(void);

extern const struct vfs_ops ext4_ops;

#endif /* AURALITE_FS_EXT4_H */
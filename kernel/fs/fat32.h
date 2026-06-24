#ifndef AURALITE_FS_FAT32_H
#define AURALITE_FS_FAT32_H

#include "kernel/fs/vfs.h"

/*
 * Full FAT32 driver.
 *
 * Features:
 *   - Standard FAT32 BPB parsing (any sectors-per-cluster, any number of FATs).
 *   - Sub-directories (read + write) via cluster chains.
 *   - Long File Names (VFAT LFN entries, UCS-2 → ASCII; reading and writing).
 *   - FSInfo sector updates (free cluster count, next-free hint).
 *   - File create/read/write/append/truncate/unlink/rename.
 *   - Directory create (mkdir) and remove (rmdir, requires empty).
 *   - 8.3 short-name aliasing alongside LFN; mtime/ctime stamped.
 *   - On-disk format mounted from AHCI port 0 at LBA 64; if absent, the driver
 *     formats a 4 MiB FAT32 superfloppy in-place.  This keeps the existing
 *     `fat32_append_log()` API intact for kernel log persistence.
 */

/* Public driver API. */
int  fat32_init(void);
void fat32_self_test(void);
int  fat32_append_log(const char *data, uint64_t len);

/* Legacy shim: prints a flat root listing.  Kept for backward compatibility
 * with the older shell.  New code should use vfs_readdir("/fat"). */
void fat32_list(void);

extern const struct vfs_ops fat32_ops;

#endif /* AURALITE_FS_FAT32_H */

#ifndef AURALITE_FS_INITRD_H
#define AURALITE_FS_INITRD_H

#include <stdint.h>

/* Shared with the host packer (tools/mkinitrd.sh); neither may silently
 * produce an archive the kernel would truncate at boot.  A self-hosted
 * kernel source closure needs more than 1024 entries. */
#define INITRD_MAX_FILES 2048
#define INITRD_MAX_DIRS  128

/*
 * USTAR (POSIX tar) initial RAM disk.
 *
 * The initrd is passed to the kernel by the BIOS or UEFI bootloader. We parse the
 * 512-byte tar headers to build an in-memory file table, then expose it via
 * the VFS as a read-only filesystem mounted at "/".
 *
 * USTAR header format (512 bytes):
 *   offset  size  field
 *   0       100   name (null-terminated path)
 *   257     6     magic ("ustar\0")
 *   124     12    size (octal string)
 *   156     1     type ('0' or '\0' = regular file)
 *   Data follows each header, padded to 512 bytes.
 */

/* Initialise the bootloader-provided initrd (address + size).
 * Returns 0 on success, -1 on an allocation failure or if the archive
 * exceeds either table limit; the caller must skip vfs_mount() on failure. */
int initrd_init(uint64_t address, uint64_t size);

/* VFS operations for the initrd. */
extern const struct vfs_ops initrd_ops;

/* Debug: list all files in the initrd. */
void initrd_list(void);

#endif /* AURALITE_FS_INITRD_H */

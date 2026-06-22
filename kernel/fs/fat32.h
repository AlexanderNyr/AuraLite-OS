#ifndef AURALITE_FS_FAT32_H
#define AURALITE_FS_FAT32_H

#include "kernel/fs/vfs.h"

/* Minimal FAT32 read/write support mounted at /fat when an AHCI disk exists.
 * The driver can format a small FAT32 superfloppy volume on the VM disk and
 * appends all kernel logs to /fat/AURALOG.TXT. */
int fat32_init(void);
void fat32_list(void);
void fat32_self_test(void);
int fat32_append_log(const char *data, uint64_t len);

extern const struct vfs_ops fat32_ops;

#endif /* AURALITE_FS_FAT32_H */

#ifndef AURALITE_FS_EXFAT_H
#define AURALITE_FS_EXFAT_H

#include "kernel/fs/vfs.h"
#include <stdint.h>

/* exFAT driver — real exFAT on-disk structures (FSFULL_PLAN.md F5).
 *
 * Implemented (full mutation surface through the VFS):
 *   - real boot region (12-sector + checksum sector + backup), verified on
 *     mount, byte-compatible with exfatprogs geometry;
 *   - FAT cluster chains and the allocation bitmap (0x81/0x82/0x83);
 *   - entry sets: primary FILE (0x85) + STREAM (0xC0) + NAME (0xC1) with the
 *     exfatprogs 16-bit entry-set checksum and ASCII-upcased name hash;
 *   - lookup, create, read, write, readdir, mkdir, rmdir, unlink, rename,
 *     settimes, truncate, stat, sync.
 *
 * Interop: host `fsck.exfat` reports kernel-formatted volumes CLEAN and the
 * kernel mounts/reads host `mkfs.exfat` volumes.
 *
 * Honest scope note (FSFULL_PLAN.md F6): the driver upcases ASCII names only;
 * UTF-16 volume labels and non-ASCII long-name upcasing are out of scope.
 *
 * Main Boot Sector (sector 0, 512 bytes).  Field offsets are the exFAT
 * spec's; `fsname` must read "EXFAT   ".  The fields the old skeleton got
 * wrong (16-bit fat_length/cluster_count that truncate the real 32-bit
 * values) are corrected here. */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct exfat_boot_sector {
    uint8_t  jump[3];              /* 0   EB 76 90 */
    uint8_t  fsname[8];            /* 3   "EXFAT   " */
    uint8_t  must_zero[53];        /* 11 */
    uint64_t partition_offset;     /* 64 */
    uint64_t volume_length;        /* 72  sectors */
    uint32_t fat_offset;           /* 80  sectors */
    uint32_t fat_length;           /* 84  sectors */
    uint32_t cluster_heap_offset;  /* 88  sectors */
    uint32_t cluster_count;        /* 92 */
    uint32_t root_first_cluster;   /* 96 */
    uint32_t volume_serial;        /* 100 */
    uint16_t fs_revision;          /* 104 */
    uint16_t volume_flags;         /* 106 */
    uint8_t  bytes_per_sector_shift;   /* 108 */
    uint8_t  sectors_per_cluster_shift;/* 109 */
    uint8_t  num_fats;             /* 110 */
    uint8_t  drive_select;         /* 111 */
    uint8_t  percent_in_use;       /* 112 */
    uint8_t  reserved[7];          /* 113 */
    uint8_t  boot_code[390];       /* 120 */
    uint16_t boot_signature;       /* 510  0x55AA */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* Entry types */
#define EXFAT_ENTRY_EOD      0x00
#define EXFAT_ENTRY_LABEL    0x03
#define EXFAT_ENTRY_BITMAP   0x81
#define EXFAT_ENTRY_UPCASE   0x82
#define EXFAT_ENTRY_GUID     0x83
#define EXFAT_ENTRY_FILE     0x85
#define EXFAT_ENTRY_STREAM   0xC0
#define EXFAT_ENTRY_NAME     0xC1

#define EXFAT_ATTR_READONLY  0x0001
#define EXFAT_ATTR_HIDDEN    0x0002
#define EXFAT_ATTR_SYSTEM    0x0004
#define EXFAT_ATTR_DIRECTORY 0x0010
#define EXFAT_ATTR_ARCHIVE   0x0020

#define EXFAT_FAT_EOF        0xFFFFFFFFu
#define EXFAT_FAT_BAD        0xFFFFFFF7u
#define EXFAT_FAT_FREE       0u

/* 0 on success (exFAT signature verified), -1 on refusal — the caller
 * mounts only on success (FSFULL_PLAN.md F1). */
int exfat_init(int device_id);
int exfat_self_test(void);
extern const struct vfs_ops exfat_ops;

#endif /* AURALITE_FS_EXFAT_H */

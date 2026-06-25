#ifndef AURALITE_FS_EXFAT_H
#define AURALITE_FS_EXFAT_H

#include "kernel/fs/vfs.h"
#include <stdint.h>

/* exFAT Main Boot Region Structures */
struct exfat_boot_region {
    uint8_t  jump[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint8_t  reserved1;
    uint16_t fat_id;
    uint16_t boot_sector_id;
    uint16_t volume_id;
    uint16_t volume_version;
    uint32_t root_dir_cluster;
    uint16_t volume_length;
    uint16_t fs_info_cluster;
    uint16_t boot_region_offset;
    uint16_t checksum;
} __attribute__((packed));

struct exfat_dir_entry {
    uint8_t  type;
    uint8_t  set_flags;
    uint16_t entry_type;
    uint8_t  name[64];
    uint64_t timestamps;
    uint32_t file_attrs;
    uint64_t size;
    uint64_t first_cluster;
} __attribute__((packed));

void exfat_init(int device_id);
extern const struct vfs_ops exfat_ops;

#endif /* AURALITE_FS_EXFAT_H */

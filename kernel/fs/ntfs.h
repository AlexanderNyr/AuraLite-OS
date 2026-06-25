#ifndef AURALITE_FS_NTFS_H
#define AURALITE_FS_NTFS_H

#include "kernel/fs/vfs.h"
#include <stdint.h>

struct ntfs_boot_sector {
    uint8_t  jump[3];
    char     oem_id[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint8_t  reserved1;
    uint16_t mft_start_cluster;
    uint16_t mft_cluster_count;
    uint16_t mft_mirror_cluster;
    uint16_t clusters_per_mft;
    uint16_t sectors_per_mft_cluster;
    uint16_t fragments;
} __attribute__((packed));

struct ntfs_mft_record {
    char     magic[4];       /* "FILE" */
    uint16_t update_seq_off;
    uint16_t update_seq_len;
    uint64_t logfile_seq_num;
    uint16_t sequence_number;
    uint16_t hard_link_count;
    uint16_t attr_offset;
    uint16_t flags;
    uint32_t used_size;
    uint32_t allocated_size;
    uint64_t base_cluster;
} __attribute__((packed));

void ntfs_init(int device_id);
extern const struct vfs_ops ntfs_ops;

#endif /* AURALITE_FS_NTFS_H */

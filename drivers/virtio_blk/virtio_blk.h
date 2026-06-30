#ifndef AURALITE_VIRTIO_BLK_H
#define AURALITE_VIRTIO_BLK_H

#include <stdint.h>

/* Synchronous sector read/write (512 bytes/sector). */
int  virtio_blk_init(void);
int  virtio_blk_available(void);
int  virtio_blk_read_sectors(uint64_t lba, uint32_t count, void *buf);
int  virtio_blk_write_sectors(uint64_t lba, uint32_t count, const void *buf);
uint64_t virtio_blk_sector_count(void);

/* VirtIO blk request header: */
typedef struct {
    uint32_t type;     /* VIRTIO_BLK_T_IN=0, VIRTIO_BLK_T_OUT=1 */
    uint32_t _reserved;
    uint64_t sector;
} __attribute__((packed)) virtio_blk_req_hdr_t;

#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_S_OK  0

#endif /* AURALITE_VIRTIO_BLK_H */

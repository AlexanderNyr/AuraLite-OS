#ifndef AURALITE_FS_BUFFER_CACHE_H
#define AURALITE_FS_BUFFER_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Block size for the cache. Matches standard disk sectors (512B) or 4KB.
 * We use 512B as the base unit to be compatible with all AHCI/USB devices. */
#define BC_BLOCK_SIZE 512
/* Total number of blocks to cache. 1024 * 512B = 512KiB.
 * For a hobby OS, this is plenty. For production, this would be larger. */
#define BC_MAX_BUFFERS 1024

struct buffer {
    uint32_t device_id;      /* ID of the disk device (e.g. AHCI port) */
    uint64_t block_num;      /* LBA / Sector number */
    uint8_t  data[BC_BLOCK_SIZE];
    
    bool     dirty;          /* True if data was modified and needs write-back */
    int      lock_count;     /* How many threads are currently using this buffer */
    
    struct buffer *prev;     /* LRU list link */
    struct buffer *next;     /* LRU list link */
};

/* Initialize the buffer cache system. */
void bc_init(void);

/* Get a block from the cache. 
 * If the block is not in cache, it's read from the physical device.
 * The returned buffer is locked and must be released with bc_release().
 * Returns NULL on device error. */
struct buffer *bc_get(uint32_t device_id, uint64_t block_num);

/* Mark the buffer as no longer needed. Decrements lock_count. */
void bc_release(struct buffer *buf);

/* Force write a dirty buffer back to disk. */
int bc_sync(struct buffer *buf);

/* Force write all dirty buffers in the cache to disk. */
void bc_flush_all(void);

/* Evict the least recently used clean buffer to make space. 
 * Returns true if a buffer was successfully evicted. */
bool bc_evict(void);

#endif /* AURALITE_FS_BUFFER_CACHE_H */

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
    uint32_t device_id;      /* blkdev id (P1; was an AHCI port before the seam) */
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

/* ============================================================================
 * F2 (FSFULL_PLAN.md) — the ONE block-I/O path for all five filesystems.
 *
 * ext4/btrfs/f2fs previously called blkdev_read/blkdev_write directly
 * (a second, cache-less I/O stack beside the cache exFAT/NTFS already
 * used), so a 1 MiB read was a fresh 512-byte sector round-trip through
 * the driver for every sector.  These helpers route N 512-byte sectors
 * through bc_get/bc_release so every read/write funnels through the
 * buffer cache: repeated reads hit the LRU, and dirty writes are
 * written back by bc_evict/bc_flush_all instead of on every store.
 *
 * `dev` is the blkdev id, `lba` the starting 512-byte sector, `count`
 * the number of 512-byte sectors (a 4 KiB FS block is count=8).
 * Return 0 on success, -1 on any cache/device error.  On a write error
 * a partially-written buffer set may be left dirty; bc_flush_all will
 * retry it, so callers should treat the error as "I/O failed", not
 * "nothing was written".
 */
int fs_read_block(int dev, uint64_t lba, uint32_t count, void *buf);
int fs_write_block(int dev, uint64_t lba, uint32_t count, const void *buf);

/* F2: the `.sync` slot for the five filesystems.  The cache has no
 * notion of which device a given mount owns, so this flushes the whole
 * cache (1024 buffers, bounded) and reports success.  Mounted where
 * vfs_ops.sync is called (fsync path) so a dirty write-through made it
 * to disk before the caller is told the flush succeeded. */
int fs_cache_sync(void *fs_data);

/* F2: cumulative cache-hit/miss counters (fed by bc_get).  Either
 * pointer may be NULL. */
void bc_get_stats(uint64_t *out_hits, uint64_t *out_misses);

#endif /* AURALITE_FS_BUFFER_CACHE_H */

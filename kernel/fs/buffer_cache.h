#ifndef AURALITE_FS_BUFFER_CACHE_H
#define AURALITE_FS_BUFFER_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* FSMODERN M2 — the cache block is now 4 KiB, not a 512-byte sector.
 *
 * The old 512-byte unit made every filesystem pay the cache's structure
 * on every sector: a 64 KiB file read cost 128 separate bc_get() walks of
 * the whole LRU list, 128 lock acquisitions and 128 device calls, and a
 * FAT cluster of 8 sectors tried to allocate 8 slots to hold what is one
 * contiguous run on disk.  The device seam has always accepted a sector
 * COUNT (one blkdev_read of 8 sectors is one AHCI command), so the cache
 * was the only thing forcing one command per 512 bytes.
 *
 * 4 KiB is the page size and the natural filesystem block.  It is NOT a
 * change to any on-disk format: filesystems still address 512-byte
 * sectors through fs_read_block/fs_write_block, which now translate
 * (lba / 8) and offset within the block.  A request whose sectors span
 * two cache blocks simply touches two of them.
 *
 * Alignment is a contract of the calls, not a hope: an unaligned
 * multi-sector request would need read-modify-write, which is a
 * corruption risk with no upside, so fs_read_block/fs_write_block fail
 * loudly on one instead. */
#define BC_BLOCK_SHIFT 12
#define BC_BLOCK_SIZE  (1u << BC_BLOCK_SHIFT)          /* 4096 */
#define BC_SECTORS_PER_BLOCK (BC_BLOCK_SIZE / 512)     /* 8 */
#define BC_BLOCK_MASK  (BC_BLOCK_SIZE - 1)

/* Total number of blocks to cache.  BC_MIN_BUFFERS * 4 KiB = 2 MiB.
 *
 * M2's acceptance says the footprint must be stated in the boot receipt
 * and derived from RAM rather than a hard 1024.  The pool is sized in
 * M2 as ~0.2% of usable RAM, clamped to [BC_MIN_BUFFERS, BC_MAX_BUFFERS]:
 * a 512 MiB machine gets 2 MiB, a 4 GiB machine gets the 8 MiB ceiling.
 * bc_init() allocates the clamped count, so BC_MAX_BUFFERS is a ceiling
 * for the static bookkeeping, not the allocation. */
#define BC_MIN_BUFFERS 512
#define BC_MAX_BUFFERS 2048

struct buffer {
    uint32_t device_id;      /* blkdev id (P1; was an AHCI port before the seam) */
    uint64_t block_num;      /* cache block number = lba / BC_SECTORS_PER_BLOCK */
    uint8_t  data[BC_BLOCK_SIZE];
    
    bool     dirty;          /* True if data was modified and needs write-back */
    int      lock_count;     /* How many threads are currently using this buffer */
    
    struct buffer *prev;     /* LRU list link */
    struct buffer *next;     /* LRU list link */
};

/* Initialize the buffer cache system. */
void bc_init(void);

/* Get a 4 KiB cache block.  `block_num` is in BC_BLOCK_SIZE units
 * (lba / BC_SECTORS_PER_BLOCK), not sectors.
 * If the block is not in cache, it is read from the physical device.
 * The returned buffer is locked and must be released with bc_release().
 * Returns NULL on device error. */
struct buffer *bc_get(uint32_t device_id, uint64_t block_num);

/* FSMODERN M2: ask for `count` CONSECUTIVE cache blocks starting at
 * `block_num`.  Fills `out` with `count` locked buffers and returns
 * `count` on success, or a negative error with nothing left locked.
 *
 * This is the whole point of the readahead: bc_get() loads one block per
 * device call, and a filesystem reading a file sequentially would pay a
 * separate round-trip for each.  The batch entry point lets the cache
 * issue ONE device read for the run it is about to hand out (the seam's
 * read() takes a sector count), so sequential file I/O becomes one
 * command per run instead of one per block.  Blocks already cached are
 * not re-read; the miss run between them is fetched with one call. */
int bc_get_many(uint32_t device_id, uint64_t block_num,
                uint32_t count, struct buffer **out, uint32_t cap);

/* FSMODERN M2: readahead hint.  Called after a sequential hit; starts a
 * speculative prefetch of the next `ahead` blocks for that device.
 * Bounded and best-effort: a prefetch never blocks a lock holder, never
 * evicts a dirty buffer to make room, and silently does nothing when the
 * cache is full of locked buffers. */
void bc_prefetch(uint32_t device_id, uint64_t block_num, uint32_t ahead);

/* Mark the buffer as no longer needed. Decrements lock_count. */
void bc_release(struct buffer *buf);

/* Force write a dirty buffer back to disk. */
int bc_sync(struct buffer *buf);

/* Force write all dirty buffers in the cache to disk. */
void bc_flush_all(void);

/* FSMODERN M4: ordered durability -- flush ONLY the dirty buffers whose
 * block number falls in [first_block, first_block + nblocks) on `dev`,
 * lowest block first, with the same adjacency coalescing as
 * bc_flush_all().  Everything outside the range stays dirty.
 *
 * Why it exists: bc_flush_all() writes the LOWEST dirty block first.  On a
 * FAT32 volume that is the FAT, which sits below the data area, so an
 * unordered whole-cache flush puts the index on the medium BEFORE the
 * clusters it names.  A power cut in that window leaves a chain pointing
 * at data the medium never received.  With this call a filesystem can
 * order its own durability:
 *
 *     bc_flush_range(dev, data_first, data_n);  // 1. the file's bytes
 *     bc_flush_range(dev, fat_first,  fat_n);   // 2. the chain that names them
 *     bc_flush_range(dev, dirent_blk, 1);       // 3. the entry that names the file
 *
 * Returns 0 when every dirty block in the range reached the device, -1 if
 * any command failed (failed buffers are left dirty for a retry). */
int bc_flush_range(int dev, uint64_t first_block, uint64_t nblocks);

/* RESIDUE2 T3 (writeback): the 1 Hz drain.  Flush up to 256 dirty
 * buffers per call (coldest first) and print the deferred/flushed
 * receipt when anything was written.  Hooked from the BSP seconds tick
 * in drivers/timer/pit.c; a hard VM kill therefore loses at most ~1 s
 * of stores.  Safe to call before bc_init() (no-op). */
void bc_tick(void);

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
 * Return 0 on success, -1 on any cache/device error.  RESIDUE2 T3:
 * writes are WRITE-BACK (the buffer is marked dirty; eviction, the
 * 1 Hz bc_tick(), bc_flush_all/fs_cache_sync and kernel_halt provide
 * durability).  On a write error a partially-written buffer set may be
 * left dirty; bc_flush_all will retry it, so callers should treat the
 * error as "I/O failed", not "nothing was written".
 */
/* FSMODERN M2: `lba` must be aligned to BC_SECTORS_PER_BLOCK (8 sectors)
 * -- every filesystem in the tree addresses 4 KiB blocks, so this holds
 * by construction; an unaligned call returns -1 rather than doing a
 * read-modify-write. */
int fs_read_block(int dev, uint64_t lba, uint32_t count, void *buf);
int fs_write_block(int dev, uint64_t lba, uint32_t count, const void *buf);

/* FSMODERN M2: the cache footprint receipt, greppable so a case can
 * assert the size was derived from RAM rather than hardcoded. */
void bc_report_footprint(void);

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

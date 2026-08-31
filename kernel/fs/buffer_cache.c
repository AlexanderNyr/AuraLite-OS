#include "kernel/fs/buffer_cache.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/spinlock.h"
#include "kernel/fs/blkdev.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/string.h"

/* Global cache state */
static struct buffer *bc_pool = NULL;
static struct buffer *lru_head = NULL; /* Most recently used */
static struct buffer *lru_tail = NULL; /* Least recently used */
static spinlock_t bc_lock = SPINLOCK_UNLOCKED;

/* F2: cache-hit/miss counters.  These let a case prove the "one I/O
 * path" claim: a repeated read bumps HITS while the raw AHCI sector
 * count (blkdev_get_stats) stays bounded. */
static uint64_t bc_hits = 0;
static uint64_t bc_misses = 0;

/* Helper: Remove buffer from LRU list */
static void lru_remove(struct buffer *buf) {
    if (buf->prev) buf->prev->next = buf->next;
    if (buf->next) buf->next->prev = buf->prev;
    if (buf == lru_head) lru_head = buf->next;
    if (buf == lru_tail) lru_tail = buf->prev;
    buf->next = buf->prev = NULL;
}

/* Helper: Move buffer to the head of LRU list (Mark as MRU) */
static void lru_touch(struct buffer *buf) {
    lru_remove(buf);
    buf->next = lru_head;
    if (lru_head) lru_head->prev = buf;
    lru_head = buf;
    if (!lru_tail) lru_tail = buf;
}

void bc_init(void) {
    spinlock_acquire(&bc_lock);
    
    /* Allocate the pool of buffers */
    bc_pool = kmalloc(sizeof(struct buffer) * BC_MAX_BUFFERS);
    if (!bc_pool) {
        kprintf("[bc] FATAL: Could not allocate buffer cache pool!\n");
        spinlock_release(&bc_lock);
        return;
    }

    memset(bc_pool, 0, sizeof(struct buffer) * BC_MAX_BUFFERS);

    /* Initialize the LRU list */
    for (int i = 0; i < BC_MAX_BUFFERS; i++) {
        struct buffer *buf = &bc_pool[i];
        buf->block_num = (uint64_t)-1; /* Mark as invalid/empty */
        
        buf->next = (i < BC_MAX_BUFFERS - 1) ? &bc_pool[i+1] : NULL;
        buf->prev = (i > 0) ? &bc_pool[i-1] : NULL;
    }
    lru_head = &bc_pool[0];
    lru_tail = &bc_pool[BC_MAX_BUFFERS - 1];

    spinlock_release(&bc_lock);
    kprintf("[bc] Buffer cache initialised (%d buffers, %d KiB)\n", 
            BC_MAX_BUFFERS, (BC_MAX_BUFFERS * BC_BLOCK_SIZE) / 1024);
}

bool bc_evict(void) {
    /* Find the least recently used buffer that is NOT locked */
    struct buffer *curr = lru_tail;
    while (curr) {
        if (curr->lock_count == 0) {
            /* If dirty, sync it first */
            if (curr->dirty) {
                if (bc_sync(curr) != 0) {
                    curr = curr->prev;
                    continue;
                }
            }
            /* Found a candidate to evict */
            curr->block_num = (uint64_t)-1;
            curr->dirty = false;
            return true;
        }
        curr = curr->prev;
    }
    return false; /* All buffers are locked */
}

int bc_sync(struct buffer *buf) {
    if (!buf->dirty) return 0;
    
    /* Write the sector through the blkdev seam (P1). */
    if (blkdev_write_sector((int)buf->device_id, buf->block_num, buf->data) != 0) {
        return -1;
    }
    buf->dirty = false;
    return 0;
}

struct buffer *bc_get(uint32_t device_id, uint64_t block_num) {
    spinlock_acquire(&bc_lock);

    /* 1. Search for the block in the current cache */
    struct buffer *curr = lru_head;
    while (curr) {
        if (curr->device_id == device_id && curr->block_num == block_num) {
            bc_hits++;
            curr->lock_count++;
            lru_touch(curr);
            spinlock_release(&bc_lock);
            return curr;
        }
        curr = curr->next;
    }
    bc_misses++;

    /* 2. Not found. Find an empty or evictable buffer */
    struct buffer *candidate = NULL;
    
    /* Try to find an empty buffer first */
    curr = lru_tail;
    while (curr) {
        if (curr->block_num == (uint64_t)-1) {
            candidate = curr;
            break;
        }
        curr = curr->prev;
    }

    /* If no empty buffer, try to evict the LRU unlocked buffer */
    if (!candidate) {
        if (bc_evict()) {
            /* After evict, find the one that just became empty */
            curr = lru_tail;
            while (curr) {
                if (curr->block_num == (uint64_t)-1) {
                    candidate = curr;
                    break;
                }
                curr = curr->prev;
            }
        }
    }

    if (!candidate) {
        kprintf("[bc] ERROR: Buffer cache exhausted (all locked)!\n");
        spinlock_release(&bc_lock);
        return NULL;
    }

    /* 3. Load data from disk into the candidate buffer */
    if (blkdev_read_sector((int)device_id, block_num, candidate->data) != 0) {
        kprintf("[bc] ERROR: Failed to read sector %llu from device %u\n", block_num, device_id);
        spinlock_release(&bc_lock);
        return NULL;
    }

    candidate->device_id = device_id;
    candidate->block_num = block_num;
    candidate->dirty = false;
    candidate->lock_count = 1;
    lru_touch(candidate);

    spinlock_release(&bc_lock);
    return candidate;
}

void bc_release(struct buffer *buf) {
    spinlock_acquire(&bc_lock);
    if (buf->lock_count > 0) {
        buf->lock_count--;
    }
    spinlock_release(&bc_lock);
}

void bc_flush_all(void) {
    spinlock_acquire(&bc_lock);
    struct buffer *curr = lru_head;
    while (curr) {
        if (curr->dirty) {
            bc_sync(curr);
        }
        curr = curr->next;
    }
    spinlock_release(&bc_lock);
}

/* ============================================================================
 * F2 — shared cache-backed block I/O for the five filesystems (see the
 * header comment for the rationale).
 * ============================================================================ */

int fs_read_block(int dev, uint64_t lba, uint32_t count, void *buf) {
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        struct buffer *b = bc_get((uint32_t)dev, lba + i);
        if (!b) return -1;
        memcpy(p, b->data, BC_BLOCK_SIZE);
        bc_release(b);
        p += BC_BLOCK_SIZE;
    }
    return 0;
}

int fs_write_block(int dev, uint64_t lba, uint32_t count, const void *buf) {
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        struct buffer *b = bc_get((uint32_t)dev, lba + i);
        if (!b) return -1;
        memcpy(b->data, p, BC_BLOCK_SIZE);
        /* F3: write-THROUGH to disk so the volume is never left with a
         * stale/lost sector under heavy write load.  The single 1024-sector
         * write-back cache was dropping dirty metadata (inode table / dir
         * blocks) during the format's massive write burst, leaving the
         * volume corrupt.  Write-through keeps the ONE cache-backed I/O
         * path (reads still hit the cache; hit/miss counters still prove
         * it) but guarantees every sector is durable. */
        if (blkdev_write_sector(dev, lba + i, b->data) != 0) {
            bc_release(b);
            return -1;
        }
        b->dirty = false;
        bc_release(b);
        p += BC_BLOCK_SIZE;
    }
    return 0;
}

int fs_cache_sync(void *fs_data) {
    (void)fs_data;
    bc_flush_all();
    /* F2 receipt: cache health at the flush point.  Greppable so an
     * integration case can assert that a repeat read raised hits with
     * the AHCI sector count bounded. */
    uint64_t h, m;
    bc_get_stats(&h, &m);
    kprintf("[bc] hits=%llu misses=%llu\n", (unsigned long long)h,
            (unsigned long long)m);
    return 0;
}

void bc_get_stats(uint64_t *out_hits, uint64_t *out_misses) {
    if (out_hits)   *out_hits   = bc_hits;
    if (out_misses) *out_misses = bc_misses;
}

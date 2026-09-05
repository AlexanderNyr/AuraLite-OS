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

/* RESIDUE2 T3 (writeback): deferred counts every store that marked a
 * buffer dirty; flushed counts every sector actually handed back to
 * the device (bc_sync).  deferred > flushed after a burst is the
 * receipt that the cache absorbed writes instead of writing through. */
static uint64_t bc_deferred = 0;
static uint64_t bc_flushed  = 0;
static uint64_t bc_coalesced = 0;

/* RESIDUE2 T3 (writeback): the receipt line shared by bc_tick() and
 * fs_cache_sync().  deferred = flushed + coalesced always: every store
 * either reached the device exactly once or was absorbed into a buffer
 * that was already dirty.  deferred > flushed therefore holds exactly
 * when the cache actually absorbed repeated stores. */
static void bc_writeback_receipt(void) {
    kprintf("[bc] writeback stores=%llu flushed=%llu coalesced=%llu\n",
            (unsigned long long)bc_deferred,
            (unsigned long long)bc_flushed,
            (unsigned long long)bc_coalesced);
}

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
    bc_flushed++;
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
        /* RESIDUE2 T3 (writeback): stores now only mark the buffer
         * dirty.  F3's write-through was correct but slow — every FAT
         * metadata update rewrote its sector even when the next store
         * would overwrite it a moment later.  Durability is provided by
         * the three flush points instead:
         *   1. bc_evict()   — a dirty buffer is synced before its slot
         *                     is reused, so a full cache can never lose
         *                     data (the failure mode F3 was guarding
         *                     against; the sync stays, only the
         *                     write-on-every-store goes away);
         *   2. bc_tick()    — the 1 Hz PIT hook drains whatever is
         *                     still dirty, which bounds data loss on a
         *                     hard QEMU kill to ~1 second;
         *   3. bc_flush_all() — fs_cache_sync (.sync) and kernel_halt.
         */
        if (b->dirty)
            bc_coalesced++;       /* absorbed into an already-dirty buffer */
        b->dirty = true;
        bc_deferred++;
        bc_release(b);
        p += BC_BLOCK_SIZE;
    }
    return 0;
}

/* RESIDUE2 T3 (writeback): the 1 Hz flush.  Called from the BSP's
 * seconds tick (drivers/timer/pit.c).  Bounded: at most
 * BC_TICK_FLUSH_BUDGET sectors per call so a format-sized burst cannot
 * stall the timer interrupt for seconds; the remainder drains on the
 * next ticks, on eviction, or at sync.  Prints the deferred/flushed
 * receipt only when the tick actually wrote something, so an idle
 * system stays quiet. */
#define BC_TICK_FLUSH_BUDGET 256
void bc_tick(void) {
    if (!bc_pool) return;
    /* We run from the BSP's seconds interrupt.  A blocking acquire here
     * would deadlock whenever the interrupted thread holds bc_lock
     * (every cache access does, briefly) — and during a write burst the
     * lock is held almost continuously.  Try-acquire and skip the tick
     * otherwise: the burst is already being drained by eviction, and
     * the next tick (or sync/halt) takes the rest. */
    if (!spinlock_try_acquire(&bc_lock))
        return;
    uint64_t before = bc_flushed;
    int budget = BC_TICK_FLUSH_BUDGET;
    struct buffer *curr = lru_tail;           /* coldest dirty first */
    while (curr && budget > 0) {
        if (curr->dirty && curr->lock_count == 0) {
            if (bc_sync(curr) != 0)
                break;                        /* device error: retry next tick */
            budget--;
        }
        curr = curr->prev;
    }
    spinlock_release(&bc_lock);
    if (bc_flushed > before)
        bc_writeback_receipt();
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
    /* RESIDUE2 T3 (writeback) receipt: deferred > flushed means the
     * cache absorbed repeated stores into the same sector (e.g. FAT
     * metadata churn) instead of writing through each one. */
    bc_writeback_receipt();
    return 0;
}

void bc_get_stats(uint64_t *out_hits, uint64_t *out_misses) {
    if (out_hits)   *out_hits   = bc_hits;
    if (out_misses) *out_misses = bc_misses;
}

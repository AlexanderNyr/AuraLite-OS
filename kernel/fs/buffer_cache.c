#include "kernel/fs/buffer_cache.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/spinlock.h"
#include "drivers/ahci/ahci.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/string.h"

/* Global cache state */
static struct buffer *bc_pool = NULL;
static struct buffer *lru_head = NULL; /* Most recently used */
static struct buffer *lru_tail = NULL; /* Least recently used */
static spinlock_t bc_lock = SPINLOCK_UNLOCKED;

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
    
    /* Call AHCI driver to write sector */
    if (ahci_write_sector(buf->device_id, buf->block_num, buf->data) != 0) {
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
            curr->lock_count++;
            lru_touch(curr);
            spinlock_release(&bc_lock);
            return curr;
        }
        curr = curr->next;
    }

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
    if (ahci_read_sector(device_id, block_num, candidate->data) != 0) {
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

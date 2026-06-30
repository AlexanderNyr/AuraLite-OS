#include "kernel/mm/page_cache.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/string.h"
#include "kernel/limine_requests.h"

#define PAGE_CACHE_BUCKETS 1024

typedef struct page_cache_entry {
    struct ofd *ofd;
    uint64_t    offset;
    uint64_t    phys;
    int         dirty;
    struct page_cache_entry *next;
} page_cache_entry_t;

static page_cache_entry_t *cache_buckets[PAGE_CACHE_BUCKETS];
static spinlock_t cache_lock = SPINLOCK_UNLOCKED;

static uint32_t hash_page(struct ofd *ofd, uint64_t offset) {
    return ((uint32_t)((uintptr_t)ofd ^ offset)) % PAGE_CACHE_BUCKETS;
}

uint64_t page_cache_get(struct ofd *file, uint64_t offset) {
    if (!file) return 0;
    uint32_t h = hash_page(file, offset);
    spinlock_acquire(&cache_lock);
    page_cache_entry_t *curr = cache_buckets[h];
    while (curr) {
        if (curr->ofd == file && curr->offset == offset) {
            spinlock_release(&cache_lock);
            return curr->phys;
        }
        curr = curr->next;
    }
    spinlock_release(&cache_lock);
    return 0;
}

void page_cache_put(struct ofd *file, uint64_t offset, uint64_t phys) {
    if (!file) return;
    uint32_t h = hash_page(file, offset);
    spinlock_acquire(&cache_lock);
    
    page_cache_entry_t *curr = cache_buckets[h];
    while (curr) {
        if (curr->ofd == file && curr->offset == offset) {
            curr->phys = phys;
            spinlock_release(&cache_lock);
            return;
        }
        curr = curr->next;
    }

    /* Not in cache, add new entry. */
    page_cache_entry_t *entry = kmalloc(sizeof(page_cache_entry_t));
    if (entry) {
        entry->ofd = file;
        entry->offset = offset;
        entry->phys = phys;
        entry->dirty = 0;
        entry->next = cache_buckets[h];
        cache_buckets[h] = entry;
    }
    spinlock_release(&cache_lock);
}

void page_cache_invalidate(struct ofd *file) {
    if (!file) return;
    spinlock_acquire(&cache_lock);
    for (int i = 0; i < PAGE_CACHE_BUCKETS; i++) {
        page_cache_entry_t **curr = &cache_buckets[i];
        while (*curr) {
            page_cache_entry_t *entry = *curr;
            if (entry->ofd == file) {
                *curr = entry->next;
                kfree(entry);
            } else {
                curr = &entry->next;
            }
        }
    }
    spinlock_release(&cache_lock);
}

void page_cache_flush(struct ofd *file) {
    /* Simple flush: iterate cache and write dirty pages back via the OFD's
     * vnode write op.  We convert the physical address to a HHDM virtual
     * pointer so the kernel can access the page contents. */
    if (!file) return;
    uint64_t hhdm = limine_get_hhdm_offset();
    spinlock_acquire(&cache_lock);
    for (int i = 0; i < PAGE_CACHE_BUCKETS; i++) {
        page_cache_entry_t *curr = cache_buckets[i];
        while (curr) {
            if (curr->ofd == file && curr->dirty) {
                if (file->vn && file->vn->ops && file->vn->ops->write) {
                    void *kbuf = (void *)(uintptr_t)(hhdm + curr->phys);
                    file->vn->ops->write(file->vn, curr->offset, kbuf, 4096);
                }
                curr->dirty = 0;
            }
            curr = curr->next;
        }
    }
    spinlock_release(&cache_lock);
}

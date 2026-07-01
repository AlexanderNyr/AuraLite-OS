#include "kernel/mm/page_cache.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/limine_requests.h"

#define PAGE_CACHE_BUCKETS 1024

typedef struct page_cache_entry {
    struct ofd *ofd;
    uint64_t    offset;
    uint64_t    phys;
    int         dirty;
    volatile int ready;
    struct page_cache_entry *next;
} page_cache_entry_t;

static page_cache_entry_t *cache_buckets[PAGE_CACHE_BUCKETS];
static spinlock_t cache_lock = SPINLOCK_UNLOCKED;

static uint32_t hash_page(struct ofd *ofd, uint64_t offset) {
    return ((uint32_t)((uintptr_t)ofd ^ offset)) % PAGE_CACHE_BUCKETS;
}

static void page_cache_wait_ready(page_cache_entry_t *entry) {
    if (!entry) return;
    while (!__atomic_load_n(&entry->ready, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("pause");
    }
}

uint64_t page_cache_get(struct ofd *file, uint64_t offset) {
    if (!file) return 0;
    uint32_t h = hash_page(file, offset);
    spinlock_acquire(&cache_lock);
    page_cache_entry_t *curr = cache_buckets[h];
    while (curr) {
        if (curr->ofd == file && curr->offset == offset) {
            uint64_t phys = curr->phys;
            page_cache_entry_t *entry = curr;
            spinlock_release(&cache_lock);
            page_cache_wait_ready(entry);
            return phys;
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
            curr->ready = 1;
            spinlock_release(&cache_lock);
            return;
        }
        curr = curr->next;
    }
    spinlock_release(&cache_lock);

    page_cache_entry_t *entry = kmalloc(sizeof(*entry));
    if (!entry) return;

    entry->ofd = file;
    entry->offset = offset;
    entry->phys = phys;
    entry->dirty = 0;
    entry->ready = 1;

    spinlock_acquire(&cache_lock);
    curr = cache_buckets[h];
    while (curr) {
        if (curr->ofd == file && curr->offset == offset) {
            curr->phys = phys;
            curr->ready = 1;
            spinlock_release(&cache_lock);
            kfree(entry);
            return;
        }
        curr = curr->next;
    }

    entry->next = cache_buckets[h];
    cache_buckets[h] = entry;
    spinlock_release(&cache_lock);
}

int page_cache_get_or_alloc(struct ofd *file, uint64_t offset,
                            uint64_t *phys_out,
                            void (*fill_fn)(uint64_t phys, void *arg),
                            void *fill_arg) {
    if (!file || !phys_out) return -1;

    uint32_t h = hash_page(file, offset);
    spinlock_acquire(&cache_lock);

    page_cache_entry_t *curr = cache_buckets[h];
    while (curr) {
        if (curr->ofd == file && curr->offset == offset) {
            uint64_t phys = curr->phys;
            page_cache_entry_t *entry = curr;
            spinlock_release(&cache_lock);
            page_cache_wait_ready(entry);
            if (pmm_inc_frame_ref(phys) != 0) return -1;
            *phys_out = phys;
            return 0;
        }
        curr = curr->next;
    }
    spinlock_release(&cache_lock);

    page_cache_entry_t *entry = kmalloc(sizeof(*entry));
    if (!entry) return -1;

    uint64_t phys = pmm_alloc_frame();
    if (!phys) {
        kfree(entry);
        return -1;
    }

    spinlock_acquire(&cache_lock);
    curr = cache_buckets[h];
    while (curr) {
        if (curr->ofd == file && curr->offset == offset) {
            uint64_t existing = curr->phys;
            page_cache_entry_t *existing_entry = curr;
            spinlock_release(&cache_lock);
            kfree(entry);
            pmm_free_frame(phys);
            page_cache_wait_ready(existing_entry);
            if (pmm_inc_frame_ref(existing) != 0) return -1;
            *phys_out = existing;
            return 0;
        }
        curr = curr->next;
    }

    entry->ofd = file;
    entry->offset = offset;
    entry->phys = phys;
    entry->dirty = 0;
    entry->ready = 0;
    entry->next = cache_buckets[h];
    cache_buckets[h] = entry;
    spinlock_release(&cache_lock);

    if (fill_fn) fill_fn(phys, fill_arg);
    __atomic_store_n(&entry->ready, 1, __ATOMIC_RELEASE);

    if (pmm_inc_frame_ref(phys) != 0) {
        spinlock_acquire(&cache_lock);
        page_cache_entry_t **pp = &cache_buckets[h];
        while (*pp) {
            if (*pp == entry) {
                *pp = entry->next;
                break;
            }
            pp = &(*pp)->next;
        }
        spinlock_release(&cache_lock);
        kfree(entry);
        pmm_free_frame(phys);
        return -1;
    }

    *phys_out = phys;
    return 0;
}

void page_cache_invalidate(struct ofd *file) {
    if (!file) return;

    page_cache_entry_t *to_free = NULL;
    spinlock_acquire(&cache_lock);
    for (int i = 0; i < PAGE_CACHE_BUCKETS; i++) {
        page_cache_entry_t **curr = &cache_buckets[i];
        while (*curr) {
            page_cache_entry_t *entry = *curr;
            if (entry->ofd == file) {
                *curr = entry->next;
                entry->next = to_free;
                to_free = entry;
            } else {
                curr = &entry->next;
            }
        }
    }
    spinlock_release(&cache_lock);

    while (to_free) {
        page_cache_entry_t *next = to_free->next;
        kfree(to_free);
        to_free = next;
    }
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
                    int64_t written = file->vn->ops->write(file->vn, curr->offset, kbuf, 4096);
                    if (written == 4096) {
                        curr->dirty = 0;
                    } else {
                        kprintf("[page_cache] flush failed for ofd=%p offset=%llu: ret=%lld\n",
                                (void *)file,
                                (unsigned long long)curr->offset,
                                (long long)written);
                    }
                } else {
                    kprintf("[page_cache] flush skipped for ofd=%p offset=%llu: no write op\n",
                            (void *)file,
                            (unsigned long long)curr->offset);
                }
            }
            curr = curr->next;
        }
    }
    spinlock_release(&cache_lock);
}

/*
 * shmem.c — Anonymous shared memory objects for MAP_SHARED|MAP_ANONYMOUS.
 *
 * MATURITY_PLAN.md phase M4.
 *
 * Design: a global table of shmem objects, each identified by an integer
 * shmid.  Each object has a fixed size (page-rounded) and a hash table of
 * (offset -> physical frame) entries.  When a VMA with VMA_SHMEM faults
 * on a page, shmem_get_or_alloc() returns the same physical frame for
 * every process that maps the same shmid at the same offset.
 *
 * PMM refcount ensures the frame survives until the last page table
 * reference is dropped (paging_free_address_space decrements it).
 */

#include "kernel/mm/shmem.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/boot_info.h"

#define SHMEM_MAX_OBJECTS   64
#define SHMEM_BUCKETS       256

typedef struct shmem_frame {
    int      shmid;
    uint64_t offset;
    uint64_t phys;
    struct shmem_frame *next;
} shmem_frame_t;

typedef struct shmem_object {
    int      valid;
    int      shmid;
    uint64_t size;       /* page-rounded */
} shmem_object_t;

static shmem_object_t shmem_objects[SHMEM_MAX_OBJECTS];
static shmem_frame_t *shmem_buckets[SHMEM_BUCKETS];
static spinlock_t shmem_lock = SPINLOCK_UNLOCKED;
static int shmem_next_id = 1;

static uint32_t shmem_hash(int shmid, uint64_t offset) {
    return ((uint32_t)(shmid * 2654435761u) ^ (uint32_t)(offset >> 12)) % SHMEM_BUCKETS;
}

int shmem_create(uint64_t size) {
    if (size == 0) return -1;
    /* Round up to page size. */
    size = (size + 4095ULL) & ~4095ULL;

    spinlock_acquire(&shmem_lock);
    int slot = -1;
    for (int i = 0; i < SHMEM_MAX_OBJECTS; i++) {
        if (!shmem_objects[i].valid) { slot = i; break; }
    }
    if (slot < 0) {
        spinlock_release(&shmem_lock);
        return -1;
    }
    int id = shmem_next_id++;
    shmem_objects[slot].valid = 1;
    shmem_objects[slot].shmid = id;
    shmem_objects[slot].size = size;
    spinlock_release(&shmem_lock);

    kprintf("[shmem] created shmid=%d size=%llu pages=%llu\n",
            id, (unsigned long long)size, (unsigned long long)(size / 4096));
    return id;
}

void shmem_destroy(int shmid) {
    spinlock_acquire(&shmem_lock);

    /* Mark invalid first. */
    for (int i = 0; i < SHMEM_MAX_OBJECTS; i++) {
        if (shmem_objects[i].valid && shmem_objects[i].shmid == shmid) {
            shmem_objects[i].valid = 0;
            break;
        }
    }

    /* Collect frame entries for this shmid. */
    shmem_frame_t *to_free = NULL;
    for (int b = 0; b < SHMEM_BUCKETS; b++) {
        shmem_frame_t **pp = &shmem_buckets[b];
        while (*pp) {
            if ((*pp)->shmid == shmid) {
                shmem_frame_t *f = *pp;
                *pp = f->next;
                f->next = to_free;
                to_free = f;
            } else {
                pp = &(*pp)->next;
            }
        }
    }
    spinlock_release(&shmem_lock);

    /* Free frames: pmm_free_frame decrements refcount; the frame is actually
     * returned to the free list only when no page table still references it. */
    while (to_free) {
        shmem_frame_t *next = to_free->next;
        pmm_free_frame(to_free->phys);
        kfree(to_free);
        to_free = next;
    }
}

int shmem_valid(int shmid) {
    spinlock_acquire(&shmem_lock);
    for (int i = 0; i < SHMEM_MAX_OBJECTS; i++) {
        if (shmem_objects[i].valid && shmem_objects[i].shmid == shmid) {
            spinlock_release(&shmem_lock);
            return 1;
        }
    }
    spinlock_release(&shmem_lock);
    return 0;
}

uint64_t shmem_size(int shmid) {
    spinlock_acquire(&shmem_lock);
    for (int i = 0; i < SHMEM_MAX_OBJECTS; i++) {
        if (shmem_objects[i].valid && shmem_objects[i].shmid == shmid) {
            uint64_t sz = shmem_objects[i].size;
            spinlock_release(&shmem_lock);
            return sz;
        }
    }
    spinlock_release(&shmem_lock);
    return 0;
}

int shmem_get_or_alloc(int shmid, uint64_t offset, uint64_t *phys_out) {
    if (!phys_out) return -1;
    offset &= ~4095ULL;

    uint32_t h = shmem_hash(shmid, offset);

    spinlock_acquire(&shmem_lock);

    /* Validate shmid and check bounds. */
    int found = 0;
    for (int i = 0; i < SHMEM_MAX_OBJECTS; i++) {
        if (shmem_objects[i].valid && shmem_objects[i].shmid == shmid) {
            if (offset >= shmem_objects[i].size) {
                spinlock_release(&shmem_lock);
                return -1;   /* offset beyond shmem size */
            }
            found = 1;
            break;
        }
    }
    if (!found) {
        spinlock_release(&shmem_lock);
        return -1;
    }

    /* Look up existing frame. */
    shmem_frame_t *curr = shmem_buckets[h];
    while (curr) {
        if (curr->shmid == shmid && curr->offset == offset) {
            uint64_t phys = curr->phys;
            spinlock_release(&shmem_lock);
            /* Increment refcount so the caller holds a reference. */
            if (pmm_inc_frame_ref(phys) != 0) return -1;
            *phys_out = phys;
            return 0;
        }
        curr = curr->next;
    }
    spinlock_release(&shmem_lock);

    /* Allocate a new zero-filled frame. */
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return -1;

    uint64_t hhdm = boot_get_hhdm_offset();
    memset((void *)(uintptr_t)(hhdm + phys), 0, 4096);

    shmem_frame_t *entry = kmalloc(sizeof(*entry));
    if (!entry) {
        pmm_free_frame(phys);
        return -1;
    }
    entry->shmid = shmid;
    entry->offset = offset;
    entry->phys = phys;

    spinlock_acquire(&shmem_lock);

    /* Double-check: another CPU may have inserted while we allocated. */
    curr = shmem_buckets[h];
    while (curr) {
        if (curr->shmid == shmid && curr->offset == offset) {
            uint64_t existing = curr->phys;
            spinlock_release(&shmem_lock);
            kfree(entry);
            pmm_free_frame(phys);
            if (pmm_inc_frame_ref(existing) != 0) return -1;
            *phys_out = existing;
            return 0;
        }
        curr = curr->next;
    }

    entry->next = shmem_buckets[h];
    shmem_buckets[h] = entry;
    spinlock_release(&shmem_lock);

    /* The caller holds the initial reference (refcount = 1 from alloc). */
    *phys_out = phys;
    return 0;
}

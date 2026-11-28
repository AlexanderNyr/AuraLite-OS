/*
 * shmem.c — Anonymous shared memory objects for MAP_SHARED|MAP_ANONYMOUS.
 *
 * Object lifetime is independent from PTE lifetime. Every cached frame has
 * one PMM reference owned by the shmem object, and every installed PTE has
 * one additional PMM reference. This allows an object to disappear while an
 * already-installed mapping remains valid, without allowing a later fault
 * to rediscover a frame that has been returned to the PMM.
 */

#include "kernel/mm/shmem.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/boot_info.h"
#include <limits.h>

#define SHMEM_MAX_OBJECTS   64
#define SHMEM_BUCKETS       256
#define SHMEM_PAGE_SIZE     4096ULL

typedef struct shmem_frame {
    int      shmid;
    uint64_t offset;
    uint64_t phys;
    struct shmem_frame *next;
} shmem_frame_t;

typedef struct shmem_object {
    int      valid;
    int      shmid;
    uint32_t refs;
    uint64_t size;       /* page-rounded */
} shmem_object_t;

static shmem_object_t shmem_objects[SHMEM_MAX_OBJECTS];
static shmem_frame_t *shmem_buckets[SHMEM_BUCKETS];
static spinlock_t shmem_lock = SPINLOCK_UNLOCKED;
static uint32_t shmem_next_id = 1;

static uint32_t shmem_hash(int shmid, uint64_t offset) {
    return ((uint32_t)shmid * 2654435761u ^
            (uint32_t)(offset >> 12)) % SHMEM_BUCKETS;
}

/* Caller holds shmem_lock. */
static shmem_object_t *shmem_find_object_locked(int shmid) {
    for (int i = 0; i < SHMEM_MAX_OBJECTS; i++) {
        if (shmem_objects[i].valid && shmem_objects[i].shmid == shmid)
            return &shmem_objects[i];
    }
    return NULL;
}

/* Caller holds shmem_lock. */
static shmem_frame_t *shmem_find_frame_locked(int shmid, uint64_t offset,
                                               uint32_t bucket) {
    shmem_frame_t *frame = shmem_buckets[bucket];
    while (frame) {
        if (frame->shmid == shmid && frame->offset == offset)
            return frame;
        frame = frame->next;
    }
    return NULL;
}

/* Caller holds shmem_lock. There are at most SHMEM_MAX_OBJECTS live IDs, so
 * trying MAX_OBJECTS+1 sequential candidates is sufficient to find a free ID. */
static int shmem_alloc_id_locked(void) {
    for (int attempt = 0; attempt <= SHMEM_MAX_OBJECTS; attempt++) {
        uint32_t candidate = shmem_next_id;
        if (shmem_next_id >= (uint32_t)INT_MAX)
            shmem_next_id = 1;
        else
            shmem_next_id++;

        if (candidate == 0 || candidate > (uint32_t)INT_MAX)
            continue;
        if (!shmem_find_object_locked((int)candidate))
            return (int)candidate;
    }
    return -1;
}

int shmem_create(uint64_t size) {
    if (size == 0 || size > UINT64_MAX - (SHMEM_PAGE_SIZE - 1))
        return -1;
    size = (size + SHMEM_PAGE_SIZE - 1) & ~(SHMEM_PAGE_SIZE - 1);

    spinlock_acquire(&shmem_lock);

    int slot = -1;
    for (int i = 0; i < SHMEM_MAX_OBJECTS; i++) {
        if (!shmem_objects[i].valid) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_release(&shmem_lock);
        return -1;
    }

    int id = shmem_alloc_id_locked();
    if (id < 0) {
        spinlock_release(&shmem_lock);
        return -1;
    }

    shmem_objects[slot].valid = 1;
    shmem_objects[slot].shmid = id;
    shmem_objects[slot].refs = 1; /* creator/handle reference */
    shmem_objects[slot].size = size;
    spinlock_release(&shmem_lock);

    kprintf("[shmem] created shmid=%d size=%llu pages=%llu\n",
            id, (unsigned long long)size,
            (unsigned long long)(size / SHMEM_PAGE_SIZE));
    return id;
}

int shmem_get(int shmid) {
    if (shmid <= 0) return -1;

    spinlock_acquire(&shmem_lock);
    shmem_object_t *object = shmem_find_object_locked(shmid);
    if (!object || object->refs == UINT32_MAX) {
        spinlock_release(&shmem_lock);
        return -1;
    }
    object->refs++;
    spinlock_release(&shmem_lock);
    return 0;
}

void shmem_put(int shmid) {
    if (shmid <= 0) return;

    shmem_frame_t *to_free = NULL;

    spinlock_acquire(&shmem_lock);
    shmem_object_t *object = shmem_find_object_locked(shmid);
    if (!object || object->refs == 0) {
        spinlock_release(&shmem_lock);
        return;
    }

    object->refs--;
    if (object->refs != 0) {
        spinlock_release(&shmem_lock);
        return;
    }

    /* No VMA, handle or in-flight operation can still resolve this object. */
    object->valid = 0;
    object->size = 0;
    object->shmid = 0;

    for (int bucket = 0; bucket < SHMEM_BUCKETS; bucket++) {
        shmem_frame_t **link = &shmem_buckets[bucket];
        while (*link) {
            if ((*link)->shmid == shmid) {
                shmem_frame_t *frame = *link;
                *link = frame->next;
                frame->next = to_free;
                to_free = frame;
            } else {
                link = &(*link)->next;
            }
        }
    }
    spinlock_release(&shmem_lock);

    /* Drop the object's PMM reference. PTE references, if any, keep frames
     * alive until their own unmap/address-space-reap path drops them. */
    while (to_free) {
        shmem_frame_t *next = to_free->next;
        pmm_free_frame(to_free->phys);
        kfree(to_free);
        to_free = next;
    }
}

void shmem_destroy(int shmid) {
    /* Historical API name: now drops the creator/handle reference instead
     * of forcibly invalidating an object still held by VMAs or page faults. */
    shmem_put(shmid);
}

int shmem_valid(int shmid) {
    int valid;
    spinlock_acquire(&shmem_lock);
    valid = shmem_find_object_locked(shmid) != NULL;
    spinlock_release(&shmem_lock);
    return valid;
}

uint64_t shmem_size(int shmid) {
    uint64_t size = 0;
    spinlock_acquire(&shmem_lock);
    shmem_object_t *object = shmem_find_object_locked(shmid);
    if (object) size = object->size;
    spinlock_release(&shmem_lock);
    return size;
}

int shmem_get_or_alloc(int shmid, uint64_t offset, uint64_t *phys_out) {
    if (!phys_out || shmid <= 0) return -1;
    *phys_out = 0;
    offset &= ~(SHMEM_PAGE_SIZE - 1);

    /* Pin the object across the unlocked allocation and PMM operations. */
    if (shmem_get(shmid) != 0) return -1;

    uint32_t bucket = shmem_hash(shmid, offset);
    uint64_t existing_phys = 0;

    spinlock_acquire(&shmem_lock);
    shmem_object_t *object = shmem_find_object_locked(shmid);
    if (!object || offset >= object->size) {
        spinlock_release(&shmem_lock);
        shmem_put(shmid);
        return -1;
    }
    shmem_frame_t *found = shmem_find_frame_locked(shmid, offset, bucket);
    if (found) existing_phys = found->phys;
    spinlock_release(&shmem_lock);

    if (existing_phys) {
        /* The object pin prevents its cached-frame reference from being
         * removed between lookup and this mapping-reference acquisition. */
        int rc = pmm_inc_frame_ref(existing_phys);
        shmem_put(shmid);
        if (rc != 0) return -1;
        *phys_out = existing_phys;
        return 0;
    }

    /* pmm_alloc_frame() already zero-fills the page. Its first reference is
     * reserved for the object; acquire the caller/PTE reference before the
     * entry becomes visible to other CPUs. */
    uint64_t phys = pmm_alloc_frame();
    if (!phys) {
        shmem_put(shmid);
        return -1;
    }
    if (pmm_inc_frame_ref(phys) != 0) {
        pmm_free_frame(phys);
        shmem_put(shmid);
        return -1;
    }

    shmem_frame_t *entry = kmalloc(sizeof(*entry));
    if (!entry) {
        pmm_free_frame(phys); /* caller reference */
        pmm_free_frame(phys); /* provisional object reference */
        shmem_put(shmid);
        return -1;
    }
    entry->shmid = shmid;
    entry->offset = offset;
    entry->phys = phys;
    entry->next = NULL;

    spinlock_acquire(&shmem_lock);
    object = shmem_find_object_locked(shmid);
    found = object ? shmem_find_frame_locked(shmid, offset, bucket) : NULL;
    if (found) {
        existing_phys = found->phys;
        spinlock_release(&shmem_lock);

        kfree(entry);
        pmm_free_frame(phys); /* unused caller reference */
        pmm_free_frame(phys); /* unused provisional object reference */

        int rc = pmm_inc_frame_ref(existing_phys);
        shmem_put(shmid);
        if (rc != 0) return -1;
        *phys_out = existing_phys;
        return 0;
    }
    if (!object) {
        /* Defensive: the temporary object reference should make this
         * impossible, but fail closed if the table is inconsistent. */
        spinlock_release(&shmem_lock);
        kfree(entry);
        pmm_free_frame(phys);
        pmm_free_frame(phys);
        shmem_put(shmid);
        return -1;
    }

    entry->next = shmem_buckets[bucket];
    shmem_buckets[bucket] = entry;
    spinlock_release(&shmem_lock);

    /* The cached entry owns one PMM reference and the caller owns the other. */
    *phys_out = phys;
    shmem_put(shmid);
    return 0;
}

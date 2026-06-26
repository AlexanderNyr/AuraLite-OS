/* kheap.c — kernel heap backed by PMM + VMM.
 *
 * The allocator core is kernel-independent (heap.c); this file supplies the
 * page-backed expansion callback and the kmalloc/kfree/krealloc wrappers.
 * On demand it maps fresh physical frames (from the PMM) into the kernel heap
 * virtual region (via the VMM) and hands the new span to the allocator as one
 * large free block.
 */

#include <stdint.h>
#include "kernel/mm/kheap.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define PAGE_SIZE 4096ULL
#define HEAP_TAG  "[heap] "
/* Expand in chunks of at least 64 KiB to amortise the mapping cost. */
#define EXPAND_MIN  (64ULL * 1024ULL)
#define MIB         (1024ULL * 1024ULL)

static heap_t kheap;

/*
 * Commit more memory into the heap: map aligned pages over [brk, brk+want),
 * then register that span as a single free block.
 * Returns 0 on success, non-zero if the region limit would be exceeded.
 */
static int kheap_expand(heap_t *h, uint64_t want) {
    if (want < EXPAND_MIN) {
        want = EXPAND_MIN;
    }
    want = (want + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);   /* page-align */

    if (h->brk + want > h->limit) {
        return 1;                                        /* region exhausted */
    }

    uint64_t old_brk = h->brk;
    uint64_t mapped  = 0;
    while (mapped < want) {
        uint64_t phys = pmm_alloc_frame();
        if (phys == 0) {
            /* Could not map the whole request; commit whatever we managed. */
            if (mapped == 0) {
                return 1;
            }
            break;
        }
        uint64_t virt = h->base + old_brk + mapped;
        paging_map(virt, phys,
                   PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_NO_EXEC);
        mapped += PAGE_SIZE;
    }

    /* Expose the freshly-mapped span as one free block. The footer is a
     * 16-byte {magic, size} boundary tag at the end of the span. */
    heap_block_t *b = (heap_block_t *)(h->base + old_brk);
    b->magic = HEAP_MAGIC_FREE;
    b->size  = mapped;
    uint64_t *foot = (uint64_t *)((char *)b + mapped - 16);
    foot[0] = HEAP_MAGIC_FREE;
    foot[1] = mapped;

    /* Insert into the free list (head). */
    b->next = h->free_list;
    b->prev = NULL;
    if (h->free_list) {
        h->free_list->prev = b;
    }
    h->free_list = b;

    h->brk = old_brk + mapped;
    return 0;
}

void kheap_init(void) {
    heap_init(&kheap, KHEAP_BASE, KHEAP_LIMIT, kheap_expand);
    kheap_dump();
}

/* ---- Statistics (walk every block in address order) ---- */

void kheap_dump(void) {
    uint64_t committed = kheap.brk;
    uint64_t used = 0, free_b = 0;
    heap_block_t *b = (heap_block_t *)kheap.base;
    while ((char *)b < (char *)kheap.base + kheap.brk) {
        if (b->magic == HEAP_MAGIC_USED) {
            used += b->size;
        } else if (b->magic == HEAP_MAGIC_FREE) {
            free_b += b->size;
        } else {
            break;   /* unmapped or corrupt */
        }
        b = (heap_block_t *)((char *)b + b->size);
    }
    kprintf(HEAP_TAG "region 0x%016llx (%llu MiB), committed %llu KiB (%llu pages)\n",
            (unsigned long long)kheap.base,
            (unsigned long long)(kheap.limit / MIB),
            (unsigned long long)(committed / 1024),
            (unsigned long long)(committed / PAGE_SIZE));
    kprintf(HEAP_TAG "used %llu KiB, free %llu KiB\n",
            (unsigned long long)(used / 1024),
            (unsigned long long)(free_b / 1024));
}

void *kmalloc(uint64_t size) {
    return heap_alloc(&kheap, size);
}

void kfree(void *ptr) {
    if (ptr) {
        heap_block_t *b = (heap_block_t *)((char *)ptr - HEAP_HEADER_SIZE);
        if (b->magic == HEAP_MAGIC_USED && b->size > HEAP_HEADER_SIZE + HEAP_FOOTER_SIZE) {
            uint64_t payload = b->size - HEAP_HEADER_SIZE - HEAP_FOOTER_SIZE;
            memset(ptr, 0, (size_t)payload);
        }
    }
    heap_free(&kheap, ptr);
}

void *krealloc(void *ptr, uint64_t size) {
    return heap_realloc(&kheap, ptr, size);
}

/*
 * Gate self-test: 10 000 alloc/free cycles exercising varied sizes, plus a
 * realloc round-trip. Verifies:
 *   - every allocation returns a non-NULL, distinct pointer
 *   - the allocator's own payload content survives across calls (corruption)
 *   - all memory is returned after freeing (no leak)
 *   - realloc preserves data and frees the old block
 */
void kheap_self_test(void) {
    enum { N = 10000 };
    static void *ptrs[N];           /* static: keep off the (small) stack */

    kprintf(HEAP_TAG "self-test: %d alloc/free cycles...\n", N);

    /* Phase A: allocate N blocks of varying sizes and tag each with its
     * expected pattern (byte = index & 0xFF into the first 8 bytes). */
    for (int i = 0; i < N; i++) {
        /* Sizes from 8 .. ~256 bytes, mixed to stress splitting/coalescing. */
        uint64_t sz = 8 + (uint64_t)((i * 37) % 256);
        ptrs[i] = kmalloc(sz);
        if (ptrs[i] == NULL) {
            kprintf(HEAP_TAG "FAIL: kmalloc(%llu) returned NULL at %d\n",
                    (unsigned long long)sz, i);
            return;
        }
        /* Stamp the first 8 bytes with i so we can detect corruption later. */
        uint8_t *p = (uint8_t *)ptrs[i];
        for (int k = 0; k < 8; k++) {
            p[k] = (uint8_t)(i + k);
        }
    }

    /* Uniqueness: every pointer must be distinct. */
    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            if (ptrs[i] == ptrs[j]) {
                kprintf(HEAP_TAG "FAIL: duplicate pointer %p (%d/%d)\n",
                        ptrs[i], i, j);
                return;
            }
        }
    }

    /* Free every other block (creates a fragmented free list), then verify the
     * survivors' stamps are intact. */
    for (int i = 0; i < N; i += 2) {
        kfree(ptrs[i]);
        ptrs[i] = NULL;
    }
    for (int i = 1; i < N; i += 2) {
        uint8_t *p = (uint8_t *)ptrs[i];
        for (int k = 0; k < 8; k++) {
            if (p[k] != (uint8_t)(i + k)) {
                kprintf(HEAP_TAG "FAIL: corruption at slot %d (byte %d)\n", i, k);
                return;
            }
        }
    }

    /* Re-allocate into the freed holes (exercises coalesced/split reuse). */
    for (int i = 0; i < N; i += 2) {
        ptrs[i] = kmalloc(16 + (uint64_t)(i % 64));
        if (ptrs[i] == NULL) {
            kprintf(HEAP_TAG "FAIL: re-alloc NULL at %d\n", i);
            return;
        }
    }

    /* realloc round-trip: grow then shrink, data must survive the grow. */
    uint8_t *r = kmalloc(32);
    if (r == NULL) {
        kprintf(HEAP_TAG "FAIL: realloc base alloc NULL\n");
        return;
    }
    for (int k = 0; k < 32; k++) {
        r[k] = (uint8_t)(0xA0 | (k & 0x0F));
    }
    uint8_t *r2 = krealloc(r, 4096);          /* grow: must copy 32 bytes */
    if (r2 == NULL) {
        kprintf(HEAP_TAG "FAIL: krealloc grow returned NULL\n");
        return;
    }
    for (int k = 0; k < 32; k++) {
        if (r2[k] != (uint8_t)(0xA0 | (k & 0x0F))) {
            kprintf(HEAP_TAG "FAIL: realloc lost data at byte %d\n", k);
            return;
        }
    }
    uint8_t *r3 = krealloc(r2, 24);           /* shrink: in-place, ptr stable */
    if (r3 == NULL) {
        kprintf(HEAP_TAG "FAIL: krealloc shrink returned NULL\n");
        return;
    }
    kfree(r3);

    /* Free everything. */
    for (int i = 0; i < N; i++) {
        if (ptrs[i]) {
            kfree(ptrs[i]);
            ptrs[i] = NULL;
        }
    }

    /* Leak check: after freeing all live allocations, the whole committed
     * region should be one coalesced free block (free_b == committed). */
    uint64_t committed = kheap.brk;
    uint64_t used = 0, free_b = 0;
    heap_block_t *b = (heap_block_t *)kheap.base;
    while ((char *)b < (char *)kheap.base + kheap.brk) {
        if (b->magic == HEAP_MAGIC_USED) {
            used += b->size;
        } else if (b->magic == HEAP_MAGIC_FREE) {
            free_b += b->size;
        } else {
            break;
        }
        b = (heap_block_t *)((char *)b + b->size);
    }
    if (used != 0) {
        kprintf(HEAP_TAG "FAIL: leak detected (%llu bytes still used)\n",
                (unsigned long long)used);
        return;
    }
    if (free_b != committed) {
        kprintf(HEAP_TAG "FAIL: fragmentation leak (free %llu != committed %llu)\n",
                (unsigned long long)free_b, (unsigned long long)committed);
        return;
    }

    kprintf(HEAP_TAG "PASS: %d cycles, no corruption, no leak, realloc OK\n", N);
    kheap_dump();
}

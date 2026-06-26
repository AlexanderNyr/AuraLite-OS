#ifndef AURALITE_MM_HEAP_H
#define AURALITE_MM_HEAP_H

#include <stdint.h>
#include <stddef.h>

/*
 * Generic, freestanding first-fit heap allocator with boundary-tag
 * coalescing.
 *
 * Deliberately free of any kernel dependency (only <stdint.h>), so the SAME
 * code is unit-tested on the host (tests/unit/test_heap.c) and used by the
 * kernel via a thin wrapper (kernel/mm/kheap.c). The only kernel-coupled part
 * is page-backed expansion, supplied as a callback so the core allocator stays
 * portable.
 *
 * Region layout: a contiguous virtual range [base, base+limit). Memory is
 * "committed" on demand by the expand callback up to a high-water mark `brk`.
 *
 * Every block (used or free) has:
 *   - a 32-byte header at its start: {magic, size, next, prev}
 *   - a 16-byte footer at its end:   {magic, size}   (boundary tag)
 * The footer lets us find the previous block's header in O(1) for coalescing.
 * A doubly-linked free list links only the free blocks (first-fit search).
 *
 * magic distinguishes used vs free blocks (so we need no separate flag field
 * and the header stays a clean 32 bytes / 16-aligned).
 */

typedef struct heap_block heap_block_t;
typedef struct heap       heap_t;

/* expand: ensure at least `want` bytes are committed from base+brk by mapping
 * pages (kernel) or by bumping brk into a pre-allocated buffer (host test).
 * Returns 0 on success, non-zero if the region limit would be exceeded. */
typedef int (*heap_expand_fn)(heap_t *h, uint64_t want);

struct heap_block {
    uint64_t       magic;   /* HEAP_MAGIC_USED / HEAP_MAGIC_FREE */
    uint64_t       size;    /* total block size in bytes (header+payload+footer) */
    heap_block_t  *next;    /* free-list linkage (valid only when free) */
    heap_block_t  *prev;
};

struct heap {
    uintptr_t      base;       /* virtual start of the heap region */
    uint64_t       limit;      /* total reservable bytes */
    uint64_t       brk;        /* bytes committed from base (high-water mark) */
    heap_block_t  *free_list;  /* head of the free list */
    heap_expand_fn expand;     /* commit more pages on demand */
};

#define HEAP_MAGIC_USED 0xA10CA10CA10CA10CULL
#define HEAP_MAGIC_FREE 0xF8EEF8EEF8EEF8EEULL
#define HEAP_HEADER_SIZE ((uint64_t)sizeof(heap_block_t))
#define HEAP_FOOTER_SIZE (16ULL)

void  heap_init(heap_t *h, uintptr_t base, uint64_t limit, heap_expand_fn expand);
void *heap_alloc(heap_t *h, uint64_t size);
void  heap_free(heap_t *h, void *ptr);
void *heap_realloc(heap_t *h, void *ptr, uint64_t size);

#endif /* AURALITE_MM_HEAP_H */

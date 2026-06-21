/* heap.c — generic first-fit heap allocator with boundary-tag coalescing.
 *
 * No kernel dependencies (only <stdint.h>); the page-backed expansion is
 * supplied as a callback. The same object compiles into the kernel (kheap.c
 * wraps it) and into the host unit test (tests/unit/test_heap.c).
 */

#include <stdint.h>
#include "kernel/mm/heap.h"

#define HEAP_ALIGN    16u
#define HEADER_SIZE   ((uint64_t)sizeof(heap_block_t))   /* 32 */
#define FOOTER_SIZE   ((uint64_t)sizeof(struct footer))  /* 16 */
#define MIN_PAYLOAD   16u
#define MIN_BLOCK     (HEADER_SIZE + FOOTER_SIZE + MIN_PAYLOAD)

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((uint64_t)((a) - 1)))

/* Boundary tag placed at the END of every block. */
struct footer {
    uint64_t magic;
    uint64_t size;     /* mirrors header->size */
};

/* ---- Footer helpers ---- */

static struct footer *block_footer(heap_block_t *b) {
    return (struct footer *)((char *)b + b->size - FOOTER_SIZE);
}

static void set_footer(heap_block_t *b) {
    struct footer *f = block_footer(b);
    f->magic = b->magic;
    f->size  = b->size;
}

/* Total block size required to hold a `payload`-byte allocation. */
static uint64_t required_block_size(uint64_t payload) {
    if (payload < MIN_PAYLOAD) {
        payload = MIN_PAYLOAD;
    }
    return ALIGN_UP(HEADER_SIZE + FOOTER_SIZE + payload, HEAP_ALIGN);
}

/* ---- Free-list operations ---- */

static void fl_insert(heap_t *h, heap_block_t *b) {
    /* Insert at head: O(1). Order does not matter for first-fit correctness. */
    b->prev = NULL;
    b->next = h->free_list;
    if (h->free_list) {
        h->free_list->prev = b;
    }
    h->free_list = b;
}

static void fl_remove(heap_t *h, heap_block_t *b) {
    if (b->prev) {
        b->prev->next = b->next;
    } else {
        h->free_list = b->next;
    }
    if (b->next) {
        b->next->prev = b->prev;
    }
    b->next = NULL;
    b->prev = NULL;
}

void heap_init(heap_t *h, uintptr_t base, uint64_t limit, heap_expand_fn expand) {
    h->base      = base;
    h->limit     = limit;
    h->brk       = 0;
    h->free_list = NULL;
    h->expand    = expand;
}

/* Split a free block `b` so it is exactly `need` bytes, turning the leftover
 * tail into a new free block. Only splits if the remainder could hold a block. */
static void split_block(heap_t *h, heap_block_t *b, uint64_t need) {
    if (b->size < need + MIN_BLOCK) {
        return;   /* remainder too small to be a block: keep it internal */
    }
    heap_block_t *rem = (heap_block_t *)((char *)b + need);
    rem->magic = HEAP_MAGIC_FREE;
    rem->size  = b->size - need;
    set_footer(rem);
    fl_insert(h, rem);

    b->size = need;
    set_footer(b);
}

void *heap_alloc(heap_t *h, uint64_t size) {
    uint64_t need = required_block_size(size);

    for (;;) {
        /* First-fit search of the free list. */
        for (heap_block_t *b = h->free_list; b; b = b->next) {
            if (b->size >= need) {
                split_block(h, b, need);
                b->magic = HEAP_MAGIC_USED;
                set_footer(b);
                fl_remove(h, b);
                return (char *)b + HEADER_SIZE;
            }
        }

        /* No fit: try to commit more memory. */
        if (h->expand == NULL || h->expand(h, need) != 0) {
            return NULL;                       /* out of memory */
        }
        /* expand() turned [old_brk, new_brk) into a fresh free block, so a
         * first-fit search will now find it. */
    }
}

void heap_free(heap_t *h, void *ptr) {
    if (ptr == NULL) {
        return;
    }
    heap_block_t *b = (heap_block_t *)((char *)ptr - HEADER_SIZE);
    if (b->magic != HEAP_MAGIC_USED) {
        /* Double-free or corruption: refuse rather than corrupt the heap. */
        return;
    }

    /* Mark free; footer is updated after any merge. */
    b->magic = HEAP_MAGIC_FREE;

    /* Coalesce with the NEXT neighbour (higher address), if it is free and
     * lies within the committed region. */
    char *next_addr = (char *)b + b->size;
    if (next_addr < (char *)h->base + h->brk) {
        heap_block_t *nb = (heap_block_t *)next_addr;
        if (nb->magic == HEAP_MAGIC_FREE) {
            fl_remove(h, nb);
            b->size += nb->size;
            set_footer(b);
        }
    }

    /* Coalesce with the PREVIOUS neighbour (lower address) via its boundary
     * tag, unless we are the first block in the region. */
    int merged_prev = 0;
    if ((char *)b > (char *)h->base) {
        struct footer *pf = (struct footer *)((char *)b - FOOTER_SIZE);
        if (pf->magic == HEAP_MAGIC_FREE) {
            heap_block_t *prev = (heap_block_t *)((char *)b - pf->size);
            prev->size += b->size;
            set_footer(prev);
            b = prev;            /* prev is already in the free list */
            merged_prev = 1;
        }
    }

    if (!merged_prev) {
        fl_insert(h, b);
    }
}

void *heap_realloc(heap_t *h, void *ptr, uint64_t size) {
    if (ptr == NULL) {
        return heap_alloc(h, size);
    }
    if (size == 0) {
        heap_free(h, ptr);
        return NULL;
    }
    heap_block_t *b = (heap_block_t *)((char *)ptr - HEADER_SIZE);
    if (b->magic != HEAP_MAGIC_USED) {
        return NULL;   /* bad pointer */
    }

    /* Usable payload of the existing block. */
    uint64_t old_payload = b->size - HEADER_SIZE - FOOTER_SIZE;

    /* If the new request fits in the current block, reuse it in place. */
    if (required_block_size(size) <= b->size) {
        return ptr;
    }

    void *np = heap_alloc(h, size);
    if (np == NULL) {
        return NULL;
    }
    /* Copy the smaller of the old/new payloads (byte loop: no libc needed). */
    uint64_t to_copy = (old_payload < size) ? old_payload : size;
    char *src = (char *)ptr;
    char *dst = (char *)np;
    for (uint64_t i = 0; i < to_copy; i++) {
        dst[i] = src[i];
    }
    heap_free(h, ptr);
    return np;
}

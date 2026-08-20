/* sizeclass.h — the kmalloc size-class cache core, pure C
 * (OPT_PLAN.md O6).
 *
 * A recycling front for the first-fit heap: objects of nine power-of-two
 * classes (16 B .. 4 KiB) are, on free, pushed onto a per-class LIFO
 * instead of returning to the free list, and served O(1) on the next
 * allocation of that class.  The first-fit walk (Fact 5's 665 394 nodes
 * per boot) is paid only on a class MISS — the first allocation of a
 * size, or a burst deeper than the cache.
 *
 * Two deliberate bounds keep the underlying allocator honest:
 *
 *   - SIZECLASS_CAP per class: beyond it a free falls through to
 *     heap_free(), so boundary-tag coalescing keeps working and the
 *     cache can hold at most ~511 KiB (sum of class sizes × cap) of a
 *     64 MiB heap.  A cache that can grow without bound is a leak with
 *     an alibi.
 *
 *   - Objects ARE ordinary heap blocks (allocated via heap_alloc once,
 *     recycled thereafter).  kfree() classifies by the block's real
 *     payload capacity, so a block that heap_alloc over-provisioned
 *     (split remainder too small to shed) simply lands in the largest
 *     class it can serve.  Nothing here knows heap internals beyond
 *     "payload bytes are mine to scribble on while free" — the next
 *     pointer lives in the first 8 payload bytes.
 *
 * Pure on purpose: no locks, no perfstat, no heap calls.  kheap.c
 * composes this under kheap_lock; tests/unit/test_sizeclass.c runs the
 * matrix on the host (the uart_ring.h convention).
 */
#ifndef AURALITE_MM_SIZECLASS_H
#define AURALITE_MM_SIZECLASS_H

#include <stdint.h>
#include <stddef.h>

#define SIZECLASS_MIN_SHIFT 4u                 /* 16 B  */
#define SIZECLASS_MAX_SHIFT 12u                /* 4 KiB */
#define SIZECLASS_COUNT (SIZECLASS_MAX_SHIFT - SIZECLASS_MIN_SHIFT + 1u)
#define SIZECLASS_CAP   64u                    /* objects held per class */

typedef struct sizeclass_cache {
    void    *head[SIZECLASS_COUNT];
    uint32_t count[SIZECLASS_COUNT];
    /* Diagnostics for /proc/perf and the self-test. */
    uint64_t hits;
    uint64_t misses;
    uint64_t spills;      /* frees past the cap, sent to heap_free */
} sizeclass_cache_t;

static inline uint64_t sizeclass_bytes(uint32_t ci) {
    return 1ULL << (SIZECLASS_MIN_SHIFT + ci);
}

/* Smallest class that can serve a request of `size` bytes; -1 when the
 * request is 0 or beyond the largest class (first-fit territory). */
static inline int sizeclass_for_request(uint64_t size) {
    if (size == 0 || size > (1ULL << SIZECLASS_MAX_SHIFT)) return -1;
    for (uint32_t ci = 0; ci < SIZECLASS_COUNT; ci++) {
        if (size <= sizeclass_bytes(ci)) return (int)ci;
    }
    return -1;                                 /* unreachable */
}

/* Largest class a block of `payload` bytes can SERVE; -1 when even the
 * smallest class does not fit.  Rounds down — a 48-byte payload serves
 * class 32, never class 64. */
static inline int sizeclass_for_payload(uint64_t payload) {
    int best = -1;
    for (uint32_t ci = 0; ci < SIZECLASS_COUNT; ci++) {
        if (sizeclass_bytes(ci) <= payload) best = (int)ci;
        else break;
    }
    return best;
}

/* Push a free object (payload pointer).  Caller checked the cap. */
static inline void sizeclass_push(sizeclass_cache_t *c, uint32_t ci,
                                  void *obj) {
    void **link = (void **)obj;               /* first 8 payload bytes */
    *link = c->head[ci];
    c->head[ci] = obj;
    c->count[ci]++;
}

/* Pop an object, or NULL when the class is empty.  The link word is
 * scrubbed so recycled memory matches the kfree()-scrubbed state the
 * heap path hands out. */
static inline void *sizeclass_pop(sizeclass_cache_t *c, uint32_t ci) {
    void *obj = c->head[ci];
    if (!obj) return NULL;
    void **link = (void **)obj;
    c->head[ci] = *link;
    *link = NULL;
    c->count[ci]--;
    return obj;
}

#endif /* AURALITE_MM_SIZECLASS_H */

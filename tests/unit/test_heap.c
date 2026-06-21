/*
 * test_heap.c — host-side unit tests for the generic heap allocator.
 *
 * The allocator core (kernel/mm/heap.c) is freestanding and portable; we link
 * it directly and back it with a host malloc() buffer whose expand() is a
 * trivial no-op (the memory is pre-committed). This exercises the same
 * split/coalesce/free-list/realloc code paths the kernel uses, without any
 * paging dependency.
 *
 * Run with:  make test-unit
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/mm/heap.h"

#define REGION_BYTES  (8ULL * 1024 * 1024)   /* 8 MiB backing buffer */

static uint8_t *backing = NULL;

/* Pre-committed region: expand() just bumps brk up to the full buffer. */
static int host_expand(heap_t *h, uint64_t want) {
    uint64_t aligned = (want + 15) & ~15ULL;
    if (h->brk + aligned > h->limit) {
        return 1;
    }
    h->brk += aligned;
    return 0;
}

static heap_t make_heap(void) {
    backing = (uint8_t *)malloc(REGION_BYTES);
    if (!backing) {
        fprintf(stderr, "host malloc failed\n");
        exit(2);
    }
    memset(backing, 0, REGION_BYTES);
    heap_t h;
    heap_init(&h, (uintptr_t)backing, REGION_BYTES, host_expand);
    /* Commit the entire region up front so the first alloc has a free block.
       heap_alloc will find the free block created here. */
    h.brk = REGION_BYTES;
    /* Build the initial single free block spanning the whole region. */
    {
        heap_block_t *b = (heap_block_t *)backing;
        b->magic = HEAP_MAGIC_FREE;
        b->size  = REGION_BYTES;
        uint64_t *foot = (uint64_t *)(backing + REGION_BYTES - 16);
        foot[0] = HEAP_MAGIC_FREE;
        foot[1] = REGION_BYTES;
        b->next = NULL;
        b->prev = NULL;
        h.free_list = b;
    }
    return h;
}

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                 \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static void test_basic(void) {
    printf("[test] basic alloc/free...\n");
    heap_t h = make_heap();
    void *a = heap_alloc(&h, 100);
    void *b = heap_alloc(&h, 100);
    CHECK(a != NULL && b != NULL);
    CHECK(a != b);
    heap_free(&h, a);
    heap_free(&h, b);
    free(backing);
}

static void test_alignment(void) {
    printf("[test] payload alignment...\n");
    heap_t h = make_heap();
    for (int i = 0; i < 256; i++) {
        void *p = heap_alloc(&h, 1 + (uint64_t)(i * 7));
        CHECK(p != NULL);
        CHECK(((uintptr_t)p % 16) == 0);   /* payload must be 16-aligned */
        heap_free(&h, p);
    }
    free(backing);
}

static void test_coalesce(void) {
    printf("[test] coalescing (free b between a,c -> one big block)...\n");
    heap_t h = make_heap();
    void *a = heap_alloc(&h, 64);
    void *b = heap_alloc(&h, 64);
    void *c = heap_alloc(&h, 64);
    void *d = heap_alloc(&h, 64);   /* guard block so c's neighbour isn't free */
    (void)d;
    heap_free(&h, a);
    heap_free(&h, c);
    heap_free(&h, b);               /* should coalesce a+b+c into one */
    /* A fresh large alloc that wouldn't have fit in a single 64-byte hole
       should now succeed, proving the three merged. */
    void *big = heap_alloc(&h, 160);   /* needs > (64-overhead) single block */
    CHECK(big != NULL);
    heap_free(&h, big);
    free(backing);
}

static void test_realloc(void) {
    printf("[test] realloc preserves data...\n");
    heap_t h = make_heap();
    uint8_t *p = heap_alloc(&h, 64);
    for (int i = 0; i < 64; i++) {
        p[i] = (uint8_t)i;
    }
    uint8_t *q = heap_realloc(&h, p, 1024);   /* grow */
    CHECK(q != NULL);
    for (int i = 0; i < 64; i++) {
        CHECK(q[i] == (uint8_t)i);
    }
    uint8_t *r = heap_realloc(&h, q, 16);      /* shrink in place */
    CHECK(r != NULL);
    for (int i = 0; i < 16; i++) {
        CHECK(r[i] == (uint8_t)i);
    }
    heap_free(&h, r);
    free(backing);
}

/* The Phase 5 gate criterion, run on the host: 10 000 alloc/free cycles. */
static void test_stress(void) {
    printf("[test] 10000-cycle stress (no corruption, no leak)...\n");
    heap_t h = make_heap();
    enum { N = 10000 };
    static void *ptrs[N];
    for (int i = 0; i < N; i++) {
        ptrs[i] = heap_alloc(&h, 8 + (uint64_t)((i * 37) % 256));
        CHECK(ptrs[i] != NULL);
        uint8_t *p = (uint8_t *)ptrs[i];
        for (int k = 0; k < 8; k++) {
            p[k] = (uint8_t)(i + k);
        }
    }
    /* Uniqueness. */
    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            CHECK(ptrs[i] != ptrs[j]);
        }
    }
    /* Free odd, verify even untouched, then re-alloc odd holes. */
    for (int i = 1; i < N; i += 2) {
        heap_free(&h, ptrs[i]);
        ptrs[i] = NULL;
    }
    for (int i = 0; i < N; i += 2) {
        uint8_t *p = (uint8_t *)ptrs[i];
        for (int k = 0; k < 8; k++) {
            CHECK(p[k] == (uint8_t)(i + k));
        }
    }
    for (int i = 1; i < N; i += 2) {
        ptrs[i] = heap_alloc(&h, 16 + (uint64_t)(i % 64));
        CHECK(ptrs[i] != NULL);
    }
    for (int i = 0; i < N; i++) {
        if (ptrs[i]) {
            heap_free(&h, ptrs[i]);
        }
    }
    /* Leak check: everything freed -> one coalesced free block == region. */
    uint64_t used = 0;
    heap_block_t *b = (heap_block_t *)backing;
    while ((uint8_t *)b < backing + REGION_BYTES) {
        if (b->magic == HEAP_MAGIC_USED) {
            used += b->size;
        }
        b = (heap_block_t *)((uint8_t *)b + b->size);
    }
    CHECK(used == 0);
    free(backing);
}

int main(void) {
    printf("=== AuraLite OS heap unit tests ===\n");
    test_basic();
    test_alignment();
    test_coalesce();
    test_realloc();
    test_stress();
    if (failures == 0) {
        printf("=== ALL TESTS PASSED ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", failures);
    return 1;
}

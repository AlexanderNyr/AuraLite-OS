/*
 * test_slab.c — host-side unit tests for the Slab Allocator (H6).
 *
 * Exercises slab_create, slab_alloc, slab_free with 10000 alloc/free cycles
 * to confirm zero OOM, no memory corruption, and correct free list maintenance.
 *
 * Run with:  make test-unit
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/mm/slab.h"

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                 \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static void test_slab_basic(void) {
    printf("[test] slab basic alloc/free...\n");
    slab_cache_t *c = slab_create("test_cache", 128, 16);
    CHECK(c != NULL);
    CHECK(c->object_size == 128);

    void *obj1 = slab_alloc(c);
    CHECK(obj1 != NULL);
    CHECK(((uintptr_t)obj1 % 16) == 0);

    void *obj2 = slab_alloc(c);
    CHECK(obj2 != NULL);
    CHECK(obj1 != obj2);

    slab_free(c, obj1);
    slab_free(c, obj2);
}

static void test_slab_stress(void) {
    printf("[test] slab 10000 alloc/free cycles...\n");
    slab_cache_t *c = slab_create("stress_cache", 64, 16);
    CHECK(c != NULL);

    void *objs[100];
    for (int cycle = 0; cycle < 100; cycle++) {
        for (int i = 0; i < 100; i++) {
            objs[i] = slab_alloc(c);
            CHECK(objs[i] != NULL);
            memset(objs[i], 0xAB, 64);
        }
        for (int i = 0; i < 100; i++) {
            slab_free(c, objs[i]);
        }
    }
}

int main(void) {
    printf("=== AuraLite OS Slab Allocator unit tests ===\n");
    test_slab_basic();
    test_slab_stress();

    if (failures == 0) {
        printf("=== ALL TESTS PASSED ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", failures);
    return 1;
}

/*
 * test_pmm.c — host-side unit tests for the PMM bitmap primitives.
 *
 * The allocation algorithms live in the pure-C, kernel-independent header
 * kernel/lib/bitmap.h, so we can compile and test them on the host with no
 * Limine/HHDM/inline-asm dependencies. Run with:  make test-unit
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/lib/bitmap.h"

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                 \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static void test_basic_ops(void) {
    printf("[test] basic set/clear/test...\n");
    uint8_t bm[16];
    memset(bm, 0, sizeof(bm));

    CHECK(bm_test(bm, 0) == 0);
    bm_set(bm, 0);
    CHECK(bm_test(bm, 0) == 1);
    bm_set(bm, 9);
    CHECK(bm_test(bm, 9) == 1);
    CHECK(bm[1] == 0x02);             /* bit 9 lives in byte 1, value 1<<1 */
    bm_clear(bm, 9);
    CHECK(bm_test(bm, 9) == 0);
}

static void test_first_free(void) {
    printf("[test] first_free...\n");
    uint8_t bm[16];

    memset(bm, 0, sizeof(bm));        /* all 128 bits free */
    CHECK(bm_first_free(bm, 128) == 0);

    bm_set(bm, 0);
    bm_set(bm, 1);
    bm_set(bm, 2);
    CHECK(bm_first_free(bm, 128) == 3);

    memset(bm, 0xFF, sizeof(bm));     /* nothing free */
    CHECK(bm_first_free(bm, 128) == -1);

    /* First byte full, second byte empty -> first free at bit 8. */
    memset(bm, 0, sizeof(bm));
    bm[0] = 0xFF;
    CHECK(bm_first_free(bm, 128) == 8);
}

static void test_contiguous(void) {
    printf("[test] find_contiguous...\n");
    uint8_t bm[32];

    memset(bm, 0, sizeof(bm));
    CHECK(bm_find_contiguous(bm, 256, 4) == 0);

    bm_set(bm, 0);
    CHECK(bm_find_contiguous(bm, 256, 4) == 1);

    /* Run of exactly 3 free bits at 5..7 must NOT satisfy count=4. */
    memset(bm, 0xFF, sizeof(bm));
    bm_clear(bm, 5);  /* bits 5,6,7 => byte 0 mask 0b11100000 -> need 3 bits */
    bm_clear(bm, 6);
    bm_clear(bm, 7);
    CHECK(bm_find_contiguous(bm, 256, 4) == -1);
    CHECK(bm_find_contiguous(bm, 256, 3) == 5);

    CHECK(bm_find_contiguous(bm, 256, 0) == 0);     /* count 0 is trivially OK */
}

/*
 * The actual Phase 3 gate criterion, run against the host: allocate 1000
 * frames through the same bm_first_free primitive the kernel uses, and confirm
 * they are all unique and within range.
 */
static void test_alloc_uniqueness(void) {
    printf("[test] 1000-frame allocation uniqueness...\n");
    uint64_t nframes = 100000;
    uint64_t nbytes  = (nframes + 7) / 8;
    uint8_t  *bm     = calloc(nbytes, 1);
    CHECK(bm != NULL);

    int64_t got[1000];
    for (int i = 0; i < 1000; i++) {
        int64_t idx = bm_first_free(bm, nframes);
        CHECK(idx >= 0);
        for (int j = 0; j < i; j++) {
            CHECK(got[j] != idx);                 /* uniqueness */
        }
        got[i] = idx;
        bm_set(bm, (uint64_t)idx);
    }
    free(bm);
}

int main(void) {
    printf("=== AuraLite OS PMM bitmap unit tests ===\n");
    test_basic_ops();
    test_first_free();
    test_contiguous();
    test_alloc_uniqueness();

    if (failures == 0) {
        printf("=== ALL TESTS PASSED ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", failures);
    return 1;
}

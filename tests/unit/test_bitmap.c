/*
 * test_bitmap.c — comprehensive unit tests for the PMM bitmap primitives
 * (kernel/lib/bitmap.h).
 *
 * 30+ test cases covering: bm_set, bm_clear, bm_test, bm_first_free,
 * bm_find_contiguous, and edge cases.
 *
 * Run with: make test-unit
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/lib/bitmap.h"

static int passed = 0;
static int failed = 0;
static int test_num = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    int before = failed; \
    name(); \
    test_num++; \
    if (failed == before) passed++; \
} while(0)

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: line %d: %s\n", __LINE__, #cond); \
        failed++; \
    } \
} while(0)

#define CHECK_EQ(actual, expected) do { \
    if ((long long)(actual) != (long long)(expected)) { \
        printf("  FAIL: line %d: %s = %lld, expected %lld\n", \
               __LINE__, #actual, (long long)(actual), (long long)(expected)); \
        failed++; \
    } \
} while(0)

/* ---- bm_set / bm_clear / bm_test ---- */

TEST(test_set_clear_basic) {
    uint8_t bm[4] = {0};
    bm_set(bm, 0);
    CHECK(bm_test(bm, 0));
    bm_clear(bm, 0);
    CHECK(!bm_test(bm, 0));
}

TEST(test_set_bit_7) {
    uint8_t bm[1] = {0};
    bm_set(bm, 7);
    CHECK_EQ(bm[0], 0x80);
    CHECK(bm_test(bm, 7));
}

TEST(test_set_bit_8) {
    uint8_t bm[2] = {0};
    bm_set(bm, 8);
    CHECK_EQ(bm[1], 0x01);
    CHECK(bm_test(bm, 8));
}

TEST(test_set_bit_15) {
    uint8_t bm[2] = {0};
    bm_set(bm, 15);
    CHECK_EQ(bm[1], 0x80);
    CHECK(bm_test(bm, 15));
}

TEST(test_set_all_bits_in_byte) {
    uint8_t bm[1] = {0};
    for (int i = 0; i < 8; i++) bm_set(bm, i);
    CHECK_EQ((unsigned char)bm[0], 0xFF);
}

TEST(test_clear_all_bits) {
    uint8_t bm[1] = {0xFF};
    for (int i = 0; i < 8; i++) bm_clear(bm, i);
    CHECK_EQ(bm[0], 0x00);
}

TEST(test_set_clear_multiple) {
    uint8_t bm[4] = {0};
    bm_set(bm, 3);
    bm_set(bm, 7);
    bm_set(bm, 11);
    bm_set(bm, 31);
    CHECK(bm_test(bm, 3));
    CHECK(bm_test(bm, 7));
    CHECK(bm_test(bm, 11));
    CHECK(bm_test(bm, 31));
    CHECK(!bm_test(bm, 0));
    CHECK(!bm_test(bm, 15));
    bm_clear(bm, 11);
    CHECK(!bm_test(bm, 11));
    CHECK(bm_test(bm, 7));
}

TEST(test_set_idempotent) {
    uint8_t bm[1] = {0};
    bm_set(bm, 3);
    bm_set(bm, 3);
    bm_set(bm, 3);
    CHECK_EQ(bm[0], 0x08);
}

TEST(test_clear_idempotent) {
    uint8_t bm[1] = {0};
    bm_clear(bm, 3);
    bm_clear(bm, 3);
    CHECK_EQ(bm[0], 0x00);
}

/* ---- bm_first_free ---- */

TEST(test_first_free_empty) {
    uint8_t bm[4] = {0};
    CHECK_EQ(bm_first_free(bm, 32), 0);
}

TEST(test_first_free_first_used) {
    uint8_t bm[4] = {0};
    bm_set(bm, 0);
    CHECK_EQ(bm_first_free(bm, 32), 1);
}

TEST(test_first_free_first_byte_full) {
    uint8_t bm[4] = {0xFF, 0, 0, 0};
    CHECK_EQ(bm_first_free(bm, 32), 8);
}

TEST(test_first_free_all_used) {
    uint8_t bm[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_EQ(bm_first_free(bm, 32), -1);
}

TEST(test_first_free_partial) {
    /* byte 2 = 0xF0 = 11110000, bits 16-19 free, bits 20-23 used */
    uint8_t bm[4] = {0xFF, 0xFF, 0xF0, 0};
    CHECK_EQ(bm_first_free(bm, 32), 16);
}

TEST(test_first_free_last_bit) {
    /* byte 1 = 0xFE = 11111110, bit 8 is free */
    uint8_t bm[2] = {0xFF, 0xFE};
    CHECK_EQ(bm_first_free(bm, 16), 8);
}

TEST(test_first_free_tail) {
    uint8_t bm[3] = {0xFF, 0xFF, 0xFE};
    CHECK_EQ(bm_first_free(bm, 23), 16);
}

TEST(test_first_free_large_bitmap) {
    static uint8_t bm[512];
    memset(bm, 0xFF, 512);
    bm_clear(bm, 4095);
    CHECK_EQ(bm_first_free(bm, 4096), 4095);
}

TEST(test_first_free_after_alloc) {
    uint8_t bm[4] = {0};
    bm_set(bm, 0); bm_set(bm, 1); bm_set(bm, 2);
    CHECK_EQ(bm_first_free(bm, 32), 3);
    bm_set(bm, 3);
    CHECK_EQ(bm_first_free(bm, 32), 4);
}

/* ---- bm_find_contiguous ---- */

TEST(test_contiguous_all_free) {
    uint8_t bm[4] = {0};
    CHECK_EQ(bm_find_contiguous(bm, 32, 4), 0);
}

TEST(test_contiguous_at_offset) {
    uint8_t bm[4] = {0};
    bm_set(bm, 0); bm_set(bm, 1);
    CHECK_EQ(bm_find_contiguous(bm, 32, 4), 2);
}

TEST(test_contiguous_exact_fit) {
    uint8_t bm[8] = {0};
    for (int i = 0; i < 48; i++) bm_set(bm, i);
    CHECK_EQ(bm_find_contiguous(bm, 64, 16), 48);
}

TEST(test_contiguous_none) {
    uint8_t bm[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_EQ(bm_find_contiguous(bm, 32, 2), -1);
}

TEST(test_contiguous_count_1) {
    uint8_t bm[4] = {0};
    bm_set(bm, 5);
    CHECK_EQ(bm_find_contiguous(bm, 32, 1), 0);
}

TEST(test_contiguous_count_0) {
    uint8_t bm[4] = {0xFF};
    CHECK_EQ(bm_find_contiguous(bm, 32, 0), 0);
}

TEST(test_contiguous_spanning_bytes) {
    uint8_t bm[4] = {0x01, 0, 0, 0};
    bm_set(bm, 0);
    /* bits 1-7 free, then bits 8-15 free => run of 15 */
    CHECK_EQ(bm_find_contiguous(bm, 32, 8), 1);
}

TEST(test_contiguous_max_run) {
    uint8_t bm[4] = {0};
    /* Set bit 16 to split */
    bm_set(bm, 16);
    CHECK_EQ(bm_find_contiguous(bm, 32, 16), 0);
    CHECK_EQ(bm_find_contiguous(bm, 32, 17), -1);
}

TEST(test_contiguous_large) {
    static uint8_t bm[64];
    memset(bm, 0, 64);
    bm_set(bm, 0);
    CHECK_EQ(bm_find_contiguous(bm, 512, 256), 1);
}

/* ---- Edge cases ---- */

TEST(test_single_bit_bitmap) {
    uint8_t bm[1] = {0};
    CHECK_EQ(bm_first_free(bm, 1), 0);
    bm_set(bm, 0);
    CHECK_EQ(bm_first_free(bm, 1), -1);
}

TEST(test_non_byte_aligned_count) {
    /* byte 1 = 0x80 = 10000000: bit 8 is clear (free), bit 15 is set */
    uint8_t bm[2] = {0xFF, 0x80};
    /* For 9 bits (0-8): bit 8 is free */
    CHECK_EQ(bm_first_free(bm, 9), 8);
    /* For 10 bits (0-9): bit 8 is free */
    CHECK_EQ(bm_first_free(bm, 10), 8);
}

TEST(test_alternating_pattern) {
    uint8_t bm[2] = {0};
    for (int i = 0; i < 16; i += 2) bm_set(bm, i);
    /* free bits: 1, 3, 5, 7, 9, 11, 13, 15 */
    CHECK_EQ(bm_first_free(bm, 16), 1);
    CHECK_EQ(bm_find_contiguous(bm, 16, 2), -1);
    CHECK_EQ(bm_find_contiguous(bm, 16, 1), 1);
}

TEST(test_stress_random) {
    static uint8_t bm[128];
    srand(42);
    /* Randomly set/clear bits and verify first_free is actually free. */
    for (int iter = 0; iter < 1000; iter++) {
        int idx = rand() % 1024;
        if (rand() & 1) bm_set(bm, idx); else bm_clear(bm, idx);
    }
    int64_t ff = bm_first_free(bm, 1024);
    if (ff >= 0) {
        CHECK(!bm_test(bm, ff));
    }
}

int main(void) {
    printf("=== Bitmap Primitive Tests ===\n\n");

    printf("--- set/clear/test ---\n");
    RUN(test_set_clear_basic);
    RUN(test_set_bit_7);
    RUN(test_set_bit_8);
    RUN(test_set_bit_15);
    RUN(test_set_all_bits_in_byte);
    RUN(test_clear_all_bits);
    RUN(test_set_clear_multiple);
    RUN(test_set_idempotent);
    RUN(test_clear_idempotent);

    printf("--- first_free ---\n");
    RUN(test_first_free_empty);
    RUN(test_first_free_first_used);
    RUN(test_first_free_first_byte_full);
    RUN(test_first_free_all_used);
    RUN(test_first_free_partial);
    RUN(test_first_free_last_bit);
    RUN(test_first_free_tail);
    RUN(test_first_free_large_bitmap);
    RUN(test_first_free_after_alloc);

    printf("--- find_contiguous ---\n");
    RUN(test_contiguous_all_free);
    RUN(test_contiguous_at_offset);
    RUN(test_contiguous_exact_fit);
    RUN(test_contiguous_none);
    RUN(test_contiguous_count_1);
    RUN(test_contiguous_count_0);
    RUN(test_contiguous_spanning_bytes);
    RUN(test_contiguous_max_run);
    RUN(test_contiguous_large);

    printf("--- edge cases ---\n");
    RUN(test_single_bit_bitmap);
    RUN(test_non_byte_aligned_count);
    RUN(test_alternating_pattern);
    RUN(test_stress_random);

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           passed, test_num, failed);
    return failed ? 1 : 0;
}

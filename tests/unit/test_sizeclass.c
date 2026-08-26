/* test_sizeclass.c — host gate for the O6 size-class cache core
 * (OPT_PLAN.md O6; kernel/mm/sizeclass.h).
 *
 * The cache's correctness rests on four pieces of arithmetic: the
 * request→class mapping (round UP), the payload→class mapping (round
 * DOWN — a 48-byte block must never be handed to a 64-byte request),
 * the LIFO push/pop with its in-payload link, and the cap.  This test
 * pins each boundary and then runs an adversarial interleaving against
 * a naive model.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/mm/sizeclass.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, ...) do {                       \
        if (cond) { passed++; }                     \
        else {                                      \
            failed++;                               \
            printf("FAIL: ");                       \
            printf(__VA_ARGS__);                    \
            printf("\n");                           \
        }                                           \
    } while (0)

static void test_request_mapping(void) {
    CHECK(sizeclass_for_request(0)  == -1, "request 0 -> no class");
    CHECK(sizeclass_for_request(1)  == 0,  "request 1 -> class 16");
    CHECK(sizeclass_for_request(16) == 0,  "request 16 -> class 16 (boundary)");
    CHECK(sizeclass_for_request(17) == 1,  "request 17 -> class 32");
    CHECK(sizeclass_for_request(4096) == (int)SIZECLASS_COUNT - 1,
          "request 4096 -> largest class (boundary)");
    CHECK(sizeclass_for_request(4097) == -1,
          "request 4097 -> first-fit territory");
    for (uint32_t ci = 0; ci < SIZECLASS_COUNT; ci++) {
        uint64_t b = sizeclass_bytes(ci);
        CHECK(sizeclass_for_request(b) == (int)ci,
              "exact class size %llu maps to itself", (unsigned long long)b);
    }
}

static void test_payload_mapping(void) {
    CHECK(sizeclass_for_payload(15) == -1, "payload 15 serves nothing");
    CHECK(sizeclass_for_payload(16) == 0,  "payload 16 serves class 16");
    CHECK(sizeclass_for_payload(48) == 1,
          "payload 48 rounds DOWN to class 32 — never up");
    CHECK(sizeclass_for_payload(4095) == (int)SIZECLASS_COUNT - 2,
          "payload 4095 serves class 2048");
    /* SELFHOST SH1: oversized blocks must NOT be parked in the largest
     * class -- the O6 leak that exhausted the 64 MiB heap one
     * SPAWN_MAX_IMAGE buffer per spawn.  Blocks within the class's own
     * range (payload < 2x class) still recycle; anything beyond falls
     * through to heap_free(). */
    CHECK(sizeclass_for_payload(8191) == (int)SIZECLASS_COUNT - 1,
          "payload 8191 still recycles in the 4 KiB class (its own range)");
    CHECK(sizeclass_for_payload(8192) == -1,
          "payload 8192 falls through to the heap (no larger class)");
    CHECK(sizeclass_for_payload(1 << 20) == -1,
          "huge payload (e.g. a freed 1 MiB+ image buffer) goes to heap_free");
    CHECK(sizeclass_for_payload(16 << 20) == -1,
          "16 MiB SPAWN_MAX_IMAGE buffer goes to heap_free (the leak gate)");
}

static void test_lifo_and_link_scrub(void) {
    sizeclass_cache_t c;
    memset(&c, 0, sizeof(c));
    static uint64_t obj_a[8], obj_b[8];

    sizeclass_push(&c, 2, obj_a);
    sizeclass_push(&c, 2, obj_b);
    CHECK(c.count[2] == 2, "count tracks pushes");
    CHECK(sizeclass_pop(&c, 2) == obj_b, "LIFO: last in, first out");
    CHECK(sizeclass_pop(&c, 2) == obj_a, "LIFO: then the first");
    CHECK(obj_a[0] == 0 && obj_b[0] == 0,
          "pop scrubs the link word (recycled memory stays zeroed)");
    CHECK(sizeclass_pop(&c, 2) == NULL, "empty class pops NULL");
    CHECK(c.count[2] == 0, "count returns to zero");
    /* Classes are independent. */
    sizeclass_push(&c, 0, obj_a);
    CHECK(sizeclass_pop(&c, 5) == NULL, "class isolation");
    CHECK(sizeclass_pop(&c, 0) == obj_a, "the object stayed in its class");
}

/* Adversarial interleaving against a naive model: random push/pop over
 * three classes; the cache must agree with a stack-per-class model at
 * every step. */
static void test_model_interleave(void) {
    enum { OBJS = 128, STEPS = 20000 };
    static uint64_t storage[OBJS][8];
    static void *model[SIZECLASS_COUNT][OBJS];
    static int    model_n[SIZECLASS_COUNT];
    static int    in_cache[OBJS];

    sizeclass_cache_t c;
    memset(&c, 0, sizeof(c));
    memset(model_n, 0, sizeof(model_n));
    memset(in_cache, 0, sizeof(in_cache));

    unsigned rng = 12345;
    int next_free = 0;
    for (int step = 0; step < STEPS; step++) {
        rng = rng * 1103515245 + 12345;
        uint32_t ci = (rng >> 8) % 3;          /* classes 0..2 */
        if ((rng >> 16) & 1) {
            /* push a not-yet-cached object */
            int oi = -1;
            for (int k = 0; k < OBJS; k++) {
                int cand = (next_free + k) % OBJS;
                if (!in_cache[cand]) { oi = cand; break; }
            }
            if (oi < 0) continue;              /* everything cached */
            next_free = (oi + 1) % OBJS;
            in_cache[oi] = 1;
            sizeclass_push(&c, ci, storage[oi]);
            model[ci][model_n[ci]++] = storage[oi];
        } else {
            void *got = sizeclass_pop(&c, ci);
            void *want = model_n[ci] ? model[ci][--model_n[ci]] : NULL;
            if (model_n[ci] < 0) model_n[ci] = 0;
            if (got != want) {
                CHECK(0, "model divergence at step %d (got %p want %p)",
                      step, got, want);
                return;
            }
            if (got) {
                in_cache[((uint64_t *)got - &storage[0][0]) / 8] = 0;
            }
        }
        if (c.count[ci] != (uint32_t)model_n[ci]) {
            CHECK(0, "count divergence at step %d", step);
            return;
        }
    }
    CHECK(1, "20000-step adversarial interleave matches the model");
}

int main(void) {
    printf("=== O6 size-class cache test suite ===\n\n");

    test_request_mapping();
    test_payload_mapping();
    test_lifo_and_link_scrub();
    test_model_interleave();

    printf("\n%d passed, %d failed\n", passed, failed);
    if (failed == 0) printf("=== ALL TESTS PASSED ===\n");
    return failed ? 1 : 0;
}

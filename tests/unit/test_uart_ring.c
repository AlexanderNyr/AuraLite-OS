/* test_uart_ring.c — host gate for the O3 TX ring index core
 * (OPT_PLAN.md O3; drivers/uart/uart_ring.h).
 *
 * The ring's counters are free-running uint32s and the buffer index is
 * `counter & (size-1)` — which means the three classic ring bugs (wrap,
 * full-vs-empty ambiguity, off-by-one at the boundary) all live in about
 * four lines of arithmetic.  This test pins them down, including the
 * case the kernel will not hit for months: head/tail crossing the 2^32
 * boundary mid-stream.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../drivers/uart/uart_ring.h"

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

#define SZ 16u

static void test_empty_full_boundaries(void) {
    uint8_t buf[SZ];
    uart_ring_t r = {0, 0};

    CHECK(uring_empty(&r), "fresh ring is empty");
    CHECK(!uring_full(&r, SZ), "fresh ring is not full");
    CHECK(uring_count(&r) == 0, "fresh count is 0");

    /* Fill to exactly full. */
    for (uint32_t i = 0; i < SZ; i++) {
        CHECK(!uring_full(&r, SZ), "not full before push %u", i);
        uring_push(&r, buf, SZ, (uint8_t)i);
    }
    CHECK(uring_full(&r, SZ), "full after SZ pushes");
    CHECK(!uring_empty(&r), "full ring is not empty");
    CHECK(uring_count(&r) == SZ, "count == SZ at full");

    /* Drain to exactly empty, FIFO order. */
    for (uint32_t i = 0; i < SZ; i++) {
        CHECK(!uring_empty(&r), "not empty before pop %u", i);
        uint8_t b = uring_pop(&r, buf, SZ);
        CHECK(b == (uint8_t)i, "FIFO order at %u (got %u)", i, b);
    }
    CHECK(uring_empty(&r), "empty after SZ pops");
    CHECK(uring_count(&r) == 0, "count 0 after drain");
}

static void test_interleaved_wraparound(void) {
    /* Push 3 / pop 2 repeatedly: indices lap the buffer many times while
     * the count stays small — the mask arithmetic is what's on trial. */
    uint8_t buf[SZ];
    uart_ring_t r = {0, 0};
    uint8_t next_in = 0, next_out = 0;

    for (int round = 0; round < 1000; round++) {
        for (int i = 0; i < 3; i++) {
            if (!uring_full(&r, SZ)) {
                uring_push(&r, buf, SZ, next_in++);
            }
        }
        for (int i = 0; i < 2; i++) {
            if (!uring_empty(&r)) {
                uint8_t b = uring_pop(&r, buf, SZ);
                if (b != next_out) {
                    CHECK(0, "order lost at round %d (got %u want %u)",
                          round, b, next_out);
                    return;
                }
                next_out++;
            }
        }
    }
    CHECK(1, "1000 interleaved rounds keep FIFO order");
    CHECK(uring_count(&r) == (uint32_t)(uint8_t)(next_in - next_out) ||
          uring_count(&r) == (uint32_t)(next_in - next_out),
          "count consistent after interleave");
}

static void test_counter_wrap_2_32(void) {
    /* Start both counters just below the uint32 wrap: count arithmetic
     * must survive head wrapping to 0 while tail is still near 2^32. */
    uint8_t buf[SZ];
    uart_ring_t r;
    r.head = 0xFFFFFFF8u;
    r.tail = 0xFFFFFFF8u;

    CHECK(uring_empty(&r), "empty at 2^32 - 8");

    for (uint32_t i = 0; i < 12; i++) {          /* crosses the wrap */
        uring_push(&r, buf, SZ, (uint8_t)(0x40 + i));
    }
    CHECK(uring_count(&r) == 12, "count 12 across the 2^32 wrap (head=%u)",
          r.head);
    CHECK(r.head == 4u, "head wrapped to 4");

    for (uint32_t i = 0; i < 12; i++) {
        uint8_t b = uring_pop(&r, buf, SZ);
        CHECK(b == (uint8_t)(0x40 + i), "wrap-crossing FIFO order at %u", i);
    }
    CHECK(uring_empty(&r), "empty again after the wrap crossing");
    CHECK(r.tail == r.head, "tail caught head across the wrap");
}

int main(void) {
    printf("=== O3 UART TX ring index test suite ===\n\n");

    test_empty_full_boundaries();
    test_interleaved_wraparound();
    test_counter_wrap_2_32();

    printf("\n%d passed, %d failed\n", passed, failed);
    if (failed == 0) printf("=== ALL TESTS PASSED ===\n");
    return failed ? 1 : 0;
}

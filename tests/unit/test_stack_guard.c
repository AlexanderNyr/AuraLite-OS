/*
 * test_stack_guard.c — host unit tests for stack guard-page classification.
 *
 * guard.c cannot be linked directly here (it pulls in the scheduler and TCB),
 * so this test re-implements the same address-window arithmetic and validates
 * the classification boundaries that guard_classify_fault() relies on:
 *
 *   - The user-stack guard window sits just below the lowest possible mapped
 *     stack byte (entropy slack + one guard page) and ends at USER_STACK_TOP.
 *   - The kernel-stack low/high guard pages exactly bracket a thread's mapped
 *     slot, with guard width == THREAD_STACK_GUARD_PAGES pages.
 *
 * If guard.c / the loader change these layouts, these expectations must change.
 */

#include <stdint.h>
#include <stdio.h>

/* ---- mirrored constants (keep in sync with guard.c / process.c / thread.h) */
#define USER_STACK_TOP         0x7FFFF0000000ULL
#define USER_STACK_SIZE        0x10000ULL
#define USER_STACK_GUARD_SIZE  0x1000ULL
#define USER_STACK_ENTROPY     (0x10ULL * 0x1000ULL)
#define PAGE_SIZE              0x1000ULL
#define THREAD_STACK_SIZE      (16 * 1024)
#define THREAD_STACK_GUARD_PAGES 1

enum guard_fault_kind {
    GUARD_FAULT_NONE = 0,
    GUARD_FAULT_KERNEL_STACK_LOW,
    GUARD_FAULT_KERNEL_STACK_HIGH,
    GUARD_FAULT_USER_STACK,
};

/* Mirror of the user-stack branch. */
static enum guard_fault_kind classify_user(uint64_t cr2) {
    uint64_t stack_low_min = USER_STACK_TOP - USER_STACK_ENTROPY - USER_STACK_SIZE;
    uint64_t guard_low = stack_low_min - USER_STACK_GUARD_SIZE;
    if (cr2 >= guard_low && cr2 < USER_STACK_TOP) return GUARD_FAULT_USER_STACK;
    return GUARD_FAULT_NONE;
}

/* Mirror of the kernel-stack branch for a slot at [region, usable, ...]. */
static enum guard_fault_kind classify_kernel(uint64_t cr2, uint64_t region) {
    uint64_t guard_bytes = (uint64_t)THREAD_STACK_GUARD_PAGES * PAGE_SIZE;
    uint64_t usable = region + guard_bytes;
    uint64_t low_lo = region, low_hi = usable;
    uint64_t high_lo = usable + THREAD_STACK_SIZE;
    uint64_t high_hi = high_lo + guard_bytes;
    if (cr2 >= low_lo && cr2 < low_hi)   return GUARD_FAULT_KERNEL_STACK_LOW;
    if (cr2 >= high_lo && cr2 < high_hi) return GUARD_FAULT_KERNEL_STACK_HIGH;
    return GUARD_FAULT_NONE;
}

static int passed = 0, failed = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } else { passed++; } } while (0)

int main(void) {
    printf("=== stack guard-page classification tests ===\n\n");

    /* ---- user stack window ---- */
    uint64_t stack_low_min = USER_STACK_TOP - USER_STACK_ENTROPY - USER_STACK_SIZE;
    uint64_t guard_low = stack_low_min - USER_STACK_GUARD_SIZE;

    /* A byte inside the guard page just below the lowest stack base is caught. */
    CHECK(classify_user(guard_low) == GUARD_FAULT_USER_STACK);
    CHECK(classify_user(stack_low_min - 1) == GUARD_FAULT_USER_STACK);
    /* Just below the guard window is NOT classified (some other region). */
    CHECK(classify_user(guard_low - 1) == GUARD_FAULT_NONE);
    /* The top boundary is exclusive. */
    CHECK(classify_user(USER_STACK_TOP) == GUARD_FAULT_NONE);
    CHECK(classify_user(USER_STACK_TOP - 1) == GUARD_FAULT_USER_STACK);
    /* A normal heap/low address is never a user-stack guard hit. */
    CHECK(classify_user(0x400000) == GUARD_FAULT_NONE);

    /* ---- kernel stack guards for a representative slot ---- */
    uint64_t region = 0xFFFFFFFF8A000000ULL; /* slot 0 base */
    uint64_t usable = region + PAGE_SIZE;     /* 1 guard page */

    /* Low guard page = the page below the usable stack. */
    CHECK(classify_kernel(region, region) == GUARD_FAULT_KERNEL_STACK_LOW);
    CHECK(classify_kernel(usable - 1, region) == GUARD_FAULT_KERNEL_STACK_LOW);
    /* The first usable byte is NOT a guard hit. */
    CHECK(classify_kernel(usable, region) == GUARD_FAULT_NONE);
    /* The last usable byte is NOT a guard hit. */
    CHECK(classify_kernel(usable + THREAD_STACK_SIZE - 1, region) == GUARD_FAULT_NONE);
    /* High guard page = the page just above the usable stack. */
    CHECK(classify_kernel(usable + THREAD_STACK_SIZE, region) == GUARD_FAULT_KERNEL_STACK_HIGH);
    CHECK(classify_kernel(usable + THREAD_STACK_SIZE + PAGE_SIZE - 1, region) == GUARD_FAULT_KERNEL_STACK_HIGH);
    /* Beyond the high guard page is unclassified. */
    CHECK(classify_kernel(usable + THREAD_STACK_SIZE + PAGE_SIZE, region) == GUARD_FAULT_NONE);

    /* Slot width sanity: guard + usable + guard. */
    CHECK((THREAD_STACK_SIZE / PAGE_SIZE) + 2 * THREAD_STACK_GUARD_PAGES ==
          (16384 / 4096) + 2);

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}

/* test_tlb_policy.c — host gate for the O5 shootdown decision core
 * (OPT_PLAN.md O5; kernel/arch/x86_64/tlb_policy.h).
 *
 * The whole protocol's correctness argument leans on two pure
 * functions: what a handler does with a seq/payload pair, and whether
 * a sender may skip a target.  Both degrade toward FULL/send — this
 * test pins the boundaries where degrading is mandatory:
 *
 *   - seq gaps (collapsed IPIs) of exactly 2, and across uint64 wrap;
 *   - the npages boundaries 0 / 1 / TLB_INVLPG_MAX / +1;
 *   - spurious redelivery (seq == last);
 *   - the skip filter's three-way truth table (kernel broadcast,
 *     unknown CR3, match/mismatch).
 */
#include <stdint.h>
#include <stdio.h>

#include "../../kernel/arch/x86_64/tlb_policy.h"

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

static void test_decide(void) {
    /* Spurious: already handled. */
    CHECK(tlb_policy_decide(7, 7, 1, TLB_INVLPG_MAX) == TLB_ACT_SPURIOUS,
          "seq == last is spurious");

    /* Clean +1 step, page counts inside the window. */
    CHECK(tlb_policy_decide(8, 7, 1, TLB_INVLPG_MAX) == TLB_ACT_RANGED,
          "one page -> ranged");
    CHECK(tlb_policy_decide(8, 7, TLB_INVLPG_MAX, TLB_INVLPG_MAX)
              == TLB_ACT_RANGED,
          "exactly INVLPG_MAX pages -> ranged");

    /* Degradations. */
    CHECK(tlb_policy_decide(8, 7, 0, TLB_INVLPG_MAX) == TLB_ACT_FULL,
          "npages 0 (whole space) -> full");
    CHECK(tlb_policy_decide(8, 7, TLB_INVLPG_MAX + 1, TLB_INVLPG_MAX)
              == TLB_ACT_FULL,
          "INVLPG_MAX+1 pages -> full");
    CHECK(tlb_policy_decide(9, 7, 1, TLB_INVLPG_MAX) == TLB_ACT_FULL,
          "seq gap of 2 (collapsed IPI) -> full even for one page");
    CHECK(tlb_policy_decide(1000, 7, 1, TLB_INVLPG_MAX) == TLB_ACT_FULL,
          "large seq gap -> full");

    /* seq wrap across 2^64: unsigned subtraction keeps the +1 truth. */
    CHECK(tlb_policy_decide(0, UINT64_MAX, 1, TLB_INVLPG_MAX)
              == TLB_ACT_RANGED,
          "seq wrap 2^64-1 -> 0 is a clean +1 step");
    CHECK(tlb_policy_decide(1, UINT64_MAX, 1, TLB_INVLPG_MAX)
              == TLB_ACT_FULL,
          "seq wrap with a skipped request -> full");
}

static void test_target_filter(void) {
    /* Kernel-shared range: everyone is a target, no exceptions. */
    CHECK(tlb_policy_target_wanted(0, 0)      == 1, "kernel range, unknown cpu");
    CHECK(tlb_policy_target_wanted(0, 0x1000) == 1, "kernel range, any cpu");

    /* Unknown target CR3: assume the worst. */
    CHECK(tlb_policy_target_wanted(0x5000, 0) == 1, "unknown target -> send");

    /* The architectural skip: a different CR3 cannot hold our entries. */
    CHECK(tlb_policy_target_wanted(0x5000, 0x5000) == 1, "same space -> send");
    CHECK(tlb_policy_target_wanted(0x5000, 0x9000) == 0, "other space -> skip");
}

int main(void) {
    printf("=== O5 TLB shootdown policy test suite ===\n\n");

    test_decide();
    test_target_filter();

    printf("\n%d passed, %d failed\n", passed, failed);
    if (failed == 0) printf("=== ALL TESTS PASSED ===\n");
    return failed ? 1 : 0;
}

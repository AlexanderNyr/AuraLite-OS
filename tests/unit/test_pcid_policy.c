/* test_pcid_policy.c — host gate for the R11 PCID decision core
 * (RESIDUE_PLAN R11; kernel/arch/x86_64/pcid_policy.h).
 *
 * The same split as test_tlb_policy.c, for the same reason made
 * SHARPER this phase: QEMU TCG does not implement PCID at all
 * (measured: `-cpu qemu64,+pcid` warns "TCG doesn't support requested
 * feature" and boots pcid=0), so NO integration lane can execute the
 * allocation/switch/filter decisions.  This host test is the only rig
 * those decisions have until the user's WHPX machine (pcid=1,
 * invpcid=0 — the D-PCID-5 trigger) runs the receipt block.
 *
 * What is pinned:
 *   - FRESH on first entry, NOFLUSH on re-entry (D-PCID-3);
 *   - the kernel's slot-0 reservation (D-PCID-1);
 *   - slot collisions: two PML4s hashing to one slot evict each other
 *     (both FRESH), never share a NOFLUSH right;
 *   - the generation bump revokes every NOFLUSH right (D-PCID-2) and
 *     rights come back per-space on the next (FRESH) entry;
 *   - de-own forces exactly the victim FRESH, leaves others alone;
 *   - the sender filter's four-way truth table (D-PCID-4): resident →
 *     IPI, non-owner → skip, stale generation → skip, live right →
 *     IPI.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../kernel/arch/x86_64/pcid_policy.h"

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

/* Two user PML4s that land in DIFFERENT slots, and a third that
 * collides with the first by construction: slots hash on
 * (pml4 >> 12) % 255, so adding 255 << 12 collides. */
#define AS_A   0x0000000001000ULL
#define AS_B   0x0000000002000ULL
#define AS_A2  (AS_A + (255ULL << 12))   /* same slot as AS_A */
#define KPML4  0x00000000ff000ULL

static void test_enter_basic(void) {
    struct pcid_cpu_state st;
    memset(&st, 0, sizeof st);
    uint32_t sa = pcid_policy_slot(AS_A);
    uint32_t sb = pcid_policy_slot(AS_B);

    CHECK(sa != 0 && sb != 0, "user slots never take the kernel's 0");
    CHECK(sa != sb, "test fixture: AS_A and AS_B in distinct slots");
    CHECK(pcid_policy_slot(AS_A2) == sa, "fixture: AS_A2 collides with AS_A");

    CHECK(pcid_policy_enter(&st, sa, AS_A) == PCID_ENTRY_FRESH,
          "first entry is FRESH");
    CHECK(pcid_policy_enter(&st, sa, AS_A) == PCID_ENTRY_NOFLUSH,
          "re-entry is NOFLUSH (the D-PCID-3 win)");
    CHECK(pcid_policy_enter(&st, sb, AS_B) == PCID_ENTRY_FRESH,
          "another space's first entry is FRESH");
    CHECK(pcid_policy_enter(&st, sa, AS_A) == PCID_ENTRY_NOFLUSH,
          "A's right survives B's entry in another slot");

    /* Kernel slot 0 behaves like any other slot: FRESH then NOFLUSH. */
    CHECK(pcid_policy_enter(&st, 0, KPML4) == PCID_ENTRY_FRESH,
          "kernel slot 0: first entry FRESH");
    CHECK(pcid_policy_enter(&st, 0, KPML4) == PCID_ENTRY_NOFLUSH,
          "kernel slot 0: re-entry NOFLUSH");
}

static void test_collision(void) {
    struct pcid_cpu_state st;
    memset(&st, 0, sizeof st);
    uint32_t s = pcid_policy_slot(AS_A);

    CHECK(pcid_policy_enter(&st, s, AS_A) == PCID_ENTRY_FRESH,  "A fresh");
    CHECK(pcid_policy_enter(&st, s, AS_A2) == PCID_ENTRY_FRESH,
          "collider evicts: A2 enters FRESH, never NOFLUSH");
    CHECK(pcid_policy_enter(&st, s, AS_A) == PCID_ENTRY_FRESH,
          "evicted A re-enters FRESH (its entries died at eviction)");
    CHECK(pcid_policy_enter(&st, s, AS_A) == PCID_ENTRY_NOFLUSH,
          "then A's right is live again");
}

static void test_generation(void) {
    struct pcid_cpu_state st;
    memset(&st, 0, sizeof st);
    uint32_t sa = pcid_policy_slot(AS_A);
    uint32_t sb = pcid_policy_slot(AS_B);

    (void)pcid_policy_enter(&st, sa, AS_A);
    (void)pcid_policy_enter(&st, sb, AS_B);
    pcid_policy_gen_bump(&st);
    CHECK(pcid_policy_enter(&st, sa, AS_A) == PCID_ENTRY_FRESH,
          "gen bump revokes A's NOFLUSH");
    CHECK(pcid_policy_enter(&st, sb, AS_B) == PCID_ENTRY_FRESH,
          "gen bump revokes B's NOFLUSH");
    CHECK(pcid_policy_enter(&st, sa, AS_A) == PCID_ENTRY_NOFLUSH,
          "post-bump re-entry restores the right");
}

static void test_deown(void) {
    struct pcid_cpu_state st;
    memset(&st, 0, sizeof st);
    uint32_t sa = pcid_policy_slot(AS_A);
    uint32_t sb = pcid_policy_slot(AS_B);

    (void)pcid_policy_enter(&st, sa, AS_A);
    (void)pcid_policy_enter(&st, sb, AS_B);
    pcid_policy_deown(&st, sa, AS_A);
    CHECK(pcid_policy_enter(&st, sa, AS_A) == PCID_ENTRY_FRESH,
          "de-owned victim re-enters FRESH");
    CHECK(pcid_policy_enter(&st, sb, AS_B) == PCID_ENTRY_NOFLUSH,
          "de-own is narrow: B's right untouched");

    /* De-owning a slot someone else owns is a no-op. */
    (void)pcid_policy_enter(&st, sa, AS_A2);       /* A2 takes the slot */
    pcid_policy_deown(&st, sa, AS_A);              /* stale victim id   */
    CHECK(pcid_policy_enter(&st, sa, AS_A2) == PCID_ENTRY_NOFLUSH,
          "de-own with a stale owner id leaves the new owner's right");
}

static void test_sender_filter(void) {
    struct pcid_cpu_state st;
    memset(&st, 0, sizeof st);
    uint32_t sa = pcid_policy_slot(AS_A);

    /* Resident victim: never skip, whatever the table says. */
    CHECK(pcid_policy_sender_skip(&st, sa, AS_A, AS_A) == 0,
          "resident victim -> IPI");

    /* Never entered: not the owner -> skip. */
    CHECK(pcid_policy_sender_skip(&st, sa, AS_A, AS_B) == 1,
          "non-owner (never entered) -> skip");

    /* Live right, not resident: must IPI (the D-PCID-4 inversion —
     * exactly the case the O5 filter got away with skipping). */
    (void)pcid_policy_enter(&st, sa, AS_A);
    CHECK(pcid_policy_sender_skip(&st, sa, AS_A, AS_B) == 0,
          "live NOFLUSH right, non-resident -> IPI");

    /* Stale generation: skip (the wrap's lazy flush covers it). */
    pcid_policy_gen_bump(&st);
    CHECK(pcid_policy_sender_skip(&st, sa, AS_A, AS_B) == 1,
          "stale generation -> skip");

    /* Evicted by a collider: skip (eviction flushed). */
    memset(&st, 0, sizeof st);
    (void)pcid_policy_enter(&st, sa, AS_A);
    (void)pcid_policy_enter(&st, sa, AS_A2);
    CHECK(pcid_policy_sender_skip(&st, sa, AS_A, AS_B) == 1,
          "evicted victim -> skip");
}

int main(void) {
    test_enter_basic();
    test_collision();
    test_generation();
    test_deown();
    test_sender_filter();

    printf("test_pcid_policy: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

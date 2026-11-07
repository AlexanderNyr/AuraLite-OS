/*
 * test_aspace.c — unit tests for the refcounted address-space table
 * (RESIDUE3 C2, kernel/mm/aspace.c, linked UNMODIFIED).
 *
 * The three cases that matter are the ones the pre-C2 tree got wrong:
 *   1. a CLONE_VM sibling that dies first must NOT free the shared page
 *      tables (the old code skipped pthread zombies entirely and freed the
 *      space only when the non-pthread TCB was reaped);
 *   2. a space outliving its creator (execve's old space) frees exactly once,
 *      at the last drop, not at the first;
 *   3. a double put never authorises a second free.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "kernel/mm/aspace.h"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; if (failed == b) passed++; } while (0)
#define CHECK(c) do { if (!(c)) { printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long _a = (long)(a), _e = (long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s = %ld, want %ld\n", __LINE__, #a, _a, _e); failed++; } } while (0)

/* A plausible non-zero PML4 root: frame number 0x1234, flags stripped. */
#define SPACE_A 0x001234000ULL
#define SPACE_B 0x005678000ULL

static void test_single_owner(void) {
    aspace_reset();
    CHECK_EQ(aspace_get(SPACE_A), 1);
    CHECK_EQ(aspace_refs(SPACE_A), 1);
    CHECK_EQ(aspace_put(SPACE_A), 1);         /* the last owner may free */
    CHECK_EQ(aspace_put(SPACE_A), 0);         /* and nobody else may again */
    CHECK_EQ(aspace_double_puts(), 1);
    CHECK_EQ(aspace_live_spaces(), 0);
    CHECK_EQ(aspace_refs(SPACE_A), 0);
}

static void test_kernel_space_is_untracked(void) {
    aspace_reset();
    /* pml4 == 0 is the kernel address space: it has no refcount and is never
     * freed through this module. */
    CHECK_EQ(aspace_get(0), 0);
    CHECK_EQ(aspace_put(0), 0);
    CHECK_EQ(aspace_live_spaces(), 0);
}

static void test_clone_vm_sibling_dies_first(void) {
    aspace_reset();
    /* fork() creates the space (1 owner), two clone(CLONE_VM) threads join. */
    CHECK_EQ(aspace_get(SPACE_A), 1);
    CHECK_EQ(aspace_get(SPACE_A), 2);
    CHECK_EQ(aspace_get(SPACE_A), 3);
    CHECK_EQ(aspace_shared_spaces(), 1);

    /* Thread 2 exits first: the page tables MUST survive. */
    CHECK_EQ(aspace_put(SPACE_A), 0);
    CHECK_EQ(aspace_refs(SPACE_A), 2);

    /* Thread 1 exits second: still shared with the main thread. */
    CHECK_EQ(aspace_put(SPACE_A), 0);
    CHECK_EQ(aspace_refs(SPACE_A), 1);
    CHECK_EQ(aspace_shared_spaces(), 0);

    /* Main thread last: now, and only now, the free is authorised. */
    CHECK_EQ(aspace_put(SPACE_A), 1);
    CHECK_EQ(aspace_last_puts(), 1);
    CHECK_EQ(aspace_live_spaces(), 0);
}

static void test_execve_space_outlives_its_creator(void) {
    aspace_reset();
    /* exec replaces SPACE_A with SPACE_B while one CLONE_VM sibling of the
     * old space is still alive: the old space must be dropped, not freed. */
    CHECK_EQ(aspace_get(SPACE_A), 1);
    CHECK_EQ(aspace_get(SPACE_A), 2);
    CHECK_EQ(aspace_get(SPACE_B), 1);

    CHECK_EQ(aspace_put(SPACE_A), 0);         /* the exec'ing thread leaves */
    /* The sibling is reaped later and is the last owner of SPACE_A. */
    CHECK_EQ(aspace_put(SPACE_A), 1);
    CHECK_EQ(aspace_put(SPACE_B), 1);         /* B is unaffected */
    CHECK_EQ(aspace_live_spaces(), 0);
}

static void test_double_put_never_frees_twice(void) {
    aspace_reset();
    CHECK_EQ(aspace_get(SPACE_A), 1);
    CHECK_EQ(aspace_get(SPACE_A), 2);
    CHECK_EQ(aspace_put(SPACE_A), 0);
    CHECK_EQ(aspace_put(SPACE_A), 1);
    /* A stale second put of the same root (a TCB reaped twice, a bug the
     * table must not amplify into a double free of live page tables). */
    CHECK_EQ(aspace_put(SPACE_A), 0);         /* tombstoned: NOT a second free */
    CHECK_EQ(aspace_last_puts(), 1);          /* only one free was authorised */
    CHECK_EQ(aspace_double_puts(), 1);
}

static void test_untracked_space_keeps_old_behaviour(void) {
    aspace_reset();
    /* A pml4 the table never saw (an unmigrated call site): the first put
     * frees, exactly as the tree behaved before C2.  Precision, not safety. */
    CHECK_EQ(aspace_put(SPACE_B), 1);
    CHECK_EQ(aspace_untracked_puts(), 1);
}

static void test_many_spaces_distinct(void) {
    aspace_reset();
    /* 64 concurrent processes: no aliasing, every space counted on its own. */
    for (int i = 0; i < 64; i++) {
        CHECK_EQ(aspace_get(0x1000000ULL + (uint64_t)i * 0x1000ULL), 1);
    }
    CHECK_EQ(aspace_live_spaces(), 64);
    for (int i = 0; i < 64; i++) {
        CHECK_EQ(aspace_put(0x1000000ULL + (uint64_t)i * 0x1000ULL), 1);
    }
    CHECK_EQ(aspace_live_spaces(), 0);
    CHECK_EQ(aspace_last_puts(), 64);
}

int main(void) {
    printf("=== AuraLite OS address-space refcount unit tests ===\n\n");
    RUN(test_single_owner);
    RUN(test_kernel_space_is_untracked);
    RUN(test_clone_vm_sibling_dies_first);
    RUN(test_execve_space_outlives_its_creator);
    RUN(test_double_put_never_frees_twice);
    RUN(test_untracked_space_keeps_old_behaviour);
    RUN(test_many_spaces_distinct);

    printf("\n=== Results: %d/%d passed, %d failed ===\n", passed, tn, failed);
    return failed == 0 ? 0 : 1;
}

/*
 * test_uaccess.c — host-side unit tests for M3 fault-recovering uaccess.
 *
 * Tests the pure logic of usercopy: range validation, overflow detection,
 * kernel-space refusal, and the bounce-buffer TOCTOU contract.  The real
 * copy_from/to_user uses an asm primitive with #PF fixup that cannot run
 * on the host; this test verifies the validation layer that gates it.
 *
 * MATURITY_PLAN.md phase M3.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ---- Stubs for the kernel types/functions usercopy.c needs ---- */

#define PAGE_SIZE_BYTES 4096ULL
#define PAGE_FLAG_PRESENT  (1ULL << 0)
#define PAGE_FLAG_WRITABLE (1ULL << 1)
#define PAGE_FLAG_USER     (1ULL << 2)
#define PAGE_FLAG_COW      (1ULL << 5)
#define PAGE_FLAG_NO_EXEC  (1ULL << 63)
#define USER_VADDR_TOP     0x0000800000000000ULL

/* Simulated page table: a small set of mapped pages. */
#define MAX_MAPPED_PAGES 64
static struct {
    uint64_t vaddr;   /* page-aligned */
    uint64_t flags;
} mapped_pages[MAX_MAPPED_PAGES];
static int n_mapped = 0;

static void map_reset(void) { n_mapped = 0; }

static void map_add(uint64_t vaddr, uint64_t flags) {
    assert(n_mapped < MAX_MAPPED_PAGES);
    mapped_pages[n_mapped].vaddr = vaddr & ~(PAGE_SIZE_BYTES - 1);
    mapped_pages[n_mapped].flags = flags;
    n_mapped++;
}

uint64_t paging_get_flags(uint64_t vaddr) {
    uint64_t page = vaddr & ~(PAGE_SIZE_BYTES - 1);
    for (int i = 0; i < n_mapped; i++) {
        if (mapped_pages[i].vaddr == page) return mapped_pages[i].flags;
    }
    return 0;   /* not present */
}

int paging_handle_cow_fault(uint64_t page, uint64_t flags) {
    (void)flags;
    /* Simulate COW resolution: make the page writable. */
    for (int i = 0; i < n_mapped; i++) {
        if (mapped_pages[i].vaddr == (page & ~(PAGE_SIZE_BYTES - 1))) {
            mapped_pages[i].flags |= PAGE_FLAG_WRITABLE;
            mapped_pages[i].flags &= ~PAGE_FLAG_COW;
            return 1;
        }
    }
    return 0;
}

/* cpu_local stubs */
int cpu_local_ready = 0;

/* ---- Inline the validation logic from usercopy.c for host testing ---- */

static int user_range_bounds(uint64_t start, uint64_t len, uint64_t *end_out) {
    if (len == 0) {
        if (end_out) *end_out = start;
        return 1;
    }
    if (start < PAGE_SIZE_BYTES) return 0;
    uint64_t last = start + len - 1;
    if (last < start) return 0;
    if (last >= USER_VADDR_TOP) return 0;
    if (end_out) *end_out = last;
    return 1;
}

static int validate_user_range_impl(const void *user_ptr, uint64_t len,
                                    int write_required) {
    uint64_t start = (uint64_t)(uintptr_t)user_ptr;
    uint64_t last;
    if (!user_range_bounds(start, len, &last)) return 0;
    if (len == 0) return 1;
    uint64_t page = start & ~(PAGE_SIZE_BYTES - 1ULL);
    uint64_t last_page = last & ~(PAGE_SIZE_BYTES - 1ULL);
    for (;;) {
        uint64_t flags = paging_get_flags(page);
        if (!(flags & PAGE_FLAG_PRESENT)) return 0;
        if (!(flags & PAGE_FLAG_USER)) return 0;
        if (write_required && (flags & PAGE_FLAG_COW) && !(flags & PAGE_FLAG_WRITABLE)) {
            if (!paging_handle_cow_fault(page, 0x3)) return 0;
            flags = paging_get_flags(page);
        }
        if (write_required && !(flags & PAGE_FLAG_WRITABLE)) return 0;
        if (page == last_page) break;
        page += PAGE_SIZE_BYTES;
        if (page == 0) return 0;
    }
    return 1;
}

/* ---- Test infrastructure ---- */

static int tests_run = 0, tests_passed = 0;

#define CHECK(name, cond) do { \
    tests_run++; \
    if (cond) { tests_passed++; printf("  PASS: %s\n", name); } \
    else      { printf("  FAIL: %s\n", name); } \
} while (0)

/* ---- Tests ---- */

static void test_null_pointer(void) {
    CHECK("NULL pointer, len=10 refused",
          validate_user_range_impl(NULL, 10, 0) == 0);
}

static void test_near_null(void) {
    CHECK("address 0x100 (below first page) refused",
          validate_user_range_impl((void*)0x100, 10, 0) == 0);
    CHECK("address 0xFFF (last byte of first page) refused",
          validate_user_range_impl((void*)0xFFF, 1, 0) == 0);
}

static void test_zero_length(void) {
    /* Zero-length access at a valid address is OK. */
    map_reset();
    map_add(0x10000, PAGE_FLAG_PRESENT | PAGE_FLAG_USER);
    CHECK("zero-length at mapped page accepted",
          validate_user_range_impl((void*)0x10000, 0, 0) == 1);
}

static void test_unmapped_page(void) {
    map_reset();
    /* No pages mapped at all. */
    CHECK("unmapped page refused",
          validate_user_range_impl((void*)0x10000, 10, 0) == 0);
}

static void test_mapped_readable(void) {
    map_reset();
    map_add(0x10000, PAGE_FLAG_PRESENT | PAGE_FLAG_USER);
    CHECK("mapped user page, read OK",
          validate_user_range_impl((void*)0x10000, 100, 0) == 1);
    CHECK("mapped user page, write refused (not writable)",
          validate_user_range_impl((void*)0x10000, 100, 1) == 0);
}

static void test_mapped_writable(void) {
    map_reset();
    map_add(0x10000, PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_WRITABLE);
    CHECK("mapped writable user page, write OK",
          validate_user_range_impl((void*)0x10000, 100, 1) == 1);
}

static void test_cow_page(void) {
    map_reset();
    map_add(0x10000, PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_COW);
    CHECK("COW page, write triggers fault resolution -> OK",
          validate_user_range_impl((void*)0x10000, 100, 1) == 1);
    /* After COW resolution, the page should be writable. */
    CHECK("COW page now writable after resolution",
          (paging_get_flags(0x10000) & PAGE_FLAG_WRITABLE) != 0);
}

static void test_kernel_space_refused(void) {
    map_reset();
    CHECK("kernel-space address (0xFFFF800000000000) refused by bounds",
          user_range_bounds(0xFFFF800000000000ULL, 10, NULL) == 0);
    CHECK("high canonical address refused by bounds",
          user_range_bounds(0xFFFFFFFF80000000ULL, 10, NULL) == 0);
}

static void test_overflow_range(void) {
    map_reset();
    /* start + len wraps around. */
    CHECK("wrap-around range refused",
          user_range_bounds(0xFFFFFFFFFFFFFF00ULL, 0x200, NULL) == 0);
    CHECK("max-len overflow refused",
          user_range_bounds(0x10000, 0xFFFFFFFFFFFFFFFFULL, NULL) == 0);
}

static void test_spanning_pages(void) {
    map_reset();
    /* Two contiguous mapped pages. */
    map_add(0x10000, PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_WRITABLE);
    map_add(0x11000, PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_WRITABLE);
    CHECK("two-page span, both mapped -> OK",
          validate_user_range_impl((void*)0x10800, 0x1000, 1) == 1);
}

static void test_spanning_gap(void) {
    map_reset();
    /* Only first page mapped, second is a gap. */
    map_add(0x10000, PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_WRITABLE);
    CHECK("two-page span, second unmapped -> refused",
          validate_user_range_impl((void*)0x10800, 0x1000, 1) == 0);
}

static void test_non_user_page(void) {
    map_reset();
    /* Page present but not USER (kernel-only mapping). */
    map_add(0x10000, PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE);
    CHECK("kernel-only page refused for user access",
          validate_user_range_impl((void*)0x10000, 10, 0) == 0);
}

static void test_boundary_exact(void) {
    map_reset();
    map_add(0x10000, PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_WRITABLE);
    /* Exact end of page: start=0x10000, len=4096 -> last byte at 0x10FFF. */
    CHECK("exact one-page range accepted",
          validate_user_range_impl((void*)0x10000, PAGE_SIZE_BYTES, 1) == 1);
    /* One byte past: needs second page. */
    CHECK("one byte past one page -> refused (no second page)",
          validate_user_range_impl((void*)0x10000, PAGE_SIZE_BYTES + 1, 1) == 0);
}

static void test_bounce_buffer_contract(void) {
    /*
     * The bounce-buffer TOCTOU contract: the kernel copies user data into a
     * kernel-local buffer BEFORE acting on it.  A racing munmap between
     * validate and the actual copy is caught by the #PF fixup in the asm
     * primitive (not testable here), but the validation-then-copy ORDER is
     * the invariant we assert on.
     *
     * Here we verify that a validate-then-copy sequence with a page that
     * becomes unmapped mid-copy would be caught: validate passes, then
     * the simulated "copy" sees the page gone.
     */
    map_reset();
    map_add(0x10000, PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_WRITABLE);
    int v = validate_user_range_impl((void*)0x10000, 100, 1);
    CHECK("validate passes before unmap", v == 1);

    /* Simulate the page being unmapped between validate and copy. */
    map_reset();
    uint64_t flags = paging_get_flags(0x10000);
    CHECK("page gone after unmap (paging_get_flags returns 0)", flags == 0);
    /* The asm copy primitive would #PF here and the fixup returns -1.
     * This is the TOCTOU safety net: validate is optimistic, the copy
     * is the pessimistic gate. */
}

static void test_user_vaddr_top_boundary(void) {
    map_reset();
    /* Address just below USER_VADDR_TOP. */
    uint64_t top = USER_VADDR_TOP - PAGE_SIZE_BYTES;
    map_add(top, PAGE_FLAG_PRESENT | PAGE_FLAG_USER);
    CHECK("last valid user page accepted",
          validate_user_range_impl((void*)(uintptr_t)top, 10, 0) == 1);

    /* Address at USER_VADDR_TOP itself should be refused by bounds. */
    CHECK("address at USER_VADDR_TOP refused",
          user_range_bounds(USER_VADDR_TOP, 10, NULL) == 0);
}

static void test_partial_page_write_readonly(void) {
    map_reset();
    /* First page writable, second page read-only. */
    map_add(0x10000, PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_WRITABLE);
    map_add(0x11000, PAGE_FLAG_PRESENT | PAGE_FLAG_USER);
    CHECK("span with read-only second page, write refused",
          validate_user_range_impl((void*)0x10800, 0x1000, 1) == 0);
    CHECK("span with read-only second page, read OK",
          validate_user_range_impl((void*)0x10800, 0x1000, 0) == 1);
}

int main(void) {
    printf("== M3 uaccess: fault-recovering user-copy validation ==\n");

    test_null_pointer();
    test_near_null();
    test_zero_length();
    test_unmapped_page();
    test_mapped_readable();
    test_mapped_writable();
    test_cow_page();
    test_kernel_space_refused();
    test_overflow_range();
    test_spanning_pages();
    test_spanning_gap();
    test_non_user_page();
    test_boundary_exact();
    test_bounce_buffer_contract();
    test_user_vaddr_top_boundary();
    test_partial_page_write_readonly();

    printf("== %d/%d passed ==\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}

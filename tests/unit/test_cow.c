/*
 * test_cow.c — unit tests for Copy-on-Write (COW) fork mechanics:
 * page-table sharing, refcount tracking, page-fault handling, and isolation.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; if (failed == b) passed++; } while(0)
#define CHECK(c) do { if(!(c)) { printf("  FAIL L%d: %s\n",__LINE__,#c); failed++; } } while(0)
#define CHECK_EQ(a,e) do { if((long)(a)!=(long)(e)) { printf("  FAIL L%d: %s=%ld want %ld\n",__LINE__,#a,(long)(a),(long)(e)); failed++; } } while(0)

/* ---- Constants & Flags ---- */
#define PAGE_FLAG_PRESENT  (1ULL << 0)
#define PAGE_FLAG_WRITABLE (1ULL << 1)
#define PAGE_FLAG_USER     (1ULL << 2)
#define PAGE_FLAG_COW      (1ULL << 9)
#define PAGE_SIZE_BYTES    4096
#define PAGE_ADDR_MASK     0x000FFFFFFFFFF000ULL
#define PML4_USER_TOP      256

#define PML4_INDEX(addr) (((addr) >> 39) & 0x1FF)

/* ---- Mocks for PMM refcounting and frames ---- */
#define MOCK_NFRAMES 1024
static uint32_t mock_refcount[MOCK_NFRAMES];
static uint8_t mock_used[MOCK_NFRAMES];
static uint8_t mock_memory[MOCK_NFRAMES][PAGE_SIZE_BYTES];

static void mock_pmm_init(void) {
    memset(mock_refcount, 0, sizeof(mock_refcount));
    memset(mock_used, 0, sizeof(mock_used));
    memset(mock_memory, 0, sizeof(mock_memory));
    /* Frame 0 is reserved/null */
    mock_used[0] = 1;
    mock_refcount[0] = 1;
}

static uint64_t mock_pmm_alloc_frame(void) {
    for (int i = 1; i < MOCK_NFRAMES; i++) {
        if (!mock_used[i]) {
            mock_used[i] = 1;
            mock_refcount[i] = 1;
            memset(mock_memory[i], 0, PAGE_SIZE_BYTES);
            return (uint64_t)i * PAGE_SIZE_BYTES;
        }
    }
    return 0;
}

static void mock_pmm_free_frame(uint64_t phys) {
    uint64_t idx = phys / PAGE_SIZE_BYTES;
    if (idx == 0 || idx >= MOCK_NFRAMES) return;
    if (!mock_used[idx]) return;
    if (mock_refcount[idx] > 1) {
        mock_refcount[idx]--;
        return;
    }
    mock_refcount[idx] = 0;
    mock_used[idx] = 0;
}

static int mock_pmm_inc_frame_ref(uint64_t phys) {
    uint64_t idx = phys / PAGE_SIZE_BYTES;
    if (idx == 0 || idx >= MOCK_NFRAMES || !mock_used[idx]) return -1;
    mock_refcount[idx]++;
    return 0;
}

static uint32_t mock_pmm_get_frame_refcount(uint64_t phys) {
    uint64_t idx = phys / PAGE_SIZE_BYTES;
    if (idx == 0 || idx >= MOCK_NFRAMES || !mock_used[idx]) return 0;
    return mock_refcount[idx];
}

/* ---- Mocks for VMM / COW Logic ---- */
static uint64_t mock_parent_pte = 0;
static uint64_t mock_child_pte = 0;

static int mock_paging_clone_pte(uint64_t *old_pte, uint64_t *new_pte) {
    uint64_t opte = *old_pte;
    if (!(opte & PAGE_FLAG_PRESENT) || !(opte & PAGE_FLAG_USER)) return 0;

    uint64_t old_phys = opte & PAGE_ADDR_MASK;
    uint64_t flags = opte & ~PAGE_ADDR_MASK;

    if (mock_pmm_inc_frame_ref(old_phys) != 0) return -1;

    if (flags & (PAGE_FLAG_WRITABLE | PAGE_FLAG_COW)) {
        flags &= ~PAGE_FLAG_WRITABLE;
        flags |= PAGE_FLAG_COW;
        *old_pte = old_phys | flags;
        /* invlpg simulated */
    }
    *new_pte = old_phys | flags;
    return 0;
}

static int mock_paging_handle_cow_fault(uint64_t fault_addr, uint64_t err_code, uint64_t *pte) {
    /* COW faults are present write-protection faults (err_code & 3 == 3) */
    if ((err_code & 0x3ULL) != 0x3ULL) return 0;

    uint64_t virt = fault_addr & ~(PAGE_SIZE_BYTES - 1ULL);
    if (PML4_INDEX(virt) >= PML4_USER_TOP) return 0;

    if (!pte || !(*pte & PAGE_FLAG_PRESENT) || !(*pte & PAGE_FLAG_USER) || !(*pte & PAGE_FLAG_COW)) {
        return 0;
    }

    uint64_t old_phys = *pte & PAGE_ADDR_MASK;
    uint64_t flags = (*pte & ~PAGE_ADDR_MASK) & ~PAGE_FLAG_COW;
    flags |= PAGE_FLAG_WRITABLE;

    uint32_t refs = mock_pmm_get_frame_refcount(old_phys);
    if (refs <= 1) {
        *pte = old_phys | flags;
        return 1;
    }

    uint64_t new_phys = mock_pmm_alloc_frame();
    if (!new_phys) return 0;
    memcpy(mock_memory[new_phys / PAGE_SIZE_BYTES], mock_memory[old_phys / PAGE_SIZE_BYTES], PAGE_SIZE_BYTES);

    *pte = new_phys | flags;
    mock_pmm_free_frame(old_phys);
    return 1;
}

/* ---- Test Cases ---- */

static void test_cow_fork_sharing(void) {
    printf("--- COW fork sharing ---\n");
    mock_pmm_init();

    uint64_t frame = mock_pmm_alloc_frame();
    CHECK(frame != 0);
    CHECK_EQ(mock_pmm_get_frame_refcount(frame), 1);

    mock_parent_pte = frame | PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER;
    mock_child_pte = 0;

    int ret = mock_paging_clone_pte(&mock_parent_pte, &mock_child_pte);
    CHECK_EQ(ret, 0);

    /* Verify flags modified correctly in both parent and child */
    CHECK((mock_parent_pte & PAGE_FLAG_COW) != 0);
    CHECK((mock_parent_pte & PAGE_FLAG_WRITABLE) == 0);
    CHECK((mock_child_pte & PAGE_FLAG_COW) != 0);
    CHECK((mock_child_pte & PAGE_FLAG_WRITABLE) == 0);

    /* Verify refcount incremented */
    CHECK_EQ(mock_pmm_get_frame_refcount(frame), 2);
}

static void test_cow_fault_copy(void) {
    printf("--- COW fault copy ---\n");
    mock_pmm_init();

    uint64_t frame1 = mock_pmm_alloc_frame();
    uint64_t idx1 = frame1 / PAGE_SIZE_BYTES;
    strcpy((char *)mock_memory[idx1], "Shared Data");

    mock_parent_pte = frame1 | PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER;
    mock_paging_clone_pte(&mock_parent_pte, &mock_child_pte);
    CHECK_EQ(mock_pmm_get_frame_refcount(frame1), 2);

    /* Child attempts to write to the page, triggering a COW fault */
    uint64_t fault_virt = 0x100000;
    int handled = mock_paging_handle_cow_fault(fault_virt, 0x3, &mock_child_pte);
    CHECK_EQ(handled, 1);

    /* Verify child PTE points to a new frame with WRITABLE set and COW cleared */
    uint64_t frame2 = mock_child_pte & PAGE_ADDR_MASK;
    CHECK(frame2 != frame1);
    CHECK((mock_child_pte & PAGE_FLAG_WRITABLE) != 0);
    CHECK((mock_child_pte & PAGE_FLAG_COW) == 0);

    /* Verify data was copied to the new frame */
    uint64_t idx2 = frame2 / PAGE_SIZE_BYTES;
    CHECK_EQ(strcmp((char *)mock_memory[idx2], "Shared Data"), 0);

    /* Verify refcount of old frame decremented */
    CHECK_EQ(mock_pmm_get_frame_refcount(frame1), 1);
    CHECK_EQ(mock_pmm_get_frame_refcount(frame2), 1);
}

static void test_cow_fault_single_ref(void) {
    printf("--- COW fault single ref ---\n");
    mock_pmm_init();

    uint64_t frame1 = mock_pmm_alloc_frame();
    mock_parent_pte = frame1 | PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER;
    mock_paging_clone_pte(&mock_parent_pte, &mock_child_pte);
    CHECK_EQ(mock_pmm_get_frame_refcount(frame1), 2);

    /* Simulate child process exiting and freeing its address space */
    mock_pmm_free_frame(mock_child_pte & PAGE_ADDR_MASK);
    mock_child_pte = 0;
    CHECK_EQ(mock_pmm_get_frame_refcount(frame1), 1);

    /* Parent now attempts to write to its COW page */
    uint64_t fault_virt = 0x200000;
    int handled = mock_paging_handle_cow_fault(fault_virt, 0x3, &mock_parent_pte);
    CHECK_EQ(handled, 1);

    /* Verify parent PTE still points to frame1, but WRITABLE is restored and COW cleared */
    CHECK_EQ(mock_parent_pte & PAGE_ADDR_MASK, frame1);
    CHECK((mock_parent_pte & PAGE_FLAG_WRITABLE) != 0);
    CHECK((mock_parent_pte & PAGE_FLAG_COW) == 0);
    CHECK_EQ(mock_pmm_get_frame_refcount(frame1), 1);
}

static void test_cow_fault_invalid_cases(void) {
    printf("--- COW fault invalid cases ---\n");
    mock_pmm_init();

    uint64_t frame = mock_pmm_alloc_frame();
    mock_parent_pte = frame | PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER;
    mock_paging_clone_pte(&mock_parent_pte, &mock_child_pte);

    /* Non-write fault (e.g. read fault err_code 0x1 or 0x0) */
    int handled = mock_paging_handle_cow_fault(0x100000, 0x1, &mock_child_pte);
    CHECK_EQ(handled, 0);

    /* Fault in kernel space (PML4 >= 256) */
    uint64_t kernel_virt = 0xFFFFFFFF80000000ULL;
    handled = mock_paging_handle_cow_fault(kernel_virt, 0x3, &mock_child_pte);
    CHECK_EQ(handled, 0);

    /* Fault on page without COW flag */
    uint64_t normal_pte = frame | PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER;
    handled = mock_paging_handle_cow_fault(0x100000, 0x3, &normal_pte);
    CHECK_EQ(handled, 0);
}

int main(void) {
    printf("=== AuraLite OS COW Unit Tests ===\n\n");
    RUN(test_cow_fork_sharing);
    RUN(test_cow_fault_copy);
    RUN(test_cow_fault_single_ref);
    RUN(test_cow_fault_invalid_cases);

    printf("\n=== Results: %d/%d passed, %d failed ===\n", passed, tn, failed);
    return failed == 0 ? 0 : 1;
}

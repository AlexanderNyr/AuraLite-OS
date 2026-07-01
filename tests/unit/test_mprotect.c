#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../../kernel/mm/vma.h"
#include "../../kernel/arch/x86_64/paging.h"

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

static uint64_t fake_phys[4];
static uint64_t mapped_va[4];
static uint64_t mapped_phys[4];
static uint64_t mapped_flags[4];
static uint64_t invalidated_va[4];
static int map_calls;
static int invlpg_calls;
static int ipi_calls;
static uint8_t last_ipi_vector;

vma_t *vma_find(vma_t *list, uint64_t va) {
    while (list) {
        if (va >= list->va_start && va < list->va_end) return list;
        list = list->next;
    }
    return NULL;
}

uint64_t paging_get_phys(uint64_t virt) {
    return fake_phys[(virt - 0x1000) / PAGE_SIZE_BYTES];
}

void paging_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    mapped_va[map_calls] = virt;
    mapped_phys[map_calls] = phys;
    mapped_flags[map_calls] = flags;
    map_calls++;
}

void lapic_send_ipi_all_excluding_self(uint8_t vector) {
    ipi_calls++;
    last_ipi_vector = vector;
}

static void record_invlpg(uint64_t va) {
    invalidated_va[invlpg_calls++] = va;
}

#define MPROTECT_INVLPG(va) record_invlpg(va)
#include "../../kernel/arch/x86_64/mprotect.c"

static void reset_state(void) {
    memset(fake_phys, 0, sizeof(fake_phys));
    memset(mapped_va, 0, sizeof(mapped_va));
    memset(mapped_phys, 0, sizeof(mapped_phys));
    memset(mapped_flags, 0, sizeof(mapped_flags));
    memset(invalidated_va, 0, sizeof(invalidated_va));
    map_calls = 0;
    invlpg_calls = 0;
    ipi_calls = 0;
    last_ipi_vector = 0;
}

static void test_updates_multiple_vmas(void) {
    vma_t second = { .va_start = 0x2000, .va_end = 0x3000, .flags = VMA_FILE | VMA_READ | VMA_EXEC, .next = NULL };
    vma_t first  = { .va_start = 0x1000, .va_end = 0x2000, .flags = VMA_ANON | VMA_READ | VMA_WRITE, .next = &second };

    assert(mprotect_update_vma_range(&first, 0x1000, 0x2000, PROT_READ) == 0);
    assert(first.flags == (VMA_ANON | VMA_READ));
    assert(second.flags == (VMA_FILE | VMA_READ));
}

static void test_rejects_gaps(void) {
    vma_t second = { .va_start = 0x3000, .va_end = 0x4000, .flags = VMA_READ, .next = NULL };
    vma_t first  = { .va_start = 0x1000, .va_end = 0x2000, .flags = VMA_READ, .next = &second };

    assert(mprotect_update_vma_range(&first, 0x1000, 0x3000, PROT_READ) != 0);
}

static void test_remap_invalidates_and_shoots_down(void) {
    reset_state();
    fake_phys[0] = 0xA000;
    fake_phys[1] = 0;
    fake_phys[2] = 0xB000;

    mprotect_remap_present_pages(0x1000, 0x3000, PROT_READ);
    assert(map_calls == 2);
    assert(mapped_va[0] == 0x1000 && mapped_phys[0] == 0xA000);
    assert(mapped_va[1] == 0x3000 && mapped_phys[1] == 0xB000);
    assert(mapped_flags[0] == (PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_NO_EXEC));
    assert(mapped_flags[1] == (PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_NO_EXEC));
    assert(invlpg_calls == 2);
    assert(invalidated_va[0] == 0x1000);
    assert(invalidated_va[1] == 0x3000);
    assert(ipi_calls == 1);
    assert(last_ipi_vector == IPI_TLB_SHOOTDOWN_VECTOR);
}

static void test_exec_and_write_flags(void) {
    reset_state();
    fake_phys[0] = 0xA000;

    mprotect_remap_present_pages(0x1000, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
    assert(map_calls == 1);
    assert(mapped_flags[0] == (PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_WRITABLE));
    assert(ipi_calls == 1);
}

int main(void) {
    test_updates_multiple_vmas();
    test_rejects_gaps();
    test_remap_invalidates_and_shoots_down();
    test_exec_and_write_flags();
    return 0;
}

/* paging.c — 4-level paging VMM for x86_64.
 *
 * The VMM walks the page-table hierarchy starting from the current PML4 (read
 * from CR3 at init).  Intermediate table frames are allocated from the PMM and
 * accessed through Limine's higher-half direct map (HHDM), which maps all
 * physical memory at a fixed virtual offset.
 */

#include <stdint.h>
#include "kernel/arch/x86_64/paging.h"
#include "kernel/arch/x86_64/cpu.h"
#include "kernel/mm/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/limine_requests.h"

/* EFER (Extended Feature Enable Register) — Intel SDM Vol.3, 35.14 (AMD usage). */
#define MSR_EFER   0xC0000080
#define EFER_NXE   (1ULL << 11)   /* enables NX bit in PTEs */

#define VMM_TAG "[vmm] "

static uint64_t  hhdm;    /* higher-half direct-map offset (from Limine)     */
static uint64_t *pml4;    /* HHDM pointer to the current PML4 (from CR3)    */

/* Convert a physical address to a writable HHDM virtual pointer. */
static inline void *phys_to_ptr(uint64_t phys) {
    return (void *)(uintptr_t)(hhdm + phys);
}

void paging_init(void) {
    hhdm = limine_get_hhdm_offset();
    if (hhdm == 0) {
        kprintf(VMM_TAG "FATAL: no HHDM available; cannot manage page tables\n");
        return;
    }

    /* Record the current PML4 (Limine set up paging before handing off). */
    uint64_t cr3 = read_cr3();
    pml4 = (uint64_t *)phys_to_ptr(cr3 & PAGE_ADDR_MASK);

    /* Enable the NX (No-Execute) execution-disable bit in page tables. */
    uint64_t efer = read_msr(MSR_EFER);
    write_msr(MSR_EFER, efer | EFER_NXE);

    kprintf(VMM_TAG "PML4 at phys 0x%016llx, HHDM 0x%016llx, NXE enabled\n",
            (unsigned long long)(cr3 & PAGE_ADDR_MASK),
            (unsigned long long)hhdm);
}

/*
 * Walk the 4-level hierarchy for `virt`, returning a pointer to the final PTE.
 * When `create` is non-zero, any missing intermediate table is allocated from
 * the PMM, zeroed, and linked into its parent.  Returns NULL if an intermediate
 * table is absent and was not created, or if allocation failed.
 *
 * Intermediate entries get Present|Writable|User so that:
 *   - the page is accessible from ring 0 (kernel) now,
 *   - and from ring 3 (user) once the final PTE also carries PAGE_FLAG_USER.
 */
static uint64_t *walk_pte(uint64_t virt, int create) {
    uint64_t *table = pml4;

    /* The first three levels each index into the NEXT table. */
    const int indices[3] = {
        PML4_INDEX(virt),
        PDPT_INDEX(virt),
        PD_INDEX(virt),
    };

    for (int level = 0; level < 3; level++) {
        uint64_t entry = table[indices[level]];
        if (!(entry & PAGE_FLAG_PRESENT)) {
            if (!create) {
                return NULL;
            }
            uint64_t new_phys = pmm_alloc_frame();
            if (new_phys == 0) {
                kprintf(VMM_TAG "OOM allocating page table\n");
                return NULL;
            }
            uint64_t *new_table = phys_to_ptr(new_phys);
            memset(new_table, 0, PAGE_SIZE_BYTES);   /* PTEs must start zeroed */
            table[indices[level]] = new_phys
                | PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER;
            entry = table[indices[level]];
        }
        table = phys_to_ptr(entry & PAGE_ADDR_MASK);
    }

    /* `table` now points to the leaf PT; return the address of PTE[pt_index]. */
    return &table[PT_INDEX(virt)];
}

void paging_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pte = walk_pte(virt, 1);
    if (pte == NULL) {
        kprintf(VMM_TAG "map: failed to get PTE for 0x%016llx\n",
                (unsigned long long)virt);
        return;
    }
    *pte = (phys & PAGE_ADDR_MASK) | flags;
    /* Flush any stale TLB entry for this virtual address. */
    invlpg(virt);
}

void paging_unmap(uint64_t virt) {
    uint64_t *pte = walk_pte(virt, 0);
    if (pte == NULL || !(*pte & PAGE_FLAG_PRESENT)) {
        return;   /* nothing to unmap */
    }
    *pte = 0;
    invlpg(virt);
}

uint64_t paging_get_phys(uint64_t virt) {
    uint64_t *pte = walk_pte(virt, 0);
    if (pte == NULL || !(*pte & PAGE_FLAG_PRESENT)) {
        return 0;
    }
    return *pte & PAGE_ADDR_MASK;
}

uint64_t paging_new_address_space(void) {
    uint64_t new_pml4_phys = pmm_alloc_frame();
    if (new_pml4_phys == 0) {
        return 0;
    }
    uint64_t *new_pml4 = phys_to_ptr(new_pml4_phys);
    memset(new_pml4, 0, PAGE_SIZE_BYTES);

    /* Share the kernel half (PML4 entries 256-511) so the kernel stays mapped
       in every address space.  The user half (0-255) starts empty. */
    for (int i = PML4_USER_TOP; i < 512; i++) {
        new_pml4[i] = pml4[i];
    }
    return new_pml4_phys;
}

void paging_self_test(void) {
    /* A canonical user-space address that is certainly unmapped: 6 TiB.
       Bit 47 is clear (positive canonical); bits 48-63 are zero. */
    const uint64_t test_virt = 0x0000006000000000ULL;
    const uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE;
    const uint64_t seed_a = 0xDEADBEEFCAFE0000ULL;
    const uint64_t seed_b = 0x123456789ABCDEF0ULL;

    kprintf(VMM_TAG "self-test: mapping 0x%016llx...\n",
            (unsigned long long)test_virt);

    /* 1) Allocate a frame and seed it with a known value via the HHDM. */
    uint64_t phys = pmm_alloc_frame();
    if (phys == 0) {
        kprintf(VMM_TAG "FAIL: OOM allocating test frame\n");
        return;
    }
    volatile uint64_t *hhdm_ptr = (volatile uint64_t *)phys_to_ptr(phys);
    *hhdm_ptr = seed_a;

    /* 2) The test address must not already be mapped. */
    if (paging_get_phys(test_virt) != 0) {
        kprintf(VMM_TAG "FAIL: test address already mapped before map\n");
        return;
    }

    /* 3) Map virt -> phys. */
    paging_map(test_virt, phys, flags);

    /* 4) The translation must now resolve to our frame. */
    if (paging_get_phys(test_virt) != phys) {
        kprintf(VMM_TAG "FAIL: get_phys returned 0x%016llx, expected 0x%016llx\n",
                (unsigned long long)paging_get_phys(test_virt),
                (unsigned long long)phys);
        return;
    }

    /* 5) Read through the virtual address: must see the seeded value. */
    volatile uint64_t *virt_ptr = (volatile uint64_t *)test_virt;
    if (*virt_ptr != seed_a) {
        kprintf(VMM_TAG "FAIL: read via virt gave 0x%016llx, expected 0x%016llx\n",
                (unsigned long long)*virt_ptr, (unsigned long long)seed_a);
        return;
    }

    /* 6) Write through the virtual address, verify via the HHDM (same page). */
    *virt_ptr = seed_b;
    if (*hhdm_ptr != seed_b) {
        kprintf(VMM_TAG "FAIL: write via virt not reflected in physical page\n");
        return;
    }

    /* 7) Unmap and invalidate the TLB. */
    paging_unmap(test_virt);

    /* 8) Translation must now report "not present." */
    if (paging_get_phys(test_virt) != 0) {
        kprintf(VMM_TAG "FAIL: address still mapped after unmap\n");
        return;
    }

    kprintf(VMM_TAG "PASS: map / read / write / unmap all correct\n");

    /* 9) Deliberately access the now-unmapped page.  The #PF handler (Phase 2)
     *    prints a register dump including CR2 and halts — proving that an
     *    unmapped access is caught cleanly instead of triple-faulting.
     *    If we somehow reach the next line, the test FAILED (no fault). */
    kprintf(VMM_TAG "self-test: accessing unmapped page (expect #PF + halt)...\n");
    volatile uint64_t *should_fault = (volatile uint64_t *)test_virt;
    uint64_t leaked = *should_fault;          /* MUST fault here */
    kprintf(VMM_TAG "FAIL: expected page fault but read 0x%016llx\n",
            (unsigned long long)leaked);
}

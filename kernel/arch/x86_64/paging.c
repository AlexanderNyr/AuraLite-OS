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

#define CR4_SMEP   (1ULL << 20)
#define CR4_SMAP   (1ULL << 21)
#define CPUID7_EBX_SMEP (1U << 7)
#define CPUID7_EBX_SMAP (1U << 20)

#define VMM_TAG "[vmm] "

static uint64_t  hhdm;    /* higher-half direct-map offset (from Limine)     */
static uint64_t *pml4;    /* HHDM pointer to the current PML4 (from CR3)    */
static uint64_t  kernel_pml4_phys;  /* the kernel's PML4 (for switching back) */
volatile int cpu_smap_is_active = 0;

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
    kernel_pml4_phys = cr3 & PAGE_ADDR_MASK;
    pml4 = (uint64_t *)phys_to_ptr(kernel_pml4_phys);

    /* Enable the NX (No-Execute) execution-disable bit in page tables. */
    uint64_t efer = read_msr(MSR_EFER);
    write_msr(MSR_EFER, efer | EFER_NXE);

    /* Enable SMEP when the CPU advertises it so the kernel cannot execute
     * instructions from user-mapped pages. */
    uint32_t a, b, c, d;
    cpuid_count(7, 0, &a, &b, &c, &d);
    (void)a; (void)c; (void)d;
    uint64_t cr4 = read_cr4();
    if (b & CPUID7_EBX_SMEP) {
        cr4 |= CR4_SMEP;
        kprintf(VMM_TAG "SMEP enabled\n");
    }
    if (b & CPUID7_EBX_SMAP) {
        cr4 |= CR4_SMAP;
        cpu_smap_is_active = 1;
        kprintf(VMM_TAG "SMAP enabled\n");
    }
    write_cr4(cr4);

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

int paging_protect(uint64_t virt, uint64_t flags) {
    uint64_t *pte = walk_pte(virt, 0);
    if (pte == NULL || !(*pte & PAGE_FLAG_PRESENT)) {
        return -1;
    }
    *pte = (*pte & PAGE_ADDR_MASK) | flags;
    invlpg(virt);
    return 0;
}

uint64_t paging_get_phys(uint64_t virt) {
    uint64_t *pte = walk_pte(virt, 0);
    if (pte == NULL || !(*pte & PAGE_FLAG_PRESENT)) {
        return 0;
    }
    return *pte & PAGE_ADDR_MASK;
}

uint64_t paging_get_flags(uint64_t virt) {
    uint64_t *pte = walk_pte(virt, 0);
    if (pte == NULL || !(*pte & PAGE_FLAG_PRESENT)) {
        return 0;
    }
    return *pte & ~PAGE_ADDR_MASK;
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

/*
 * Switch the active address space: update CR3 and the VMM's pml4 pointer.
 * Safe because the kernel half (PML4 entries 256-511) is shared, so kernel
 * code, the heap, and all kernel stacks remain accessible after the switch.
 */
void paging_switch_to(uint64_t new_pml4_phys) {
    if (new_pml4_phys == 0) {
        return;
    }
    write_cr3(new_pml4_phys);
    pml4 = (uint64_t *)phys_to_ptr(new_pml4_phys);
}

/* Update only the pml4 pointer (used after a manual CR3 write in scheduler). */
void paging_update_pml4_ptr(uint64_t phys) {
    pml4 = (uint64_t *)phys_to_ptr(phys);
}

uint64_t paging_get_kernel_pml4(void) {
    return kernel_pml4_phys;
}

/*
 * Clone all user-space pages from the current address space into a new one.
 *
 * This is a mark-and-share copy-on-write fork(): page-table pages are copied,
 * but leaf user frames are shared.  Writable leaves are made read-only in BOTH
 * parent and child and tagged PAGE_FLAG_COW; the first user write takes a #PF
 * and paging_handle_cow_fault() performs the real page copy.  Read-only leaves
 * are shared as-is.
 */
uint64_t paging_clone_user_space(void) {
    uint64_t new_pml4_phys = paging_new_address_space();
    if (new_pml4_phys == 0) return 0;
    uint64_t *new_pml4 = phys_to_ptr(new_pml4_phys);

    for (int i4 = 0; i4 < PML4_USER_TOP; i4++) {
        if (!(pml4[i4] & PAGE_FLAG_PRESENT)) continue;
        uint64_t *o_pdpt = phys_to_ptr(pml4[i4] & PAGE_ADDR_MASK);
        uint64_t n_pdpt_p = pmm_alloc_frame();
        if (!n_pdpt_p) goto fail;
        uint64_t *n_pdpt = phys_to_ptr(n_pdpt_p);
        memset(n_pdpt, 0, PAGE_SIZE_BYTES);
        new_pml4[i4] = n_pdpt_p | PAGE_FLAG_PRESENT|PAGE_FLAG_WRITABLE|PAGE_FLAG_USER;

        for (int i3 = 0; i3 < 512; i3++) {
            if (!(o_pdpt[i3] & PAGE_FLAG_PRESENT)) continue;
            uint64_t *o_pd = phys_to_ptr(o_pdpt[i3] & PAGE_ADDR_MASK);
            uint64_t n_pd_p = pmm_alloc_frame();
            if (!n_pd_p) goto fail;
            uint64_t *n_pd = phys_to_ptr(n_pd_p);
            memset(n_pd, 0, PAGE_SIZE_BYTES);
            n_pdpt[i3] = n_pd_p | PAGE_FLAG_PRESENT|PAGE_FLAG_WRITABLE|PAGE_FLAG_USER;

            for (int i2 = 0; i2 < 512; i2++) {
                if (!(o_pd[i2] & PAGE_FLAG_PRESENT)) continue;
                uint64_t *o_pt = phys_to_ptr(o_pd[i2] & PAGE_ADDR_MASK);
                uint64_t n_pt_p = pmm_alloc_frame();
                if (!n_pt_p) goto fail;
                uint64_t *n_pt = phys_to_ptr(n_pt_p);
                memset(n_pt, 0, PAGE_SIZE_BYTES);
                n_pd[i2] = n_pt_p | PAGE_FLAG_PRESENT|PAGE_FLAG_WRITABLE|PAGE_FLAG_USER;

                for (int i1 = 0; i1 < 512; i1++) {
                    uint64_t opte = o_pt[i1];
                    if (!(opte & PAGE_FLAG_PRESENT)) continue;
                    if (!(opte & PAGE_FLAG_USER)) continue;

                    uint64_t old_phys = opte & PAGE_ADDR_MASK;
                    uint64_t flags = opte & ~PAGE_ADDR_MASK;

                    if (pmm_inc_frame_ref(old_phys) != 0) goto fail;

                    if (flags & (PAGE_FLAG_WRITABLE | PAGE_FLAG_COW)) {
                        flags &= ~PAGE_FLAG_WRITABLE;
                        flags |= PAGE_FLAG_COW;
                        o_pt[i1] = old_phys | flags;
                        uint64_t virt = ((uint64_t)i4 << 39) |
                                        ((uint64_t)i3 << 30) |
                                        ((uint64_t)i2 << 21) |
                                        ((uint64_t)i1 << 12);
                        invlpg(virt);
                    }
                    n_pt[i1] = old_phys | flags;
                }
            }
        }
    }
    return new_pml4_phys;

fail:
    (void)paging_free_address_space(new_pml4_phys);
    return 0;
}

int paging_handle_cow_fault(uint64_t fault_addr, uint64_t err_code) {
    /* COW faults are present write-protection faults.  The U/S bit may be 0
     * when the kernel writes to a user COW page via copy_to_user(). */
    if ((err_code & 0x3ULL) != 0x3ULL) return 0;

    uint64_t virt = fault_addr & ~(PAGE_SIZE_BYTES - 1ULL);
    if (PML4_INDEX(virt) >= PML4_USER_TOP) return 0;

    uint64_t *pte = walk_pte(virt, 0);
    if (!pte || !(*pte & PAGE_FLAG_PRESENT) || !(*pte & PAGE_FLAG_USER) ||
        !(*pte & PAGE_FLAG_COW)) {
        return 0;
    }

    uint64_t old_phys = *pte & PAGE_ADDR_MASK;
    uint64_t flags = (*pte & ~PAGE_ADDR_MASK) & ~PAGE_FLAG_COW;
    flags |= PAGE_FLAG_WRITABLE;

    uint32_t refs = pmm_get_frame_refcount(old_phys);
    if (refs <= 1) {
        *pte = old_phys | flags;
        invlpg(virt);
        return 1;
    }

    uint64_t new_phys = pmm_alloc_frame();
    if (!new_phys) return 0;
    memcpy(phys_to_ptr(new_phys), phys_to_ptr(old_phys), PAGE_SIZE_BYTES);

    *pte = new_phys | flags;
    invlpg(virt);
    pmm_free_frame(old_phys); /* drop this address space's reference */
    return 1;
}

/* ---- Full user-half address-space reaping ---- */

static uint64_t reaped_frames_total = 0;
static uint64_t reaped_spaces_total = 0;

uint64_t paging_reaped_frames_total(void) { return reaped_frames_total; }
uint64_t paging_reaped_spaces_total(void) { return reaped_spaces_total; }

uint64_t paging_free_address_space(uint64_t pml4_phys) {
    if (pml4_phys == 0) return 0;

    /* Refuse to reap the live address space: that would yank away the page
     * tables we are currently walking and could fault the kernel. */
    uint64_t cur_cr3 = read_cr3() & PAGE_ADDR_MASK;
    if (pml4_phys == cur_cr3) {
        kprintf(VMM_TAG "WARN: refusing to reap active PML4 0x%016llx\n",
                (unsigned long long)pml4_phys);
        return 0;
    }
    if (pml4_phys == kernel_pml4_phys) {
        kprintf(VMM_TAG "WARN: refusing to reap kernel PML4\n");
        return 0;
    }

    uint64_t freed = 0;
    uint64_t *p4 = (uint64_t *)phys_to_ptr(pml4_phys);

    /* Walk only the USER half (entries 0..PML4_USER_TOP-1).  The kernel half
     * (256..511) is shared by every address space; we MUST NOT free it. */
    for (int i4 = 0; i4 < PML4_USER_TOP; i4++) {
        uint64_t e4 = p4[i4];
        if (!(e4 & PAGE_FLAG_PRESENT)) continue;
        uint64_t pdpt_phys = e4 & PAGE_ADDR_MASK;
        uint64_t *p3 = (uint64_t *)phys_to_ptr(pdpt_phys);

        for (int i3 = 0; i3 < 512; i3++) {
            uint64_t e3 = p3[i3];
            if (!(e3 & PAGE_FLAG_PRESENT)) continue;
            /* No 1 GiB pages in this kernel (we never set PS in PDPT). */
            uint64_t pd_phys = e3 & PAGE_ADDR_MASK;
            uint64_t *p2 = (uint64_t *)phys_to_ptr(pd_phys);

            for (int i2 = 0; i2 < 512; i2++) {
                uint64_t e2 = p2[i2];
                if (!(e2 & PAGE_FLAG_PRESENT)) continue;
                /* No 2 MiB pages either. */
                uint64_t pt_phys = e2 & PAGE_ADDR_MASK;
                uint64_t *p1 = (uint64_t *)phys_to_ptr(pt_phys);

                for (int i1 = 0; i1 < 512; i1++) {
                    uint64_t e1 = p1[i1];
                    if (!(e1 & PAGE_FLAG_PRESENT)) continue;
                    /* Only free pages that are USER-owned.  Defensive: even
                     * though we are walking the user half, a wild value
                     * shouldn't trick us into freeing a kernel/HHDM frame. */
                    if (!(e1 & PAGE_FLAG_USER)) continue;
                    uint64_t leaf_phys = e1 & PAGE_ADDR_MASK;
                    pmm_free_frame(leaf_phys);
                    freed++;
                    p1[i1] = 0;
                }
                pmm_free_frame(pt_phys);
                freed++;
                p2[i2] = 0;
            }
            pmm_free_frame(pd_phys);
            freed++;
            p3[i3] = 0;
        }
        pmm_free_frame(pdpt_phys);
        freed++;
        p4[i4] = 0;
    }

    /* Finally release the PML4 frame itself. */
    pmm_free_frame(pml4_phys);
    freed++;

    reaped_frames_total += freed;
    reaped_spaces_total++;
    return freed;
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

    /* The deliberate #PF on the unmapped address was demonstrated in Phase 4.
     * We no longer trigger it at boot (it would halt before later phases run).
     * The Phase 2 IDT + CR2 reporting remain live, so any real unmapped access
     * would still produce a clean fault dump. */
}

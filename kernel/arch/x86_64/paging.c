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
#include "kernel/arch/x86_64/smp.h"
#include "kernel/arch/x86_64/cpu_local.h"
#include "kernel/arch/x86_64/lapic.h"
#include "kernel/arch/x86_64/tlb_shootdown.h"
#include "kernel/arch/x86_64/tlb_policy.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vma.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/lib/spinlock.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/boot_info.h"

/* EFER (Extended Feature Enable Register) — Intel SDM Vol.3, 35.14 (AMD usage). */
#define MSR_EFER   0xC0000080
#define EFER_NXE   (1ULL << 11)   /* enables NX bit in PTEs */

#define CR4_SMEP   (1ULL << 20)
#define CR4_SMAP   (1ULL << 21)
#define CPUID7_EBX_SMEP (1U << 7)
#define CPUID7_EBX_SMAP (1U << 20)

#define VMM_TAG "[vmm] "

static uint64_t  hhdm;    /* higher-half direct-map offset (from Limine)     */
static uint64_t *pml4;    /* HHDM pointer to the KERNEL's PML4 (from CR3 @ boot).
                           * Since SMP step 3.2 this is no longer re-pointed on
                           * address-space switches; each CPU tracks its own
                           * current tables in cpu_local.vm_pml4 (see below).  */
static uint64_t  kernel_pml4_phys;  /* the kernel's PML4 (for switching back) */
volatile int cpu_smap_is_active = 0;

/* VM structure lock: serialises every page-table mutation and every
 * structure-creating walk across CPUs (map/unmap/protect, COW resolution,
 * fork/clone of an address space).  Without it, two CPUs touching the same
 * shared tables -- e.g. one expanding the kernel heap (the kernel half of
 * the page tables is SHARED by every address space, see
 * paging_new_address_space()) while another walks them, or CLONE_VM
 * siblings faulting on the same user page -- would tear entries under each
 * other.  Read-only lookups (paging_get_phys/get_flags) stay lock-free:
 * PTE updates are atomic 64-bit stores, matching the pre-SMP locking model.
 * Lock order (outermost first): vma_lock -> vm_lock -> pmm.lock; kheap and
 * slab sit strictly above vm_lock, so vm_lock must never call kmalloc. */
static spinlock_t vm_lock = SPINLOCK_UNLOCKED;

/* The page-table hierarchy THIS cpu is currently translating through.
 * Before cpu_local exists (early boot) -- or for kernel contexts that never
 * switched -- this is the kernel PML4 recorded by paging_init().  After SMP
 * bring-up every CPU's scheduler publishes its current address space into
 * cpu_local.vm_pml4 via paging_switch_to(), so a page fault handled on CPU1
 * walks CPU1's tables, not whatever CPU0 last happened to run. */
static uint64_t *vmm_current_tables(void) {
    if (cpu_local_ready) {
        struct cpu_local *cl = get_cpu_local();
        if (cl && cl->vm_pml4) {
            return (uint64_t *)(uintptr_t)cl->vm_pml4;
        }
    }
    return pml4;
}

/* Convert a physical address to a writable HHDM virtual pointer. */
static inline void *phys_to_ptr(uint64_t phys) {
    return (void *)(uintptr_t)(hhdm + phys);
}

/* Control-register bit definitions local to this file (cpu.h only exposes
 * raw accessors). */
#define CR0_WP   (1ULL << 16)  /* write-protect: ring-0 must fault on RO pages */
#define CR0_NE   (1ULL << 5)   /* numeric error: native FPU exception reporting */
#define CR0_MP   (1ULL << 1)   /* monitor coprocessor (paired with EM=0 for SSE) */
#define CR0_EM   (1ULL << 2)   /* emulation -- MUST be clear for SSE */
#define CR0_NW   (1ULL << 29)  /* not-write-through (reset leaves this+CD set) */
#define CR0_CD   (1ULL << 30)  /* cache disable (reset leaves this set!) */

#define CR4_OSFXSR     (1ULL << 9)   /* OS supports FXSAVE/SSE execution */
#define CR4_OSXMMEXCPT (1ULL << 10)  /* OS supports unmasked SSE exceptions */

/* Per-CPU control-register and feature parity: CR0, EFER, CR4.  Every CPU
 * comes out of reset (BSP) or INIT (each AP) with its OWN copy of these
 * registers, and the pre-SMP code normalised them exactly once -- for the
 * BSP (boot.asm, paging_init()).  An AP that starts scheduling real threads
 * without the same baseline breaks in ways that are individually baffling:
 *
 *   - CR4.OSFXSR = 0   -> every SSE instruction #UDs (observed: the
 *                         software-SSE 3D demo thread dying with vector 6
 *                         the moment it was scheduled on an AP);
 *   - CR0.WP    = 0    -> ring-0 writes to user COW pages SILENTLY succeed
 *                         instead of #PF-ing through the COW resolver,
 *                         corrupting shared pages for both parent and child;
 *   - CR0.CD/NW = 1    -> the AP runs with its caches OFF (reset state the
 *                         trampoline only OR-ed bits into), slowing that
 *                         core by an order of magnitude.
 *
 * paging_init() runs this for the BSP, ap_entry() (smp.c) for each AP; the
 * writes are idempotent.  Prints only on the BSP to keep the log quiet. */
void paging_cpu_features_init(void) {
    uint64_t cr0 = read_cr0();
    cr0 &= ~(CR0_EM | CR0_CD | CR0_NW);
    cr0 |=  CR0_MP | CR0_NE | CR0_WP;
    write_cr0(cr0);

    uint64_t efer = read_msr(MSR_EFER);
    write_msr(MSR_EFER, efer | EFER_NXE);

    uint32_t a, b, c, d;
    cpuid_count(7, 0, &a, &b, &c, &d);
    (void)a; (void)c; (void)d;
    uint64_t cr4 = read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT;
    int is_bsp = 1;
    if (cpu_local_ready) {
        struct cpu_local *cl = get_cpu_local();
        if (cl) is_bsp = (cl->cpu_id == 0);
    }
    if (b & CPUID7_EBX_SMEP) {
        cr4 |= CR4_SMEP;
        if (is_bsp) kprintf(VMM_TAG "SMEP enabled\n");
    }
    if (b & CPUID7_EBX_SMAP) {
        cr4 |= CR4_SMAP;
        cpu_smap_is_active = 1;
        if (is_bsp) kprintf(VMM_TAG "SMAP enabled\n");
    }
    write_cr4(cr4);

    /* HW_PLAN H3: program IA32_PAT entry PA4 = WC (0x01).  The low
     * four entries keep their reset defaults (WB/WT/UC-/UC), so every
     * existing mapping keeps its meaning; PA4 was an unused duplicate
     * of PA0 (reset value 0x0007040600070406 -- H0's printed fact).
     * A 4-KiB PTE selects PA4 with PAT=1, PCD=0, PWT=0 -- the combo
     * paging_fb_set_wc() writes.  PAT is a PER-CPU MSR and this
     * function is the one place that already runs on the BSP AND in
     * every AP's ap_entry() -- an AP left at the reset PAT while the
     * BSP writes WC PTEs would be an attribute-aliasing bug on metal
     * (TCG would never notice; that is exactly why it is fenced here
     * and not left to luck).  CPUID.1:EDX.16 gates it (D4: runtime,
     * no knob) -- PAT-less parts keep reset behaviour and the fb
     * remap refuses separately. */
    uint32_t d1, c1;
    cpuid_count(1, 0, &a, &b, &c1, &d1);
    if ((d1 >> 16) & 1) {
        uint64_t pat = read_msr(0x277);
        pat = (pat & ~(0x7ULL << 32)) | (0x1ULL << 32);   /* PA4 := WC */
        write_msr(0x277, pat);
        /* Print the READBACK, not the intent (D1): what the MSR says
         * after the write is the fact the smoke pins. */
        if (is_bsp) kprintf(VMM_TAG "IA32_PAT: PA4=WC (readback 0x%016llx)\n",
                            (unsigned long long)read_msr(0x277));
    }
}

void paging_init(void) {
    hhdm = boot_get_hhdm_offset();
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
    if (!(efer & EFER_NXE)) {
        kprintf(VMM_TAG "WARN: NXE was disabled, enabling it now\n");
    }
    write_msr(MSR_EFER, efer | EFER_NXE);

    /* SMEP/SMAP/OSFXSR/CR0-parity for the BSP (idempotent writes). */
    paging_cpu_features_init();

    kprintf(VMM_TAG "PML4 at phys 0x%016llx, HHDM 0x%016llx, NXE enabled\n",
            (unsigned long long)(cr3 & PAGE_ADDR_MASK),
            (unsigned long long)hhdm);
}

/*
 * Split an existing huge-page entry (`entry`, found at `*slot`) into a
 * freshly allocated next-level table whose 512 entries reproduce the exact
 * same physical mapping, preserving the original flags (including
 * cacheability and NX). `level` is 1 for a 1-GiB PDPTE (splits into 512
 * 2-MiB PD entries) or 2 for a 2-MiB PDE (splits into 512 4-KiB PT
 * entries). Returns the physical address of the new table, or 0 on OOM (in
 * which case *slot is left untouched).
 *
 * This exists because the boot loaders (BIOS Stage 2 and the UEFI stub)
 * pre-map all of low physical memory with 1 GiB/2 MiB pages for speed. When
 * a driver later asks the VMM to create a 4 KiB mapping (e.g. for a device's
 * MMIO BAR, or the Local APIC) that falls inside one of those huge pages,
 * the walk must not blindly reinterpret the huge-page's physical field as a
 * page-table pointer -- doing so hands the caller a PTE pointer that
 * actually aliases live MMIO/register space, silently corrupting whatever
 * device sits there. QEMU's TCG happens not to notice; real silicon can
 * raise a Machine Check Exception (observed: writing a 64-bit PTE straight
 * into the Local APIC's own register window during smp_init()).
 */
static uint64_t split_huge_page(uint64_t *slot, uint64_t entry, int level) {
    uint64_t new_phys = pmm_alloc_frame();
    if (new_phys == 0) {
        kprintf(VMM_TAG "OOM splitting huge page for finer mapping\n");
        return 0;
    }
    uint64_t *new_table = phys_to_ptr(new_phys);
    uint64_t base_phys = entry & PAGE_ADDR_MASK;
    uint64_t flags = entry & ~(PAGE_ADDR_MASK | PAGE_FLAG_PS);
    /* step = size spanned by each entry in the NEW (finer) table: a 1-GiB
     * PDPTE splits into 512 * 2-MiB PD entries; a 2-MiB PDE splits into
     * 512 * 4-KiB PT entries. */
    uint64_t step = (level == 1) ? (1ULL << 21) : (1ULL << 12);
    /* The new PD entries are still 2-MiB huge pages (PS=1); the new PT
     * entries are plain 4-KiB leaves and must NOT carry PS (that bit
     * position means PAT on a 4-KiB PTE). */
    uint64_t leaf_flag = (level == 1) ? PAGE_FLAG_PS : 0;
    for (uint64_t i = 0; i < 512; i++) {
        new_table[i] = (base_phys + i * step) | flags | leaf_flag;
    }
    *slot = new_phys | (entry & (PAGE_FLAG_PRESENT | PAGE_FLAG_WRITABLE | PAGE_FLAG_USER));
    return new_phys;
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
 *
 * If an intermediate level is already PRESENT but is a huge-page leaf
 * (PAGE_FLAG_PS set on a PDPTE/PDE -- as the boot-time identity/HHDM maps
 * always are), and `create` is set, the huge page is transparently split
 * into an equivalent next-level table so a finer-grained mapping can be
 * installed underneath it without corrupting the original mapping.
 */
static uint64_t *walk_pte(uint64_t virt, int create) {
    uint64_t *table = vmm_current_tables();

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
        } else if ((level == 1 || level == 2) && (entry & PAGE_FLAG_PS)) {
            /* PDPTE (level==1) or PDE (level==2) huge-page leaf: this
             * covers a 1 GiB / 2 MiB physical span with no page table of
             * its own.  If the caller wants to create a finer mapping
             * somewhere inside that span, split it first. Read-only walks
             * (create==0) instead fail, matching "not present at this
             * granularity" semantics for paging_get_phys()/friends -- those
             * callers only ever look up 4 KiB leaves anyway. */
            if (!create) {
                return NULL;
            }
            uint64_t new_phys = split_huge_page(&table[indices[level]], entry, level);
            if (new_phys == 0) {
                return NULL;
            }
            entry = table[indices[level]];
        }
        table = phys_to_ptr(entry & PAGE_ADDR_MASK);
    }

    /* `table` now points to the leaf PT; return the address of PTE[pt_index]. */
    return &table[PT_INDEX(virt)];
}

void paging_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t vf = spinlock_acquire_irqsave(&vm_lock);
    uint64_t *pte = walk_pte(virt, 1);
    if (pte == NULL) {
        spinlock_release_irqrestore(&vm_lock, vf);
        kprintf(VMM_TAG "map: failed to get PTE for 0x%016llx\n",
                (unsigned long long)virt);
        return;
    }
    *pte = (phys & PAGE_ADDR_MASK) | flags;
    /* Flush any stale TLB entry for this virtual address. */
    invlpg(virt);
    spinlock_release_irqrestore(&vm_lock, vf);
}

void paging_unmap(uint64_t virt) {
    uint64_t vf = spinlock_acquire_irqsave(&vm_lock);
    uint64_t *pte = walk_pte(virt, 0);
    if (pte == NULL || !(*pte & PAGE_FLAG_PRESENT)) {
        spinlock_release_irqrestore(&vm_lock, vf);
        return;   /* nothing to unmap */
    }
    *pte = 0;
    invlpg(virt);

    /* TLB Shootdown (O5: precise).  One page, one address space — the
     * mailbox carries both, targets outside this CR3 are skipped, and
     * fire-and-forget still holds (holding vm_lock here can never
     * deadlock against a spinning CPU; see tlb_shootdown.c for why an
     * ack protocol WOULD). Kernel-half pages broadcast (cr3_filter 0):
     * those mappings are shared by every PML4. */
    if (smp_get_cpu_count() > 1) {
        uint64_t asid_cr3 = (PML4_INDEX(virt) < PML4_USER_TOP) ? read_cr3() : 0;
        tlb_shootdown_range(asid_cr3, virt, 1);
    }
    spinlock_release_irqrestore(&vm_lock, vf);
}

int paging_protect(uint64_t virt, uint64_t flags) {
    uint64_t vf = spinlock_acquire_irqsave(&vm_lock);
    uint64_t *pte = walk_pte(virt, 0);
    if (pte == NULL || !(*pte & PAGE_FLAG_PRESENT)) {
        spinlock_release_irqrestore(&vm_lock, vf);
        return -1;
    }
    *pte = (*pte & PAGE_ADDR_MASK) | flags;
    invlpg(virt);
    spinlock_release_irqrestore(&vm_lock, vf);
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
 * Switch the active address space: update CR3 and the VMM's per-CPU current
 * tables pointer.  Safe because the kernel half (PML4 entries 256-511) is
 * shared, so kernel code, the heap, and all kernel stacks remain accessible
 * after the switch.  The pointer goes to THIS cpu's cpu_local.vm_pml4 slot:
 * with several CPUs scheduling different address spaces concurrently, a
 * global "current pml4" would be overwritten by whichever CPU switched last
 * and every page-fault walk would roam the wrong hierarchy.
 */
void paging_switch_to(uint64_t new_pml4_phys) {
    if (new_pml4_phys == 0) {
        return;
    }
    write_cr3(new_pml4_phys);
    if (cpu_local_ready) {
        struct cpu_local *cl = get_cpu_local();
        if (cl) {
            /* O5: publish for the shootdown sender's skip filter. */
            tlb_note_cr3((uint32_t)cl->cpu_id, new_pml4_phys);
            cl->vm_pml4 = (uint64_t)(uintptr_t)phys_to_ptr(new_pml4_phys);
            return;
        }
    }
    pml4 = (uint64_t *)phys_to_ptr(new_pml4_phys);   /* early boot fallback */
}

/* Update only the tables pointer (used after a manual CR3 write in the
 * scheduler): same per-CPU semantics as paging_switch_to(), minus the CR3
 * write the caller has already performed. */
void paging_update_pml4_ptr(uint64_t phys) {
    if (cpu_local_ready) {
        struct cpu_local *cl = get_cpu_local();
        if (cl) {
            cl->vm_pml4 = (uint64_t)(uintptr_t)phys_to_ptr(phys);
            return;
        }
    }
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

    /* SMP safety: this walk reads AND writes the forking thread's own page
     * tables (marking writable leaves read-only + COW in the parent).  With
     * CLONE_VM siblings possibly faulting on other CPUs at the same time,
     * the walk must be atomic against every other page-table mutator
     * (vm_lock) and against VMA-list edits (the caller's vma_lock) -- so the
     * VMA_SHARED lookup below cannot be mid-munmap.  Lock order is
     * vma_lock -> vm_lock, matching the mprotect syscall path. */
    tcb_t *parent = sched_current();
    uint64_t vflags = 0;
    if (parent) {
        vflags = spinlock_acquire_irqsave(&parent->vma_lock);
    }
    uint64_t vmflags = spinlock_acquire_irqsave(&vm_lock);
    int marked_cow = 0;

    uint64_t *src = vmm_current_tables();
    for (int i4 = 0; i4 < PML4_USER_TOP; i4++) {
        if (!(src[i4] & PAGE_FLAG_PRESENT)) continue;
        uint64_t *o_pdpt = phys_to_ptr(src[i4] & PAGE_ADDR_MASK);
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
                    uint64_t virt = ((uint64_t)i4 << 39) |
                                    ((uint64_t)i3 << 30) |
                                    ((uint64_t)i2 << 21) |
                                    ((uint64_t)i1 << 12);
                    int is_shared_vma = 0;
                    if (parent) {   /* parent's vma_lock is held for the walk */
                        vma_t *vma = vma_find(parent->vma_list, virt);
                        is_shared_vma = (vma && (vma->flags & VMA_SHARED)) ? 1 : 0;
                    }

                    if (pmm_inc_frame_ref(old_phys) != 0) goto fail;

                    if (!is_shared_vma && (flags & (PAGE_FLAG_WRITABLE | PAGE_FLAG_COW))) {
                        flags &= ~PAGE_FLAG_WRITABLE;
                        flags |= PAGE_FLAG_COW;
                        o_pt[i1] = old_phys | flags;
                        invlpg(virt);
                        marked_cow = 1;
                    }
                    n_pt[i1] = old_phys | flags;
                }
            }
        }
    }
    spinlock_release_irqrestore(&vm_lock, vmflags);
    if (parent) {
        spinlock_release_irqrestore(&parent->vma_lock, vflags);
    }
    /* If we just write-protected the parent's pages for COW, sibling threads
     * of the parent running on OTHER cpus with this same CR3 may still hold
     * stale WRITABLE TLB entries -- one stray write through such an entry
     * would silently bypass COW.  Flush them once, after the whole walk. */
    if (marked_cow && smp_get_cpu_count() > 1) {
        /* O5: the write-protected pages are scattered across the whole
         * parent space — npages 0 requests a full flush, but only on
         * CPUs actually running the parent's CR3. */
        tlb_shootdown_range(read_cr3(), 0, 0);
    }
    return new_pml4_phys;

fail:
    spinlock_release_irqrestore(&vm_lock, vmflags);
    if (parent) {
        spinlock_release_irqrestore(&parent->vma_lock, vflags);
    }
    (void)paging_free_address_space(new_pml4_phys);
    return 0;
}

int paging_handle_cow_fault(uint64_t fault_addr, uint64_t err_code) {
    /* COW faults are present write-protection faults.  The U/S bit may be 0
     * when the kernel writes to a user COW page via copy_to_user(). */
    if ((err_code & 0x3ULL) != 0x3ULL) return 0;

    uint64_t virt = fault_addr & ~(PAGE_SIZE_BYTES - 1ULL);
    if (PML4_INDEX(virt) >= PML4_USER_TOP) return 0;

    /* Serialise against concurrent COW resolutions of the SAME pte by a
     * CLONE_VM sibling on another cpu (and every other table mutator). */
    uint64_t vf = spinlock_acquire_irqsave(&vm_lock);

    uint64_t *pte = walk_pte(virt, 0);
    if (!pte || !(*pte & PAGE_FLAG_PRESENT) || !(*pte & PAGE_FLAG_USER) ||
        !(*pte & PAGE_FLAG_COW)) {
        spinlock_release_irqrestore(&vm_lock, vf);
        return 0;
    }

    uint64_t old_phys = *pte & PAGE_ADDR_MASK;
    uint64_t flags = (*pte & ~PAGE_ADDR_MASK) & ~PAGE_FLAG_COW;
    flags |= PAGE_FLAG_WRITABLE;

    uint32_t refs = pmm_get_frame_refcount(old_phys);
    if (refs <= 1) {
        *pte = old_phys | flags;
        invlpg(virt);
        spinlock_release_irqrestore(&vm_lock, vf);
        /* Siblings on other cpus (CLONE_VM) may hold stale entries for this
         * same CR3: without a shootdown their next write would fault again
         * and, seeing COW already cleared, be misdiagnosed as a real SEGV. */
        if (smp_get_cpu_count() > 1) {
            tlb_shootdown_range(read_cr3(), virt, 1);   /* O5: precise */
        }
        return 1;
    }

    uint64_t new_phys = pmm_alloc_frame();
    if (!new_phys) {
        spinlock_release_irqrestore(&vm_lock, vf);
        return 0;
    }
    memcpy(phys_to_ptr(new_phys), phys_to_ptr(old_phys), PAGE_SIZE_BYTES);

    *pte = new_phys | flags;
    invlpg(virt);
    spinlock_release_irqrestore(&vm_lock, vf);
    pmm_free_frame(old_phys); /* drop this address space's reference */
    if (smp_get_cpu_count() > 1) {
        tlb_shootdown_range(read_cr3(), virt, 1);       /* O5: precise */
    }
    return 1;
}

/* Stale-TLB "spurious fault" detector for the SMP page-fault path.
 *
 * Once two CPUs can run threads that share one address space (CLONE_VM), a
 * fault may arrive on a CPU whose TLB still holds an old entry even though
 * another CPU has ALREADY resolved the very same fault (COW copy, demand
 * map).  Re-running the resolver would misbehave (double map, or SIGSEGV
 * on a perfectly legal write), so the #PF path calls this helper before
 * treating the fault as real: it returns 1 when the current page tables
 * already permit the faulting access, after invalidating the stale local
 * TLB entry. */
int paging_user_fault_resolved(uint64_t fault_addr, uint64_t err_code) {
    uint64_t virt = fault_addr & ~(PAGE_SIZE_BYTES - 1ULL);
    if (PML4_INDEX(virt) >= PML4_USER_TOP) return 0;   /* not a user address */

    /* Read-only walk under the VM lock so we cannot observe a table being
     * built halfway by another cpu.  The PTE itself is an atomic 64-bit
     * slot, but the hierarchy above it can be mid-creation. */
    uint64_t vf = spinlock_acquire_irqsave(&vm_lock);
    uint64_t *pte = walk_pte(virt, 0);
    if (!pte || !(*pte & PAGE_FLAG_PRESENT) || !(*pte & PAGE_FLAG_USER)) {
        spinlock_release_irqrestore(&vm_lock, vf);
        return 0;
    }
    uint64_t e = *pte;
    spinlock_release_irqrestore(&vm_lock, vf);

    /* Does the installed PTE already permit exactly what faulted? */
    if ((err_code & 0x2ULL) && !(e & (PAGE_FLAG_WRITABLE | PAGE_FLAG_COW))) {
        return 0;   /* genuine write to read-only page */
    }
    if ((err_code & 0x10ULL) && (e & PAGE_FLAG_NO_EXEC)) {
        return 0;   /* genuine execute of NX page */
    }
    /* Yes: stale-TLB artefact of a concurrent resolver.  Flush and retry. */
    invlpg(virt);
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

/* ---- HW_PLAN H3: the write-combining framebuffer -------------------------
 *
 * The framebuffer is reached through the HHDM, which the boot maps as
 * plain WB huge pages -- correct, but on metal every pixel store then
 * competes for cache lines and orders like normal memory.  WC is the
 * memory type made for exactly this traffic (streaming stores, no
 * read-for-ownership), and it needs two pieces: the PAT entry
 * (PA4=WC, programmed per-CPU in paging_cpu_features_init) and PTEs
 * that select it.  A 4-KiB PTE selects PA4 with PAT=1 PCD=0 PWT=0;
 * the PAT bit on a 4-KiB PTE is bit 7 -- the same position PS holds
 * one level up, which is why the flag reuses PAGE_FLAG_PS below and
 * says so out loud.
 *
 * walk_pte(create=1) transparently splits the HHDM's 1-GiB/2-MiB
 * leaves into 4-KiB PTEs for the touched range only (split_huge_page
 * exists for precisely this shape of job), so the remap is exact to
 * the framebuffer's pitch*height bytes and neighbours keep WB.
 *
 * TCG ignores memory types entirely -- the gui suite gating this
 * phase proves pixel-identity, and the THROUGHPUT claim stays a
 * metal receipt (HW_PLAN D2/\u00a76).  What TCG does prove: the split,
 * the flags, the flush, and a boot that still draws. */
void paging_fb_set_wc(void) {
    boot_fb_t *fb = boot_get_framebuffer();
    if (fb == NULL || fb->phys_base == 0) {
        kprintf(VMM_TAG "fb: none present; WC remap skipped\n");
        return;
    }

    uint32_t a, b, c, d;
    cpuid_count(1, 0, &a, &b, &c, &d);
    if (!((d >> 16) & 1)) {
        kprintf(VMM_TAG "fb: CPU has no PAT; keeping boot-time attributes\n");
        return;
    }

    uint64_t len   = (uint64_t)fb->pitch * (uint64_t)fb->height;
    uint64_t base  = hhdm + fb->phys_base;
    uint64_t pages = 0;

    for (uint64_t off = 0; off < len; off += PAGE_SIZE_BYTES) {
        uint64_t *pte = walk_pte(base + off, 1);
        if (pte == NULL || !(*pte & PAGE_FLAG_PRESENT)) {
            kprintf(VMM_TAG "fb: page at +0x%llx not mapped; WC remap "
                    "aborted after %llu pages\n",
                    (unsigned long long)off, (unsigned long long)pages);
            return;
        }
        /* PAT=1 (bit 7 == PAGE_FLAG_PS's position on a 4-KiB PTE),
         * PCD=0, PWT=0 => PAT entry 4 => WC. */
        *pte = (*pte | PAGE_FLAG_PS)
             & ~(PAGE_FLAG_CACHE_DISABLE | PAGE_FLAG_WRITE_THROUGH);
        invlpg(base + off);
        pages++;
    }

    /* The probe line the smokes pin: decode what the FIRST PTE really
     * says now, not what this function meant to write. */
    uint64_t *pte0 = walk_pte(base, 0);
    if (pte0 != NULL) {
        kprintf(VMM_TAG "fb: WC via PAT4 (%llu pages; PTE PAT=%d PCD=%d "
                "PWT=%d)\n",
                (unsigned long long)pages,
                (int)!!(*pte0 & PAGE_FLAG_PS),
                (int)!!(*pte0 & PAGE_FLAG_CACHE_DISABLE),
                (int)!!(*pte0 & PAGE_FLAG_WRITE_THROUGH));
    }
}

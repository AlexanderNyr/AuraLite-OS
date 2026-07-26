/* boot/uefi/efi_paging.c -- install page tables that mirror the BIOS
 * path's virtual layout, so the kernel finds the same three regions
 * regardless of which loader booted it:
 *
 *   virt 0x0000000000000000..0x40000000    identity, 1 GiB
 *   virt 0xffff800000000000..0xffff800040000000  HHDM, 1 GiB (shares PD0)
 *   virt 0xFFFFFFFF80000000..0xFFFFFFFF80400000  kernel image, 4 MiB
 *
 * UEFI itself hands us a CPU already in long mode with an identity
 * map covering the whole system.  When we call ExitBootServices the
 * firmware page tables stay active, but we do not trust them past
 * the moment we start doing MMIO-sensitive stuff -- so we build our
 * own CR3 immediately afterwards.
 *
 * Page-table pages are allocated statically from a fixed physical
 * region.  On BL5's BIOS path the tables live at 0x01000000 but that
 * assumes the E820 map does not report the region as reserved.  On
 * UEFI the memory map can be more crowded; we let the firmware pick
 * the pages via AllocatePages and record the PML4 physical address
 * in a global for the assembly trampoline to load into CR3.
 *
 * This file is compiled --target=x86_64-unknown-windows -ffreestanding.
 */

#include <stdint.h>
#include "boot/uefi/efi_types.h"

#define PTE_P    0x001
#define PTE_W    0x002
#define PTE_PS   0x080

/* Slots for the six page-table pages plus a live PML4 address.  We
 * expose PML4_PHYS so efi_trampoline.S can `mov cr3, PML4_PHYS`
 * atomically with the same instruction sequence the BIOS path uses. */
extern uint64_t pml4_phys;
uint64_t pml4_phys = 0;

/* efi_setup_hhdm_paging -- allocate PTs and populate them.
 *
 * Layout (mirrors BL4 but with a wider HHDM to reach the GOP MMIO and 
 * using 2-MiB pages instead of 1-GiB pages to support Intel Sandy Bridge 
 * CPUs which do not support 1-GiB page sizes):
 *
 *   PML4[0]   -> PDPT_ident  : identity map 0..64 GiB (64 * 1-GiB PDPTEs
 *                              each pointing to its own Page Directory (PD) page
 *                              which maps 512 * 2-MiB entries).
 *   PML4[256] -> PDPT_hhdm   : HHDM identical to PDPT_ident, sharing
 *                              the same 64 * 1-GiB pages, so virt
 *                              0xffff800000000000 .. +64 GiB reaches the
 *                              framebuffer MMIO too (GOP FB usually
 *                              sits at phys 0x80000000+ on OVMF).
 *   PML4[511] -> PDPT_khigh  : kernel higher half via a small PD with
 *                              two 2-MiB entries (kernel image only).
 *
 * We allocate 68 physical pages:
 *   Page 0: PML4
 *   Page 1: PDPT_ident
 *   Page 2: PDPT_khigh
 *   Page 3: PD_kernel
 *   Pages 4..67: 64 PD pages (each mapping 1 GiB using 2-MiB pages)
 *
 * Returns 0 on success, non-zero on allocation failure. */
EFI_STATUS efi_setup_hhdm_paging(EFI_BOOT_SERVICES *BS) {
    uint64_t base = 0;
    EFI_STATUS s = BS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                     68, &base);
    if (s != EFI_SUCCESS) return s;

    uint64_t *pml4      = (uint64_t *)(uintptr_t)(base + 0*4096);
    uint64_t *pdpt_id   = (uint64_t *)(uintptr_t)(base + 1*4096);
    uint64_t *pdpt_high = (uint64_t *)(uintptr_t)(base + 2*4096);
    uint64_t *pd_kernel = (uint64_t *)(uintptr_t)(base + 3*4096);

    /* Zero all 68 pages to make sure we don't have garbage entries. */
    for (uint64_t *p = pml4; p < pml4 + 68*512; p++) *p = 0;

    /* Populate the 64 PD_ident pages. Each page has 512 entries, each mapping 2 MiB.
     * Page i (where i is 0..63) maps the physical range [i * 1 GiB, (i+1) * 1 GiB). */
    for (uint64_t i = 0; i < 64; i++) {
        uint64_t *pd_ident_page = (uint64_t *)(uintptr_t)(base + (4 + i)*4096);
        
        /* PDPT_ident entry i points to this PD page */
        pdpt_id[i] = (uint64_t)(uintptr_t)pd_ident_page | PTE_P | PTE_W;
        
        /* Fill the 512 entries of the PD page with 2-MiB page entries */
        for (uint64_t j = 0; j < 512; j++) {
            uint64_t phys_addr = (i << 30) + (j << 21);
            pd_ident_page[j] = phys_addr | PTE_PS | PTE_P | PTE_W;
        }
    }

    /* PDPT_khigh[510]: kernel virt 0xFFFFFFFF80000000..+2 MiB. */
    pdpt_high[510] = (uint64_t)(uintptr_t)pd_kernel | PTE_P | PTE_W;

    /* PD_kernel: two 2-MiB pages mapping phys 0..4 MiB. */
    pd_kernel[0] = (0x00000000ULL) | PTE_PS | PTE_P | PTE_W;
    pd_kernel[1] = (0x00200000ULL) | PTE_PS | PTE_P | PTE_W;

    /* PML4: identity and HHDM share the same PDPT (both cover 0..64 GiB).
     * Kernel higher half at PML4[511]. */
    pml4[0]   = (uint64_t)(uintptr_t)pdpt_id   | PTE_P | PTE_W;
    pml4[256] = (uint64_t)(uintptr_t)pdpt_id   | PTE_P | PTE_W;
    pml4[511] = (uint64_t)(uintptr_t)pdpt_high | PTE_P | PTE_W;

    pml4_phys = (uint64_t)(uintptr_t)pml4;
    return EFI_SUCCESS;
}

/* Install the freshly-built PML4 as CR3.  Written in inline asm so
 * we do not need a separate .S file for such a tiny helper.  Must be
 * called only after ExitBootServices (before that the firmware still
 * owns the current PML4 and stomping CR3 may confuse it). */
void efi_activate_paging(void) {
    __asm__ volatile (
        "mov %0, %%cr3\n"
        :
        : "r"(pml4_phys)
        : "memory");
}

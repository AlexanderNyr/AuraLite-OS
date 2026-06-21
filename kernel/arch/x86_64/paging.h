#ifndef NOVOS_ARCH_X86_64_PAGING_H
#define NOVOS_ARCH_X86_64_PAGING_H

#include <stdint.h>

/*
 * 4-level paging (PML4 -> PDPT -> PD -> PT) virtual memory manager for x86_64.
 *
 * Limine enters the kernel with paging already enabled: the kernel is mapped
 * higher-half, and a higher-half direct map (HHDM) of all physical memory is
 * available.  The VMM builds on top of that:
 *
 *   - It reads the current PML4 from CR3 (set up by Limine).
 *   - It walks / creates page tables in the CURRENT address space.
 *   - Page-table frames are reached through the HHDM (phys + HHDM offset),
 *     avoiding the chicken-and-egg of "map a table before you can manage it."
 *
 * Virtual address space layout (target):
 *
 *   0x0000000000000000 .. 0x00007FFFFFFFFFFF   user space (128 TiB)
 *   0xFFFF800000000000 .. 0xFFFFBFFFFFFFFFFF   HHDM direct map (Limine)
 *   0xFFFFFFFF80000000 .. 0xFFFFFFFFFFFFFFFF   kernel image + heap + stacks
 *
 * For now the kernel manages only the existing (Limine-provided) address space.
 * Per-process address spaces are created with paging_new_address_space().
 */

/* ---- Page-table entry flags (Intel SDM Vol.3, 4.3) ---- */
#define PAGE_FLAG_PRESENT  (1ULL << 0)   /* bit 0  */
#define PAGE_FLAG_WRITABLE (1ULL << 1)   /* bit 1  */
#define PAGE_FLAG_USER     (1ULL << 2)   /* bit 2  */
#define PAGE_FLAG_NO_EXEC  (1ULL << 63)  /* bit 63 (requires EFER.NXE) */

/* ---- Constants ---- */
#define PAGE_SIZE_BYTES  4096
#define PAGE_ADDR_MASK   0x000FFFFFFFFFF000ULL  /* physical addr field (12..51) */

/* Canonical user/ kernel split: PML4 entries 0-255 are user, 256-511 kernel. */
#define PML4_USER_TOP     256

/* ---- Index extraction from a virtual address ---- */
#define PML4_INDEX(addr) (((addr) >> 39) & 0x1FF)
#define PDPT_INDEX(addr) (((addr) >> 30) & 0x1FF)
#define PD_INDEX(addr)   (((addr) >> 21) & 0x1FF)
#define PT_INDEX(addr)   (((addr) >> 12) & 0x1FF)

/*
 * Initialise the VMM: record the current PML4 (from CR3) and enable the NX
 * (No-Execute) bit via EFER.NXE so that PAGE_FLAG_NO_EXEC is honoured.
 */
void paging_init(void);

/*
 * Map a single 4 KiB page: virt -> phys with the given flags (OR of
 * PAGE_FLAG_*).  Creates intermediate page tables (PML4e, PDPTe, PDe) as
 * needed, allocating frames from the PMM and zeroing them.
 */
void paging_map(uint64_t virt, uint64_t phys, uint64_t flags);

/* Unmap a single page (clear its PTE) and invalidate the TLB entry. */
void paging_unmap(uint64_t virt);

/*
 * Translate a virtual address to its mapped physical address.
 * @returns the physical page base, or 0 if not present.
 * (0 is also the IVT frame, never mapped, so it doubles as "not present.")
 */
uint64_t paging_get_phys(uint64_t virt);

/*
 * Allocate and initialise a fresh PML4 for a new process, copying the kernel
 * half (entries 256-511) from the current address space so the kernel remains
 * mapped.  @returns the physical address of the new PML4, or 0 on failure.
 * (Used by the ELF loader / process creation; untested until Phase 8.)
 */
uint64_t paging_new_address_space(void);

/* Gate self-test: map a page, verify R/W via both virt and HHDM, unmap,
 * confirm the translation is gone, then deliberately fault on the unmapped
 * address to demonstrate clean #PF handling. */
void paging_self_test(void);

#endif /* NOVOS_ARCH_X86_64_PAGING_H */

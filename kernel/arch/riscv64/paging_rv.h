/* kernel/arch/riscv64/paging_rv.h -- Sv39 paging (RISCV_PLAN V3). */

#ifndef AURALITE_ARCH_RISCV64_PAGING_RV_H
#define AURALITE_ARCH_RISCV64_PAGING_RV_H

#include <stdint.h>

#define PAGE_SIZE_RV 4096u

/* D3: the Sv39 direct map.  boot.S's early tables make it true before
 * the first C instruction; paging_rv_init()'s final tables keep it
 * true with real permissions (RW, never X). */
#define HHDM_OFFSET 0xFFFFFFC000000000UL

static inline void *p2v_rv(uint64_t phys)
{
    return (void *)(phys + HHDM_OFFSET);
}

static inline uint64_t v2p_rv(const void *virt)
{
    return (uint64_t)virt - HHDM_OFFSET;
}

/* PTE bits (privileged spec 4.4).  A PTE with R=W=X=0 is a pointer to
 * the next level; anything else is a leaf at that level. */
#define PTE_V (1UL << 0)
#define PTE_R (1UL << 1)
#define PTE_W (1UL << 2)
#define PTE_X (1UL << 3)
#define PTE_U (1UL << 4)
#define PTE_A (1UL << 6)
#define PTE_D (1UL << 7)

/* Build the final kernel tables (higher-half sections with real W^X,
 * HHDM as RW megapages, NO identity window) and switch satp to them.
 * After this returns, a low virtual address is a fault. */
void paging_rv_init(void);

/* 4 KiB mappings in the final tables.  flags = PTE_R/W/X/U as needed
 * (PTE_V, PTE_A, PTE_D are added internally).  Returns 0 or -1. */
int  paging_rv_map(uint64_t va, uint64_t pa, uint64_t flags);
int  paging_rv_unmap(uint64_t va);

/* Physical address behind va, or ~0UL if not mapped. */
uint64_t paging_rv_probe(uint64_t va);

/* The [vmm] gate: map/write/alias-read/unmap, then the three fault
 * probes (store to .text, execute from data, load from the dropped
 * identity window).  Returns 0 on pass. */
int  paging_rv_selftest(void);

#endif /* AURALITE_ARCH_RISCV64_PAGING_RV_H */

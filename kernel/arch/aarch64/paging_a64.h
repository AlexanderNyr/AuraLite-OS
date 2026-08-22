/* kernel/arch/aarch64/paging_a64.h -- TTBR1 39-bit paging (ARM64_PLAN A3). */

#ifndef AURALITE_ARCH_AARCH64_PAGING_A64_H
#define AURALITE_ARCH_AARCH64_PAGING_A64_H

#include <stdint.h>

#define PAGE_SIZE_A64 4096u

/* D3: the direct map -- the SAME constant as riscv64's, by TTBR1
 * arithmetic rather than copy-paste: T1SZ=25 opens a 512 GiB window
 * at 0xFFFFFF8000000000, and the HHDM sits 256 GiB in, at level-1
 * index 256 -- the exact index the exact constant occupies in Sv39.
 * boot.S's early tables make it true before the first higher-half
 * instruction; paging_a64_init()'s final tables keep it true with
 * real attributes (Normal WB for RAM, Device-nGnRE for MMIO, RW,
 * never X). */
#define HHDM_OFFSET 0xFFFFFFC000000000UL

static inline void *p2v_a64(uint64_t phys)
{
    return (void *)(phys + HHDM_OFFSET);
}

static inline uint64_t v2p_a64(const void *virt)
{
    return (uint64_t)virt - HHDM_OFFSET;
}

/* Descriptor bits (VMSAv8-64, stage 1).  Unlike Sv39 there is no
 * standalone "leaf" flag pattern: bit 1 distinguishes table/page at
 * levels above 3 and BLOCK (0) from PAGE (1) encodings; permissions
 * live in AP[2:1] plus the two execute-never bits -- the second of
 * which (UXN vs PXN) is the "W^X twice over" bonus D3 promised. */
#define PTE_VALID   (1UL << 0)
#define PTE_TABLE   (1UL << 1)   /* levels 1-2: next-level pointer */
#define PTE_PAGE    (1UL << 1)   /* level 3: page (must be set) */
#define PTE_ATTR(i) ((uint64_t)(i) << 2)   /* MAIR index */
#define PTE_AP_RO   (1UL << 7)   /* AP[2]: read-only at EL1 */
#define PTE_AP_EL0  (1UL << 6)   /* AP[1]: EL0 access (A4) */
#define PTE_SH_IS   (3UL << 8)   /* inner shareable */
#define PTE_AF      (1UL << 10)  /* access flag (set up front) */
#define PTE_PXN     (1UL << 53)  /* privileged execute-never */
#define PTE_UXN     (1UL << 54)  /* unprivileged execute-never */

/* MAIR indices, fixed in boot.S and honoured by every mapping:
 * 0 = Device-nGnRnE (MMIO), 1 = Normal WB (RAM). */
#define MAIR_IDX_DEVICE 0
#define MAIR_IDX_NORMAL 1

/* Convenience permission bundles for paging_a64_map (the caller says
 * WHAT the memory is; the function says how that spells in bits). */
#define A64_MAP_RW_NORMAL  0     /* kernel data: RW, XN both ways */
#define A64_MAP_RO_NORMAL  1     /* rodata: R, XN both ways */
#define A64_MAP_RX_NORMAL  2     /* text: R+X (PXN clear, UXN set) */
#define A64_MAP_RW_DEVICE  3     /* MMIO: RW, XN, Device-nGnRE */
#define A64_MAP_RX_USER    4     /* EL0 text: R+X at EL0, PXN (A4) */
#define A64_MAP_RW_USER    5     /* EL0 data/stack: RW, XN both ways */
#define A64_MAP_RO_USER    6     /* EL0 rodata: R at EL0, XN both ways (A5c) */

/* Build the final kernel tables (higher-half sections with real W^X,
 * HHDM as 2 MiB Normal-WB blocks, MMIO re-attributed Device, and
 * TTBR0 DROPPED -- the identity window dies) and switch TTBR1/TTBR0.
 * After this returns, a low virtual address is a fault. */
void paging_a64_init(void);

/* R5: final roots (0 until paging_a64_init ran). */
extern uint64_t paging_a64_final_ttbr1;
extern uint64_t paging_a64_final_ttbr0;

/* 4 KiB mappings in the final tables.  kind = A64_MAP_*. */
int  paging_a64_map(uint64_t va, uint64_t pa, int kind);
int  paging_a64_unmap(uint64_t va);

/* Physical address behind va, or ~0UL if not mapped. */
uint64_t paging_a64_probe(uint64_t va);

/* A7: MAIR AttrIndx of the leaf mapping at va (MAIR_IDX_DEVICE /
 * MAIR_IDX_NORMAL), or -1 if unmapped -- the virtio transport's
 * attach-time Device-attribute gate reads this. */
int paging_a64_attr_index(uint64_t va);

/* The [vmm] gate: map/write/alias-read/unmap, then the three fault
 * probes (store to .text, execute from data, load from the dropped
 * identity window).  Returns 0 on pass. */
int  paging_a64_selftest(void);

#endif /* AURALITE_ARCH_AARCH64_PAGING_A64_H */

/* kernel/arch/aarch64/paging_a64.c -- TTBR1 39-bit paging (ARM64_PLAN A3).
 *
 * Three-level walk, 512-entry tables, 1 GiB blocks at level 1 and
 * 2 MiB at level 2 -- the Sv39 shape by choice (D3), so this file is
 * paging_rv.c's structure move for move.  What is genuinely aarch64:
 *
 *   - attributes are indirect (MAIR index in the PTE, plan Fact 5.2):
 *     RAM maps Normal WB, the MMIO windows map Device-nGnRE, and the
 *     kind bundles in paging_a64.h spell it so callers cannot get a
 *     bit wrong;
 *   - permissions are AP[2] (RO) plus TWO execute-never bits: kernel
 *     .text runs PXN-clear but UXN-SET (user never executes kernel
 *     pages -- the W^X-twice-over bonus), everything else carries
 *     both XN bits;
 *   - the break-before-make discipline: TLBI VMALLE1IS + dsb ish +
 *     isb in ONE helper (Fact 5.3), not scattered;
 *   - the identity window dies differently: it lives in TTBR0, so
 *     the final tables simply point TTBR0 at an EMPTY root -- the
 *     whole low half becomes translation faults in one register
 *     write (Sv39 needed the entry dropped; here the hardware gives
 *     the low half its own root and we hand it a blank one).
 *
 * W^X is enforceable and therefore enforced: no descriptor in the
 * final tables is writable and executable at any EL.
 */

#include <stdint.h>

#include "kernel/arch/aarch64/paging_a64.h"
#include "kernel/arch/aarch64/pl011.h"
#include "kernel/arch/aarch64/pmm_a64.h"
#include "kernel/arch/aarch64/trap_a64.h"

extern const uint64_t kernel_layout[8];
#define L_KSTART_PHYS  0
#define L_BOOTEND_PHYS 1
#define L_TEXT_START   2
#define L_TEXT_END     3
#define L_RO_START     4
#define L_RO_END       5
#define L_DATA_START   6
#define L_KEND_PHYS    7

#define ENTRIES     512
#define BLOCK_2M    (2UL * 1024 * 1024)

static uint64_t *root_hi;              /* final TTBR1 root (HHDM view) */
static uint64_t *root_lo;              /* the TTBR0 root: BLANK at switch
                                        * time (the identity window dies),
                                        * populated LATER by user mappings
                                        * only -- A4's EL0 pages live here.
                                        * The measured fact that created
                                        * this comment: VA 0x40000000 is
                                        * the LOW half, and the low half
                                        * translates through TTBR0 on this
                                        * ISA -- one Sv39 root covers all
                                        * of VA, a VMSAv8 pair does not.
                                        * The first EL0 entry Instruction-
                                        * Aborted at its own entry point
                                        * because the user text had been
                                        * mapped into the WRONG TREE. */

static void puts_(const char *s) { pl011_puts(s); }
static void put_hex(uint64_t v)  { pl011_puthex64(v); }

/* PA field: bits [47:12]. */
static inline uint64_t pte_pa(uint64_t pte) { return pte & 0x0000FFFFFFFFF000UL; }

/* One helper owns the barrier discipline (Fact 5.3). */
static void tlb_flush_all(void)
{
    __asm__ volatile("dsb ishst\n\t"
                     "tlbi vmalle1\n\t"
                     "dsb ish\n\t"
                     "isb" ::: "memory");
}

static uint64_t *table_alloc(void)
{
    uint64_t pa = pmm_a64_alloc_frame();
    if (!pa)
        return 0;
    uint64_t *t = (uint64_t *)p2v_a64(pa);
    for (int i = 0; i < ENTRIES; i++)
        t[i] = 0;
    return t;
}

/* Spell a `kind` into descriptor bits for a level-3 page entry. */
static uint64_t kind_bits(int kind)
{
    switch (kind) {
    case A64_MAP_RX_NORMAL:
        return PTE_ATTR(MAIR_IDX_NORMAL) | PTE_SH_IS | PTE_AF |
               PTE_AP_RO | PTE_UXN;                 /* R+X, PXN clear */
    case A64_MAP_RO_NORMAL:
        return PTE_ATTR(MAIR_IDX_NORMAL) | PTE_SH_IS | PTE_AF |
               PTE_AP_RO | PTE_UXN | PTE_PXN;
    case A64_MAP_RW_DEVICE:
        return PTE_ATTR(MAIR_IDX_DEVICE) | PTE_SH_IS | PTE_AF |
               PTE_UXN | PTE_PXN;
    case A64_MAP_RX_USER:
        /* EL0 text: readable+executable at EL0, read-only at EL1,
         * PXN SET -- the kernel must never execute user pages (W^X's
         * second axis, the A4 counterpart of A3's UXN-on-kernel). */
        return PTE_ATTR(MAIR_IDX_NORMAL) | PTE_SH_IS | PTE_AF |
               PTE_AP_RO | PTE_AP_EL0 | PTE_PXN;
    case A64_MAP_RW_USER:
        /* EL0 data/stack: RW at EL0 (and thus EL1 -- VMSAv8 couples
         * them), executable NOWHERE. */
        return PTE_ATTR(MAIR_IDX_NORMAL) | PTE_SH_IS | PTE_AF |
               PTE_AP_EL0 | PTE_UXN | PTE_PXN;
    case A64_MAP_RO_USER:
        /* EL0 rodata (A5c): readable at EL0 and EL1, writable nowhere
         * (AP[2]|AP[1] = RO-all), executable nowhere.  The loader's
         * PF-neither bundle -- rodata is enforced read-only here, not
         * merely unwritten. */
        return PTE_ATTR(MAIR_IDX_NORMAL) | PTE_SH_IS | PTE_AF |
               PTE_AP_RO | PTE_AP_EL0 | PTE_UXN | PTE_PXN;
    case A64_MAP_RW_NORMAL:
    default:
        return PTE_ATTR(MAIR_IDX_NORMAL) | PTE_SH_IS | PTE_AF |
               PTE_UXN | PTE_PXN;
    }
}

/* Walk to the descriptor for va at `level` (1 = 1 GiB, 2 = 2 MiB,
 * 3 = 4 KiB), allocating intermediate tables when `create`.  The
 * root is CHOSEN BY THE VA: low half = TTBR0's tree (user), high
 * half = TTBR1's (kernel) -- the two-trees fact above. */
static uint64_t *walk(uint64_t va, int level, int create)
{
    uint64_t *t;
    if (va < (1UL << 39))
        t = root_lo;
    else if (va >= 0xFFFFFF8000000000UL)
        t = root_hi;
    else
        return 0;                      /* the unmappable middle */
    if (!t)
        return 0;
    for (int l = 1; l < level; l++) {
        uint64_t idx = (va >> (12 + 9 * (3 - l))) & 0x1FF;
        uint64_t pte = t[idx];
        if (!(pte & PTE_VALID)) {
            if (!create)
                return 0;
            uint64_t *nt = table_alloc();
            if (!nt)
                return 0;
            t[idx] = pte_pa(v2p_a64(nt)) | PTE_VALID | PTE_TABLE;
            t = nt;
        } else {
            if (!(pte & PTE_TABLE))
                return 0;              /* a block already covers va */
            t = (uint64_t *)p2v_a64(pte_pa(pte));
        }
    }
    return &t[(va >> (12 + 9 * (3 - level))) & 0x1FF];
}

/* Skip-if-present map at one level; level 3 entries get PTE_PAGE,
 * coarser levels are blocks (bit 1 clear).  The paging_rv map_range
 * semantics: what lets the HHDM block sweep run AFTER the 4 KiB
 * section maps without trampling them. */
static int map_range(uint64_t va, uint64_t pa, uint64_t len,
                     uint64_t bits, int level)
{
    uint64_t step = (level == 2) ? BLOCK_2M : PAGE_SIZE_A64;
    uint64_t leaf_tag = (level == 3) ? PTE_PAGE : 0;
    for (uint64_t off = 0; off < len; off += step) {
        uint64_t *pte = walk(va + off, level, 1);
        if (!pte)
            return -1;
        if (*pte & PTE_VALID)
            continue;
        *pte = pte_pa(pa + off) | bits | PTE_VALID | leaf_tag;
    }
    return 0;
}

/* ---- public 4 KiB API ----------------------------------------------------- */

int paging_a64_map(uint64_t va, uint64_t pa, int kind)
{
    uint64_t *pte = walk(va, 3, 1);
    if (!pte)
        return -1;
    *pte = pte_pa(pa) | kind_bits(kind) | PTE_VALID | PTE_PAGE;
    tlb_flush_all();
    return 0;
}

int paging_a64_unmap(uint64_t va)
{
    uint64_t *pte = walk(va, 3, 0);
    if (!pte || !(*pte & PTE_VALID))
        return -1;
    *pte = 0;
    /* [AMEND-4] (OPT O5's cross-arch note made task): this ISA hands
     * out per-VA invalidation as ONE instruction -- x86_64 had to
     * build mailboxes and IPIs for the same precision.  TLBI VAE1IS
     * wants VA[55:12] in bits [43:0]; inner-shareable covers future
     * SMP (D5's exit ramp) at no cost today.  tlb_flush_all() remains
     * the documented fallback for whole-space teardowns. */
    __asm__ volatile("dsb ishst\n\t"
                     "tlbi vae1is, %0\n\t"
                     "dsb ish\n\t"
                     "isb"
                     :: "r"(va >> 12) : "memory");
    return 0;
}

uint64_t paging_a64_probe(uint64_t va)
{
    for (int level = 1; level <= 3; level++) {
        uint64_t *pte = walk(va, level, 0);
        if (!pte || !(*pte & PTE_VALID))
            continue;
        int is_leaf = (level == 3) || !(*pte & PTE_TABLE);
        if (is_leaf) {
            uint64_t mask = (1UL << (12 + 9 * (3 - level))) - 1;
            return pte_pa(*pte) + (va & mask);
        }
    }
    return ~0UL;
}

/* ---- init ------------------------------------------------------------------ */

void paging_a64_init(void)
{
    root_hi = table_alloc();
    root_lo = table_alloc();

    /* The paging_rv_init order, which is the algorithm (its comment
     * transfers verbatim): sections first at 4 KiB, then the split
     * blocks' remainder, then the HHDM sweep -- reversed, the sweep's
     * blocks would gate the section tables and the kernel would go
     * unprotected. */
    uint64_t text_s = kernel_layout[L_TEXT_START];
    uint64_t text_e = kernel_layout[L_TEXT_END];
    uint64_t ro_s   = kernel_layout[L_RO_START];
    uint64_t ro_e   = kernel_layout[L_RO_END];
    uint64_t data_s = kernel_layout[L_DATA_START];
    uint64_t kend_v = kernel_layout[L_KEND_PHYS] + HHDM_OFFSET;
    uint64_t kstart_phys = kernel_layout[L_KSTART_PHYS];

    /* 1: sections.  W and X never together -- and never for EL0. */
    map_range(text_s, v2p_a64((void *)text_s), text_e - text_s,
              kind_bits(A64_MAP_RX_NORMAL), 3);        /* .text  RX  */
    map_range(ro_s, v2p_a64((void *)ro_s), ro_e - ro_s,
              kind_bits(A64_MAP_RO_NORMAL), 3);        /* .rodata R  */
    map_range(data_s, v2p_a64((void *)data_s), kend_v - data_s,
              kind_bits(A64_MAP_RW_NORMAL), 3);        /* data+bss RW */

    /* 2: the split blocks' remainder at 4 KiB RW (never X) --
     * includes .boot's frames: it already ran, now it is plain RAM. */
    uint64_t span_s = (kstart_phys + HHDM_OFFSET) & ~(BLOCK_2M - 1);
    uint64_t span_e = (kend_v + BLOCK_2M - 1) & ~(BLOCK_2M - 1);
    map_range(span_s, span_s - HHDM_OFFSET, span_e - span_s,
              kind_bits(A64_MAP_RW_NORMAL), 3);

    /* 3a: the MMIO plateau, Device-nGnRE at 4 KiB, BEFORE the RAM
     * sweep (skip-if-present means first writer wins): PL011, GICD,
     * GICC, the 32 virtio windows -- everything the kernel touches
     * lives in PA 0x08000000..0x0A004000 on this board (A1 measured
     * the bases; one 4 KiB run over the range covers them all). */
    map_range(HHDM_OFFSET + 0x08000000UL, 0x08000000UL,
              0x02010000UL, kind_bits(A64_MAP_RW_DEVICE), 3);

    /* 3b: the HHDM RAM sweep, 2 MiB Normal WB blocks, skip-if-
     * present.  RAM starts at 0x40000000 on this board; sweeping
     * from 0 would attribute the MMIO hole Normal, and Normal
     * attributes on device registers permit reordering/combining --
     * the exact bug class Fact 5.2 names.  Sweep RAM only. */
    map_range(HHDM_OFFSET + 0x40000000UL, 0x40000000UL,
              4UL * 1024 * 1024 * 1024 - 0x40000000UL,
              kind_bits(A64_MAP_RW_NORMAL) & ~(PTE_PAGE), 2);

    /* Switch: TTBR1 -> the final tables; TTBR0 -> the (still-)BLANK
     * user root: the identity window dies in one register write --
     * Sv39 dropped an entry, VMSAv8 hands the low half its own root
     * and at THIS moment it is empty.  A4 populates it with EL0
     * pages through the same walk(); the identity VA stays unmapped
     * forever (the selftest's third probe keeps that honest). */
    uint64_t t1 = v2p_a64(root_hi);
    uint64_t t0 = v2p_a64(root_lo);
    __asm__ volatile("msr ttbr1_el1, %0\n\t"
                     "msr ttbr0_el1, %1" :: "r"(t1), "r"(t0));
    tlb_flush_all();

    puts_("[vmm]  TTBR1 final tables live: .text RX+UXN, .rodata R, data RW, "
          "HHDM Normal-WB; MMIO Device-nGnRE; TTBR0 blanked\n");
    puts_("[vmm]  W^X holds twice over: no descriptor is W+X at any EL "
          "(UXN on every kernel page)\n");
    puts_("[vmm]  root at phys ");
    put_hex(t1);
    puts_("\n");
}

/* ---- the [vmm] gate --------------------------------------------------------
 *
 * Positive: map / write / alias-read / unmap at 4 KiB.
 * Negative, three fault probes (the paging_rv_selftest trio, ESR-
 * flavoured -- EC 0x25 Data Abort for the stores/loads, EC 0x21
 * Instruction Abort for execute-from-data):
 *   1. store to .text          -- W^X, write half
 *   2. execute from data       -- W^X, execute half (PXN earning it)
 *   3. load from the old identity window -- proves TTBR0 is blank
 */

static void probe_store_text(void *arg)
{
    (void)arg;
    *(volatile uint64_t *)kernel_layout[L_TEXT_START] = 0;
}

static uint64_t exec_probe_buf[2] = { 0xD65F03C0 /* ret */, 0 };

static void probe_exec_data(void *arg)
{
    (void)arg;
    void (*fn)(void) = (void (*)(void))exec_probe_buf;
    fn();
}

static void probe_load_identity(void *arg)
{
    (void)arg;
    (void)*(volatile uint64_t *)0x40200000UL;   /* the old identity VA */
}

int paging_a64_selftest(void)
{
    /* -- positive ---------------------------------------------------- */
    uint64_t frame = pmm_a64_alloc_frame();
    if (!frame)
        return -1;

    uint64_t probe_va = 0xFFFFFFD000000000UL;   /* far from HHDM traffic */
    if (paging_a64_map(probe_va, frame, A64_MAP_RW_NORMAL) != 0)
        return -1;

    volatile uint64_t *hi = (volatile uint64_t *)probe_va;
    volatile uint64_t *lo = (volatile uint64_t *)p2v_a64(frame);
    *hi = 0x41415243483634UL;                   /* "AARCH64" */
    if (*lo != 0x41415243483634UL)
        return -1;                              /* alias read failed */

    if (paging_a64_unmap(probe_va) != 0)
        return -1;
    if (paging_a64_probe(probe_va) != ~0UL)
        return -1;
    pmm_a64_free_frame(frame);
    puts_("[vmm]  map / write / alias-read / unmap correct at 4 KiB\n");

    /* -- negative: three faults, each expected, named, unwound ------- */

    if (trap_run_fault_probe_a64(0x25, 0x25, probe_store_text, 0) != 0) {
        puts_("[vmm]  FAIL: store to .text did not fault\n");
        return -1;
    }
    puts_("[vmm]  store to .text faulted (W^X write half)\n");

    if (trap_run_fault_probe_a64(0x21, 0x21, probe_exec_data, 0) != 0) {
        puts_("[vmm]  FAIL: execute-from-data did not fault\n");
        return -1;
    }
    puts_("[vmm]  execute-from-data faulted (W^X execute half -- PXN "
          "earning its keep)\n");

    if (trap_run_fault_probe_a64(0x25, 0x25, probe_load_identity, 0) != 0) {
        puts_("[vmm]  FAIL: identity-window load did not fault\n");
        return -1;
    }
    puts_("[vmm]  identity window confirmed dropped (TTBR0 blank: low "
          "load faults)\n");

    return 0;
}

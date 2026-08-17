/* kernel/arch/riscv64/paging_rv.c -- Sv39 paging (RISCV_PLAN V3).
 *
 * Three-level walk; 512 GiB of VA per root table half.  boot.S's
 * early table (assembly-time gigapages, identity window included)
 * got the kernel up here; this file builds the FINAL tables:
 *
 *   - kernel sections with real permissions from kernel_layout:
 *       .boot   -- not mapped at its low VMA at all (it already ran);
 *                  its frames are plain HHDM RW like any other RAM
 *       .text   R+X   .rodata  R      .data/.bss  R+W
 *   - HHDM: RAM as 2 MiB RW megapages, plus 4 KiB RW windows for the
 *     MMIO the kernel already uses (UART, PLIC)
 *   - NO identity window.  The V3 gate faults on a low load to prove
 *     its absence, the way I3 proved the dropped window with eip=c01.
 *
 * W^X is enforceable here and therefore enforced: no PTE in the final
 * tables carries W and X together -- the property i386 could not have
 * (plan D3's honest ❌ gets its green sibling on this arch).
 */

#include <stdint.h>

#include "kernel/arch/riscv64/paging_rv.h"
#include "kernel/arch/riscv64/pmm_rv.h"
#include "kernel/arch/riscv64/sbi.h"
#include "kernel/arch/riscv64/trap.h"

/* boot.S exports the linker layout as data (medany cannot reach the
 * absolute low symbols from up here). */
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
#define MEGAPAGE    (2UL * 1024 * 1024)

static uint64_t *root;                 /* final root table (HHDM view) */

static void puts_(const char *s) { sbi_puts(s); }
static void put_hex(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    sbi_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        sbi_putc(hex[(v >> shift) & 0xF]);
}

static inline uint64_t pte_pa(uint64_t pte)   { return (pte >> 10) << 12; }
static inline uint64_t pa_pte(uint64_t pa)    { return (pa >> 12) << 10; }
static inline int      pte_leaf(uint64_t pte) { return pte & (PTE_R | PTE_W | PTE_X); }

static uint64_t *table_alloc(void)
{
    uint64_t pa = pmm_rv_alloc_frame();
    if (!pa)
        return 0;
    uint64_t *t = (uint64_t *)p2v_rv(pa);
    for (int i = 0; i < ENTRIES; i++)
        t[i] = 0;
    return t;
}

/* Walk to the PTE for va at `level` (2=1 GiB slot, 1=2 MiB, 0=4 KiB),
 * allocating intermediate tables when `create`.  Returns 0 if absent
 * and !create, or on OOM. */
static uint64_t *walk(uint64_t va, int level, int create)
{
    uint64_t *t = root;
    for (int l = 2; l > level; l--) {
        uint64_t idx = (va >> (12 + 9 * l)) & 0x1FF;
        uint64_t pte = t[idx];
        if (!(pte & PTE_V)) {
            if (!create)
                return 0;
            uint64_t *nt = table_alloc();
            if (!nt)
                return 0;
            t[idx] = pa_pte(v2p_rv(nt)) | PTE_V;
            t = nt;
        } else {
            if (pte_leaf(pte))
                return 0;              /* a bigger page already covers va */
            t = (uint64_t *)p2v_rv(pte_pa(pte));
        }
    }
    return &t[(va >> (12 + 9 * level)) & 0x1FF];
}

/* Skip-if-present semantics: an entry that is already a table pointer
 * (a finer-grained region) or an existing leaf is left alone.  This
 * is what lets the HHDM megapage sweep run AFTER the 4 KiB section
 * maps without trampling them. */
static int map_range(uint64_t va, uint64_t pa, uint64_t len,
                     uint64_t flags, int level)
{
    uint64_t step = level == 1 ? MEGAPAGE : PAGE_SIZE_RV;
    for (uint64_t off = 0; off < len; off += step) {
        uint64_t *pte = walk(va + off, level, 1);
        if (!pte)
            return -1;
        if (*pte & PTE_V)
            continue;
        *pte = pa_pte(pa + off) | flags | PTE_V | PTE_A | PTE_D;
    }
    return 0;
}

/* ---- public 4 KiB API ----------------------------------------------------- */

int paging_rv_map(uint64_t va, uint64_t pa, uint64_t flags)
{
    uint64_t *pte = walk(va, 0, 1);
    if (!pte)
        return -1;
    *pte = pa_pte(pa) | flags | PTE_V | PTE_A | PTE_D;
    __asm__ volatile("sfence.vma %0" :: "r"(va) : "memory");
    return 0;
}

int paging_rv_unmap(uint64_t va)
{
    uint64_t *pte = walk(va, 0, 0);
    if (!pte || !(*pte & PTE_V))
        return -1;
    *pte = 0;
    __asm__ volatile("sfence.vma %0" :: "r"(va) : "memory");
    return 0;
}

uint64_t paging_rv_probe(uint64_t va)
{
    for (int level = 2; level >= 0; level--) {
        uint64_t *pte = walk(va, level, 0);
        if (!pte || !(*pte & PTE_V))
            continue;
        if (pte_leaf(*pte)) {
            uint64_t mask = (1UL << (12 + 9 * level)) - 1;
            return pte_pa(*pte) + (va & mask);
        }
    }
    return ~0UL;
}

/* ---- init ------------------------------------------------------------------ */

void paging_rv_init(void)
{
    root = table_alloc();

    /* Order is the algorithm here.  map_range skips present entries
     * and walk() will not put a table pointer under a leaf, so:
     *
     *   1. kernel sections at 4 KiB with real permissions -- these
     *      claim their 2 MiB regions as fine-grained tables;
     *   2. the rest of those SAME 2 MiB regions at 4 KiB RW -- the
     *      HHDM's promise for the non-section pages (.boot's frames,
     *      the early root table, tail padding);
     *   3. the HHDM sweep: 4 GiB of RW 2 MiB megapages -- skips every
     *      region step 1 already carved, covers all other RAM and the
     *      MMIO plateau (UART 0x10000000, PLIC 0x0c000000).
     *
     * Reversed, step 3's leaves would block step 1's tables and the
     * kernel would go unprotected -- so the order is load-bearing. */
    uint64_t text_s = kernel_layout[L_TEXT_START];
    uint64_t text_e = kernel_layout[L_TEXT_END];
    uint64_t ro_s   = kernel_layout[L_RO_START];
    uint64_t ro_e   = kernel_layout[L_RO_END];
    uint64_t data_s = kernel_layout[L_DATA_START];
    uint64_t kend_v = kernel_layout[L_KEND_PHYS] + HHDM_OFFSET;
    uint64_t kstart_phys = kernel_layout[L_KSTART_PHYS];

    /* 1: sections.  W and X never together -- W^X by construction. */
    map_range(text_s, v2p_rv((void *)text_s), text_e - text_s,
              PTE_R | PTE_X, 0);                       /* .text  RX */
    map_range(ro_s, v2p_rv((void *)ro_s), ro_e - ro_s,
              PTE_R, 0);                               /* .rodata R */
    map_range(data_s, v2p_rv((void *)data_s), kend_v - data_s,
              PTE_R | PTE_W, 0);                       /* .data+.bss RW */

    /* 2: the split megapages' remainder, RW (never X). */
    uint64_t span_s = (kstart_phys + HHDM_OFFSET) & ~(MEGAPAGE - 1);
    uint64_t span_e = (kend_v + MEGAPAGE - 1) & ~(MEGAPAGE - 1);
    map_range(span_s, span_s - HHDM_OFFSET, span_e - span_s,
              PTE_R | PTE_W, 0);

    /* 3: the HHDM sweep, 2 MiB RW megapages, skip-if-present. */
    map_range(HHDM_OFFSET, 0, 4UL * 1024 * 1024 * 1024,
              PTE_R | PTE_W, 1);

    /* satp -> the final tables.  NO identity entry was created: the
     * low half of the VA space is now guaranteed to fault. */
    uint64_t satp = (8UL << 60) | (v2p_rv(root) >> 12);
    __asm__ volatile("csrw satp, %0; sfence.vma" :: "r"(satp) : "memory");

    puts_("[vmm]  Sv39 final tables live: .text RX, .rodata R, data RW, "
          "HHDM RW; identity window dropped\n");
    puts_("[vmm]  W^X holds by construction: no PTE carries W+X "
          "(the bit i386 did not have)\n");
    puts_("[vmm]  root at phys ");
    put_hex(v2p_rv(root));
    puts_("\n");
}

/* ---- the [vmm] gate --------------------------------------------------------
 *
 * Positive: map / write / alias-read / unmap at 4 KiB.
 * Negative (the phase's whole point, three fault probes):
 *   1. store to .text          -- W^X, write half
 *   2. execute from data       -- W^X, execute half (NEW on this arch;
 *                                  i386 could not fault this)
 *   3. load from the old identity window -- proves it dropped
 *
 * Each probe arms trap.c's expectation, faults, and is resumed past
 * by the handler bumping sepc; "did it fault" is the assertion.
 */

/* The three probe bodies.  Each must fault; returning is failure. */

static void probe_store_text(void *arg)
{
    (void)arg;
    *(volatile uint64_t *)kernel_layout[L_TEXT_START] = 0;
}

/* Static, not stack: .data is RW-never-X in the final tables just
 * like the stack, and a fixed address makes the log reproducible. */
static uint64_t exec_probe_buf[2] = { 0x00008067 /* ret */, 0 };

static void probe_exec_data(void *arg)
{
    (void)arg;
    void (*fn)(void) = (void (*)(void))exec_probe_buf;
    fn();
}

static void probe_load_identity(void *arg)
{
    (void)arg;
    (void)*(volatile uint64_t *)0x80200000UL;   /* the old identity VA */
}

int paging_rv_selftest(void)
{
    /* -- positive ---------------------------------------------------- */
    uint64_t frame = pmm_rv_alloc_frame();
    if (!frame)
        return -1;

    uint64_t probe_va = 0xFFFFFFD000000000UL;   /* far from HHDM traffic */
    if (paging_rv_map(probe_va, frame, PTE_R | PTE_W) != 0)
        return -1;

    volatile uint64_t *hi  = (volatile uint64_t *)probe_va;
    volatile uint64_t *lo  = (volatile uint64_t *)p2v_rv(frame);
    *hi = 0x52495343563339UL;                   /* "RISCV39" */
    if (*lo != 0x52495343563339UL)
        return -1;                              /* alias read failed */

    if (paging_rv_unmap(probe_va) != 0)
        return -1;
    if (paging_rv_probe(probe_va) != ~0UL)
        return -1;
    pmm_rv_free_frame(frame);
    puts_("[vmm]  map / write / alias-read / unmap correct at 4 KiB\n");

    /* -- negative: three faults, each expected, named, unwound ------- */

    if (trap_run_fault_probe(7 /* Store/AMO Access Fault */,
                             15 /* Store/AMO Page Fault */,
                             probe_store_text, 0) != 0) {
        puts_("[vmm]  FAIL: store to .text did not fault\n");
        return -1;
    }
    puts_("[vmm]  store to .text faulted (W^X write half)\n");

    if (trap_run_fault_probe(1 /* Instruction Access Fault */,
                             12 /* Instruction Page Fault */,
                             probe_exec_data, 0) != 0) {
        puts_("[vmm]  FAIL: execute-from-data did not fault\n");
        return -1;
    }
    puts_("[vmm]  execute-from-data faulted (W^X execute half -- "
          "impossible to prove on i386)\n");

    if (trap_run_fault_probe(5 /* Load Access Fault */,
                             13 /* Load Page Fault */,
                             probe_load_identity, 0) != 0) {
        puts_("[vmm]  FAIL: identity-window load did not fault\n");
        return -1;
    }
    puts_("[vmm]  identity window confirmed dropped (low load faults)\n");

    return 0;
}

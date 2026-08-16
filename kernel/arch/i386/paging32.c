/* kernel/arch/i386/paging32.c -- non-PAE 2-level paging (I386_PLAN I3).
 *
 * boot32.asm built the boot page directory (PSE direct map + identity
 * window) before the higher-half jump; this file owns everything after:
 * dropping the identity window, 4 KiB mappings for the heap and (in I4)
 * user space, and the boot self-test in the vmm PASS contract.
 *
 * PDE/PTE bit layout (Intel SDM Vol.3 s.4.3, 32-bit paging):
 *   bit 0 P, bit 1 R/W, bit 2 U/S, bit 7 PS (PDE only).
 * PAGE32_FLAG_NO_EXEC is accepted and ignored -- no PAE, no NX bit;
 * the honest statement lives in I386_PLAN D3 and docs/status.md.
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/i386/paging32.h"
#include "kernel/arch/i386/pmm32.h"
#include "kernel/arch/i386/kprintf32.h"

#define PDE_P   (1u << 0)
#define PDE_RW  (1u << 1)
#define PDE_US  (1u << 2)
#define PDE_PS  (1u << 7)
#define PTE_P   (1u << 0)
#define PTE_RW  (1u << 1)
#define PTE_US  (1u << 2)

/* Defined in boot32.asm (.boot section, physical address == virtual via
 * the direct map once paging is on). */
extern uint32_t boot_page_directory[];

static uint32_t *page_dir;   /* direct-mapped pointer to the PD */

static inline void invlpg(uint32_t virt)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void paging32_init(void)
{
    /* The PD lives in .boot, below 896 MiB, so the direct map reaches
     * it.  Its physical address IS the .boot symbol's address (VMA=LMA
     * there); go through p2v_32 for the post-paging pointer. */
    page_dir = (uint32_t *)p2v_32((uint32_t)boot_page_directory);

    kprintf32("[vmm] PD at phys %x, direct map %u MiB at %x, PSE 4 MiB pages\n",
              (uint32_t)boot_page_directory,
              DIRECT_MAP_BYTES / (1024 * 1024), KERNEL_VBASE_32);
    kprintf32("[vmm] no PAE => no NX: W^X for user pages is NOT enforceable "
              "on i386 (plan D3)\n");
}

void paging32_drop_identity(void)
{
    /* PDEs 0..223 were only needed for the instruction after the PG
     * flip.  With execution higher-half, NULL-page dereferences must
     * fault, so the window goes. */
    for (uint32_t i = 0; i < DIRECT_MAP_BYTES / (4u * 1024 * 1024); i++)
        page_dir[i] = 0;

    /* Full TLB flush: reload CR3. */
    uint32_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");

    kprintf32("[vmm] identity window [0, %u MiB) dropped; NULL now faults\n",
              DIRECT_MAP_BYTES / (1024 * 1024));
}

int paging32_map(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t pdi = virt >> 22;
    uint32_t pti = (virt >> 12) & 0x3FF;

    uint32_t pde = page_dir[pdi];
    if (pde & PDE_PS)
        return -1;                        /* refuse to split a 4 MiB page */

    uint32_t *pt;
    if (!(pde & PDE_P)) {
        uint32_t pt_phys = pmm32_alloc_frame();
        if (!pt_phys)
            return -1;
        pt = (uint32_t *)p2v_32(pt_phys);
        for (int i = 0; i < 1024; i++)
            pt[i] = 0;
        /* PDE carries the union of permissions; PTEs restrict. */
        page_dir[pdi] = pt_phys | PDE_P | PDE_RW |
                        ((flags & PAGE32_FLAG_USER) ? PDE_US : 0);
    } else {
        pt = (uint32_t *)p2v_32(pde & ~0xFFFu);
        if ((flags & PAGE32_FLAG_USER) && !(pde & PDE_US))
            page_dir[pdi] |= PDE_US;
    }

    if (pt[pti] & PTE_P)
        return -1;                        /* already mapped: caller bug */

    pt[pti] = (phys & ~0xFFFu) | PTE_P |
              ((flags & PAGE32_FLAG_WRITE) ? PTE_RW : 0) |
              ((flags & PAGE32_FLAG_USER)  ? PTE_US : 0);
    invlpg(virt);
    return 0;
}

int paging32_unmap(uint32_t virt)
{
    uint32_t pdi = virt >> 22;
    uint32_t pti = (virt >> 12) & 0x3FF;

    uint32_t pde = page_dir[pdi];
    if (!(pde & PDE_P) || (pde & PDE_PS))
        return -1;

    uint32_t *pt = (uint32_t *)p2v_32(pde & ~0xFFFu);
    if (!(pt[pti] & PTE_P))
        return -1;

    pt[pti] = 0;
    invlpg(virt);
    return 0;
}

int paging32_probe(uint32_t virt, uint32_t *phys_out)
{
    uint32_t pdi = virt >> 22;
    uint32_t pti = (virt >> 12) & 0x3FF;

    uint32_t pde = page_dir[pdi];
    if (!(pde & PDE_P))
        return 0;

    if (pde & PDE_PS) {
        if (phys_out)
            *phys_out = (pde & 0xFFC00000u) | (virt & 0x003FFFFFu);
        return 1;
    }

    uint32_t *pt = (uint32_t *)p2v_32(pde & ~0xFFFu);
    if (!(pt[pti] & PTE_P))
        return 0;
    if (phys_out)
        *phys_out = (pt[pti] & ~0xFFFu) | (virt & 0xFFFu);
    return 1;
}

/* Same shape as the x86_64 vmm self-test: map a fresh page at a probe
 * address, write/read through it, unmap, confirm the probe goes dark. */
int paging32_selftest(void)
{
    /* Above the heap window: 0xFC000000 is past the direct map's
     * 0xF8000000 ceiling AND past the heap, so its PDE starts empty.
     * The first attempt used 0xE0000000, which is *inside* the PSE
     * direct map -- paging32_map correctly refused to split the 4 MiB
     * page and the self-test caught the layout bug at first boot. */
    const uint32_t probe = 0xFC000000u;

    kprintf32("[vmm] self-test: mapping %x...\n", probe);

    uint32_t frame = pmm32_alloc_frame();
    if (!frame)
        return -1;

    if (paging32_map(probe, frame, PAGE32_FLAG_WRITE) != 0)
        return -1;

    volatile uint32_t *p = (volatile uint32_t *)probe;
    *p = 0xC0DEC0DEu;
    if (*p != 0xC0DEC0DEu)
        return -1;

    /* The same frame through the direct map must see the store. */
    volatile uint32_t *alias = (volatile uint32_t *)p2v_32(frame);
    if (*alias != 0xC0DEC0DEu)
        return -1;

    uint32_t phys = 0;
    if (!paging32_probe(probe, &phys) || phys != frame)
        return -1;

    if (paging32_unmap(probe) != 0)
        return -1;
    if (paging32_probe(probe, NULL))
        return -1;

    pmm32_free_frame(frame);
    kprintf32("[vmm] PASS: map / write / alias-read / unmap all correct\n");
    return 0;
}

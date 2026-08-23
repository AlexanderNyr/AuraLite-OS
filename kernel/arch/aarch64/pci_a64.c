/* kernel/arch/aarch64/pci_a64.c -- the aarch64 tenant's ECAM glue
 * (RESIDUE_PLAN R7, ledger RES-20/21).
 *
 * Measured board facts: reg=<0x40_10000000 +0x10000000> -- the ECAM
 * sits ABOVE 4 GiB (QEMU's highmem layout for direct-kernel boots),
 * where the HHDM formula would wrap right out of the 39-bit window.
 * So the ECAM gets a VA CARVE instead: bus 0's 1 MiB is mapped
 * Device-nGnRE at HHDM + 0x20000000 -- a hole in the direct map
 * (phys 0x0A010000..0x3F000000 is neither the A3 device plateau nor
 * RAM, so those VAs are free by construction).  The BAR window IS
 * low (mem32 at 0x10000000, also inside the hole) and keeps the
 * HHDM-formula VA, mapped page by page when a BAR lands.
 *
 * Only bus 0 is mapped: 1 MiB covers 32 devices x 8 functions, and
 * the walker's scope is bus 0 by contract (a bridge would be named,
 * not followed).
 */

#include <stdint.h>

#include "kernel/arch/aarch64/pci_a64.h"
#include "kernel/arch/aarch64/paging_a64.h"
#include "kernel/arch/aarch64/vmmio_a64.h"
#include "kernel/arch/aarch64/pl011.h"

#define ECAM_CARVE_VA (HHDM_OFFSET + 0x20000000UL)
#define ECAM_MAP_LEN  0x100000UL         /* bus 0: 256 x 4 KiB */

static struct pci_ecam ecam;
static int ecam_live;

volatile uint8_t *pci_a64_map_mmio(uint64_t pa, uint32_t len)
{
    uint64_t p0 = pa & ~(uint64_t)(PAGE_SIZE_A64 - 1);
    uint64_t p1 = (pa + len + PAGE_SIZE_A64 - 1)
                  & ~(uint64_t)(PAGE_SIZE_A64 - 1);
    for (uint64_t p = p0; p < p1; p += PAGE_SIZE_A64)
        if (paging_a64_map(HHDM_OFFSET + p, p, A64_MAP_RW_DEVICE) != 0)
            return 0;
    return (volatile uint8_t *)p2v_a64(pa);
}

int pci_a64_init(const fdt_platform_t *plat)
{
    if (ecam_live)
        return 0;                        /* idempotent: the vblk
                                          * fallback may walk first */
    if (plat->pcie_ecam_base == 0 || plat->pcie_mmio_size == 0) {
        pl011_puts("[pci] no pci-host-ecam-generic in the tree\n");
        return -1;
    }
    for (uint64_t off = 0; off < ECAM_MAP_LEN; off += PAGE_SIZE_A64) {
        if (paging_a64_map(ECAM_CARVE_VA + off,
                           plat->pcie_ecam_base + off,
                           A64_MAP_RW_DEVICE) != 0) {
            pl011_puts("[pci] ECAM carve mapping failed\n");
            return -1;
        }
    }
    ecam.va        = (volatile uint8_t *)ECAM_CARVE_VA;
    ecam.bus_limit = 1;                  /* the carve covers bus 0 */
    ecam.ops       = vmmio_a64_ops();
    ecam.mmio_cpu  = plat->pcie_mmio_cpu;
    ecam.mmio_pci  = plat->pcie_mmio_pci;
    ecam.mmio_size = plat->pcie_mmio_size;
    ecam.mmio_cursor = 0;
    if (pci_ecam_walk(&ecam) < 0)
        return -1;
    ecam_live = 1;
    return 0;
}

struct pci_ecam *pci_a64_ecam(void)
{
    return ecam_live ? &ecam : 0;
}

/* kernel/arch/riscv64/pci_rv.c -- the rv64 tenant's ECAM glue
 * (RESIDUE_PLAN R7, ledger RES-20/21).
 *
 * Measured board facts (the DTB, not folklore): the ECAM window is
 * reg=<0x30000000 +0x10000000> and the 32-bit non-prefetchable BAR
 * window is 0x40000000 (pci == cpu on this board -- both numbers
 * still travel separately through the seam).  Everything below 4 GiB
 * sits inside the HHDM the final Sv39 tables already map, so
 * "mapping" here is the p2v formula; the aarch64 twin has to work
 * for its ECAM (above 4 GiB there) and that asymmetry lives in the
 * tenants, not in the shared walker.
 */

#include <stdint.h>

#include "kernel/arch/riscv64/pci_rv.h"
#include "kernel/arch/riscv64/paging_rv.h"
#include "kernel/arch/riscv64/vmmio_rv.h"
#include "kernel/arch/riscv64/sbi.h"

static struct pci_ecam ecam;
static int ecam_live;

volatile uint8_t *pci_rv_map_mmio(uint64_t pa, uint32_t len)
{
    (void)len;                           /* HHDM covers the window */
    return (volatile uint8_t *)p2v_rv(pa);
}

int pci_rv_init(const fdt_platform_t *plat)
{
    if (ecam_live)
        return 0;                        /* idempotent: the vblk
                                          * fallback may walk first */
    if (plat->pcie_ecam_base == 0 || plat->pcie_mmio_size == 0) {
        sbi_puts("[pci] no pci-host-ecam-generic in the tree\n");
        return -1;
    }
    ecam.va        = (volatile uint8_t *)p2v_rv(plat->pcie_ecam_base);
    ecam.bus_limit = (uint32_t)(plat->pcie_ecam_size >> 20);
    ecam.ops       = vmmio_rv_ops();
    ecam.mmio_cpu  = plat->pcie_mmio_cpu;
    ecam.mmio_pci  = plat->pcie_mmio_pci;
    ecam.mmio_size = plat->pcie_mmio_size;
    ecam.mmio_cursor = 0;
    if (pci_ecam_walk(&ecam) < 0)
        return -1;
    ecam_live = 1;
    return 0;
}

struct pci_ecam *pci_rv_ecam(void)
{
    return ecam_live ? &ecam : 0;
}

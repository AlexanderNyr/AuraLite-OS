/* kernel/arch/riscv64/pci_rv.h -- the rv64 tenant's ECAM glue
 * (RESIDUE_PLAN R7). */

#ifndef AURALITE_ARCH_RISCV64_PCI_RV_H
#define AURALITE_ARCH_RISCV64_PCI_RV_H

#include <stdint.h>

#include "kernel/dt/fdt.h"
#include "kernel/drivers/pci_ecam.h"

/* Map the DTB's ECAM window, walk bus 0, print the receipt.
 * Returns 0, or -1 when the tree carries no ECAM node. */
int pci_rv_init(const fdt_platform_t *plat);

/* The live handle (0 until pci_rv_init succeeded). */
struct pci_ecam *pci_rv_ecam(void);

/* Device-attribute view of a freshly placed BAR: rv64's HHDM covers
 * the low 4 GiB and Sv39-without-Svpbmt leaves memory types to the
 * PMAs, so this is the HHDM pointer (the honest mmio_is_device hook
 * already says so). */
volatile uint8_t *pci_rv_map_mmio(uint64_t pa, uint32_t len);

#endif /* AURALITE_ARCH_RISCV64_PCI_RV_H */

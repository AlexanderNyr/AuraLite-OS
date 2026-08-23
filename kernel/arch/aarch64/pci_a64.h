/* kernel/arch/aarch64/pci_a64.h -- the aarch64 tenant's ECAM glue
 * (RESIDUE_PLAN R7). */

#ifndef AURALITE_ARCH_AARCH64_PCI_A64_H
#define AURALITE_ARCH_AARCH64_PCI_A64_H

#include <stdint.h>

#include "kernel/dt/fdt.h"
#include "kernel/drivers/pci_ecam.h"

/* Map the DTB's ECAM window Device-nGnRE, walk bus 0, print the
 * receipt.  Returns 0, or -1 when the tree carries no ECAM node. */
int pci_a64_init(const fdt_platform_t *plat);

/* The live handle (0 until pci_a64_init succeeded). */
struct pci_ecam *pci_a64_ecam(void);

/* Device-attribute view of a freshly placed BAR: installs 4 KiB
 * Device-nGnRE pages in the final tables (the BAR window is inside
 * the HHDM hole below RAM -- nothing to displace) and returns the
 * HHDM-formula VA. */
volatile uint8_t *pci_a64_map_mmio(uint64_t pa, uint32_t len);

#endif /* AURALITE_ARCH_AARCH64_PCI_A64_H */

/* kernel/drivers/pci_ecam.h -- the generic-ECAM PCI config walker
 * (RESIDUE_PLAN R7, ledger RES-20).
 *
 * Both virt boards carry a pci-host-ecam-generic node (ARM64_PLAN D7
 * measured `pcie@10000000` and deferred it; the riscv board's twin is
 * `pci@30000000`).  ECAM is memory-mapped config space: one 4 KiB
 * page per function, bus<<20 | dev<<15 | fn<<12.  No 0xCF8 dance --
 * that port pair is x86's (drivers/pci/pci.c keeps it); this file is
 * the MMIO-native flavour the two DTB tenants share.
 *
 * PORTABLE FILE RULES apply (the virtio_mmio.c promotion's rules):
 * no inline asm, no bare width casts.  Console and attribute checks
 * arrive through the SAME vmmio_arch_ops seam the virtio transport
 * uses -- one table per tenant, every shared driver behind it.
 *
 * BAR assignment lives here too, because -kernel boots have no
 * firmware: nobody has written a single BAR before us (measured: all
 * zeros on both boards).  The allocator is a bump cursor over the
 * DTB's 32-bit non-prefetchable window -- sizes are powers of two,
 * the cursor aligns to each, and refusal (0) is the answer when the
 * window runs out.
 */

#ifndef AURALITE_DRIVERS_PCI_ECAM_H
#define AURALITE_DRIVERS_PCI_ECAM_H

#include <stdint.h>

#include "kernel/drivers/virtio_mmio.h"   /* struct vmmio_arch_ops */

/* Config-space offsets this walker touches (PCI 3.0, type 0). */
#define PCI_CFG_VENDOR    0x00
#define PCI_CFG_DEVICE    0x02
#define PCI_CFG_COMMAND   0x04
#define PCI_CFG_STATUS    0x06
#define PCI_CFG_CLASSREV  0x08
#define PCI_CFG_HDRTYPE   0x0E
#define PCI_CFG_BAR0      0x10
#define PCI_CFG_CAP_PTR   0x34

#define PCI_CMD_MEM       0x0002u
#define PCI_CMD_MASTER    0x0004u
#define PCI_STATUS_CAPS   0x0010u

struct pci_ecam {
    volatile uint8_t *va;        /* ECAM window, mapped by the tenant */
    uint32_t bus_limit;          /* buses the mapped window covers    */
    const struct vmmio_arch_ops *ops;   /* puts + mmio_is_device      */
    /* The BAR window (DTB ranges, 32-bit non-prefetchable entry). */
    uint64_t mmio_cpu;           /* cpu-side base (tenant maps this)  */
    uint64_t mmio_pci;           /* pci-side base (goes into BARs)    */
    uint64_t mmio_size;
    uint64_t mmio_cursor;        /* bump allocator, offset from base  */
};

/* bdf = bus<<8 | dev<<3 | fn. */
uint32_t pci_ecam_r32(struct pci_ecam *e, uint32_t bdf, uint32_t off);
uint16_t pci_ecam_r16(struct pci_ecam *e, uint32_t bdf, uint32_t off);
uint8_t  pci_ecam_r8 (struct pci_ecam *e, uint32_t bdf, uint32_t off);
void pci_ecam_w32(struct pci_ecam *e, uint32_t bdf, uint32_t off, uint32_t v);
void pci_ecam_w16(struct pci_ecam *e, uint32_t bdf, uint32_t off, uint16_t v);

/* Init + walk bus 0 (everything on both virt boards sits there; a
 * bridge would be NAMED, not followed -- bring-up scope).  Prints one
 * line per function and the receipt:
 *   [pci] ECAM: N function(s)
 * Returns N.  The attribute gate runs FIRST: an ECAM window behind a
 * Normal mapping is refused before the first config read (Fact 5.2's
 * bug class, same refusal the virtio transport performs). */
int pci_ecam_walk(struct pci_ecam *e);

/* Find the first function with this vendor/device on bus 0.
 * Returns bdf, or -1. */
int pci_ecam_find(struct pci_ecam *e, uint16_t vendor, uint16_t device_lo,
                  uint16_t device_hi);

/* Size BAR bar_idx (0..5), place it in the mem32 window, write the
 * base back (high dword zeroed for a 64-bit BAR).  Returns the
 * CPU-side physical address, or 0 on refusal (IO BAR, window full).
 * *out_size carries the decoded size. */
uint64_t pci_ecam_place_bar(struct pci_ecam *e, uint32_t bdf,
                            uint32_t bar_idx, uint32_t *out_size);

/* COMMAND |= memory decode + bus master. */
void pci_ecam_enable(struct pci_ecam *e, uint32_t bdf);

#endif /* AURALITE_DRIVERS_PCI_ECAM_H */

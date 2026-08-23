/* kernel/drivers/virtio_pci.h -- virtio over PCI, the SECOND transport
 * behind the same virtio core (RESIDUE_PLAN R7, ledger RES-21).
 *
 * Same vring structures (drivers/virtio/virtio_common.h), same
 * three-descriptor blk request the mmio path posts, same
 * vmmio_arch_ops seam for frames/console/clock/attributes -- only the
 * doorbells differ.  This is the MODERN (virtio 1.0) interface: the
 * device's registers live behind BARs located by vendor capabilities
 * in config space, and VIRTIO_F_VERSION_1 (feature bit 32) MUST be
 * negotiated -- a driver that acks nothing is refused FEATURES_OK by
 * a modern device, which the mmio transport's accept-none habit only
 * got away with because legacy mmio has no FEATURES_OK gate.
 * Measured, not assumed: QEMU's virtio-blk-pci is transitional
 * (device id 0x1001, revision 0) and takes the modern path once
 * VERSION_1 is offered back.
 *
 * PORTABLE FILE RULES apply: no inline asm, no bare width casts.
 */

#ifndef AURALITE_DRIVERS_VIRTIO_PCI_H
#define AURALITE_DRIVERS_VIRTIO_PCI_H

#include <stdint.h>

#include "drivers/virtio/virtio_common.h"
#include "kernel/drivers/pci_ecam.h"

/* Vendor-specific capability layout (virtio 1.0 s4.1.4). */
#define VPCI_CAP_COMMON  1
#define VPCI_CAP_NOTIFY  2
#define VPCI_CAP_ISR     3
#define VPCI_CAP_DEVICE  4

/* common_cfg offsets (virtio 1.0 s4.1.4.3). */
#define VPC_DFSELECT     0x00
#define VPC_DFEATURE     0x04
#define VPC_GFSELECT     0x08
#define VPC_GFEATURE     0x0C
#define VPC_NUMQ         0x12
#define VPC_STATUS       0x14
#define VPC_QSELECT      0x16
#define VPC_QSIZE        0x18
#define VPC_QENABLE      0x1C
#define VPC_QNOTIFY_OFF  0x1E
#define VPC_QDESC_LO     0x20
#define VPC_QDESC_HI     0x24
#define VPC_QAVAIL_LO    0x28
#define VPC_QAVAIL_HI    0x2C
#define VPC_QUSED_LO     0x30
#define VPC_QUSED_HI     0x34

struct vpci_dev {
    const struct vmmio_arch_ops *ops;
    volatile uint8_t *common;
    volatile uint8_t *notify_base;
    uint32_t          notify_mult;
    volatile uint8_t *isr;
    volatile uint8_t *devcfg;
    volatile uint16_t *notify_q0;    /* queue 0's doorbell, resolved */
    uint16_t            qsize;
    struct vring_desc  *desc;
    struct vring_avail *avail;
    struct vring_used  *used;
    uint16_t            last_used_idx;
    /* blk request page: header + 512 data + status. */
    uint64_t dma_phys;
    uint8_t *dma;
    uint64_t capacity;               /* blk: sectors, from device cfg */
};

/* Probe bus 0 for virtio-blk (vendor 0x1af4, transitional 0x1001 or
 * modern 0x1042), place its BAR, run the modern status dance with
 * VERSION_1, bring up queue 0, read the capacity.  map_mmio is the
 * tenant's device-attribute mapping for the freshly placed BAR
 * (aarch64 installs Device-nGnRE pages; rv64 returns its HHDM view).
 * Returns 0 into a ready *d, or -1 (silently when simply absent). */
int vpci_blk_probe(struct vpci_dev *d, const struct vmmio_arch_ops *ops,
                   struct pci_ecam *e,
                   volatile uint8_t *(*map_mmio)(uint64_t pa, uint32_t len));

/* One 512-byte sector, same three-chain request as the mmio path.
 * is_write: 0 read, 1 write.  Returns 0/-1. */
int vpci_blk_rw(struct vpci_dev *d, int is_write, uint64_t lba,
                uint8_t *buf512);

#endif /* AURALITE_DRIVERS_VIRTIO_PCI_H */

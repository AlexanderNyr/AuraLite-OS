/* kernel/arch/aarch64/vblk_a64.h -- virtio-blk over mmio
 * (ARM64_PLAN A7; vblk_rv.h's shape at the fourth tenant). */

#ifndef AURALITE_ARCH_AARCH64_VBLK_A64_H
#define AURALITE_ARCH_AARCH64_VBLK_A64_H

#include <stdint.h>

#include "kernel/dt/fdt.h"

/* Probe the DTB windows for a blk device.  0 = found and ready. */
int vblk_a64_init(const fdt_platform_t *plat);

int vblk_a64_available(void);
uint64_t vblk_a64_sector_count(void);

/* R7: which transport answered the probe -- "virtio-mmio" or
 * "virtio-pci" (fsglue's blkdev line prints the truth). */
const char *vblk_a64_transport(void);

int vblk_a64_read(uint64_t lba, uint8_t *buf512);
int vblk_a64_write(uint64_t lba, const uint8_t *buf512);

/* The ata32-shaped gate: known-bytes read of the test disk's sector 0
 * + write/readback/restore on the last sector.  0 = pass. */
int vblk_a64_selftest(void);

#endif /* AURALITE_ARCH_AARCH64_VBLK_A64_H */

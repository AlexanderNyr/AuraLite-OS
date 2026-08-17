/* kernel/arch/riscv64/vblk_rv.h -- virtio-blk over mmio (RISCV_PLAN V7). */

#ifndef AURALITE_ARCH_RISCV64_VBLK_RV_H
#define AURALITE_ARCH_RISCV64_VBLK_RV_H

#include <stdint.h>

#include "kernel/dt/fdt.h"

/* Probe the DTB windows for a blk device.  0 = found and ready. */
int vblk_rv_init(const fdt_platform_t *plat);

int vblk_rv_available(void);
uint64_t vblk_rv_sector_count(void);

int vblk_rv_read(uint64_t lba, uint8_t *buf512);
int vblk_rv_write(uint64_t lba, const uint8_t *buf512);

/* The ata32-shaped gate: known-bytes read of our own image's sector 0
 * + write/readback/restore on the last sector.  0 = pass. */
int vblk_rv_selftest(void);

#endif /* AURALITE_ARCH_RISCV64_VBLK_RV_H */

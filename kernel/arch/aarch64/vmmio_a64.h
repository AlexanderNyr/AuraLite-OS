/* kernel/arch/aarch64/vmmio_a64.h -- this tenant's side of the
 * promoted virtio-mmio transport (ARM64_PLAN A7). */

#ifndef AURALITE_ARCH_AARCH64_VMMIO_A64_H
#define AURALITE_ARCH_AARCH64_VMMIO_A64_H

#include "kernel/drivers/virtio_mmio.h"

/* The aarch64 vmmio_arch_ops table (pmm/HHDM/PL011/CNTVCT + the
 * Device-nGnRE attach gate).  Reads CNTFRQ_EL0 on first use. */
const struct vmmio_arch_ops *vmmio_a64_ops(void);

#endif /* AURALITE_ARCH_AARCH64_VMMIO_A64_H */

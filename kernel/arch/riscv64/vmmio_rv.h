/* kernel/arch/riscv64/vmmio_rv.h -- this tenant's side of the
 * promoted virtio-mmio transport (ARM64_PLAN A7). */

#ifndef AURALITE_ARCH_RISCV64_VMMIO_RV_H
#define AURALITE_ARCH_RISCV64_VMMIO_RV_H

#include "kernel/drivers/virtio_mmio.h"

/* The rv64 vmmio_arch_ops table (pmm/HHDM/SBI console/rdtime). */
const struct vmmio_arch_ops *vmmio_rv_ops(void);

#endif /* AURALITE_ARCH_RISCV64_VMMIO_RV_H */

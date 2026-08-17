/* kernel/arch/riscv64/vnet_rv.h -- virtio-net over mmio (RISCV_PLAN V7). */

#ifndef AURALITE_ARCH_RISCV64_VNET_RV_H
#define AURALITE_ARCH_RISCV64_VNET_RV_H

#include "kernel/dt/fdt.h"

/* Probe the DTB windows for a net device.  0 = found and ready. */
int vnet_rv_init(const fdt_platform_t *plat);

/* The I8-shaped gate over the SHARED miniproto: DHCP lease on SLIRP,
 * gateway ARP, payload-verified ICMP echo.  0 = pass. */
int vnet_rv_selftest(void);

#endif /* AURALITE_ARCH_RISCV64_VNET_RV_H */

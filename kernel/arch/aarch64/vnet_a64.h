/* kernel/arch/aarch64/vnet_a64.h -- virtio-net over mmio
 * (ARM64_PLAN A7; vnet_rv.h's shape at the fourth tenant). */

#ifndef AURALITE_ARCH_AARCH64_VNET_A64_H
#define AURALITE_ARCH_AARCH64_VNET_A64_H

#include "kernel/dt/fdt.h"

/* Probe the DTB windows for a net device, set up RX+TX queues.
 * 0 = found and ready. */
int vnet_a64_init(const fdt_platform_t *plat);

/* The V7-shaped gate over the SHARED miniproto: DHCP lease, gateway
 * ARP, payload-verified ICMP echo.  0 = pass. */
int vnet_a64_selftest(void);

#endif /* AURALITE_ARCH_AARCH64_VNET_A64_H */

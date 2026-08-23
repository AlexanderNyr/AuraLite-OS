/* fwcfg_a64.h — QEMU fw-cfg MMIO probe (RESIDUE R11, RES-34). */
#ifndef AURALITE_ARCH_AARCH64_FWCFG_A64_H
#define AURALITE_ARCH_AARCH64_FWCFG_A64_H

#include <stdint.h>

/* base_phys = the DTB's qemu,fw-cfg-mmio reg (0 = absent, no-op).
 * Reads opt/auralite.selftest and overrides the build-default mode. */
void fwcfg_a64_selftest_probe(uint64_t base_phys);

#endif /* AURALITE_ARCH_AARCH64_FWCFG_A64_H */

/* fwcfg32.h — QEMU fw_cfg probe, i386 edition (RESIDUE R11, RES-34). */
#ifndef AURALITE_ARCH_I386_FWCFG32_H
#define AURALITE_ARCH_I386_FWCFG32_H

/* Reads -fw_cfg name=opt/auralite.selftest,string=full|fast|off and
 * overrides the build-default self-test mode.  No-op without fw_cfg. */
void fwcfg32_selftest_probe(void);

#endif /* AURALITE_ARCH_I386_FWCFG32_H */

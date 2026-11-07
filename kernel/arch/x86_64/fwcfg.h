#ifndef AURALITE_ARCH_X86_64_FWCFG_H
#define AURALITE_ARCH_X86_64_FWCFG_H

/*
 * fw_cfg option access (QEMU's `-fw_cfg name=opt/...`).
 *
 * One entry point, because every caller so far has wanted the same thing: a
 * single string a test harness can put on the QEMU command line to ask the
 * guest to take a measurement.  Returns 1 when the option exists (with `out`
 * NUL-terminated, at most out_max-1 characters), 0 when fw_cfg is absent, the
 * key is missing, or the value is implausibly large — all of which must
 * degrade to the build defaults rather than to noise.
 */
int fwcfg_read_opt(const char *wanted, char *out, int out_max);

#endif /* AURALITE_ARCH_X86_64_FWCFG_H */

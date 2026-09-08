#ifndef AURALITE_KERNEL_VERSION_H
#define AURALITE_KERNEL_VERSION_H

/* OTA_PLAN O2 (fixed in O3): the version identity is a BUILD knob, not a
 * source literal.  `make iso AURALITE_VERSION=0.0.2-ota` passes
 * -DAURALITE_VERSION='"..."' to every kernel and userspace compile; this
 * #ifndef keeps non-Makefile compiles on the stock value.
 *
 * This header is deliberately ARCH-FREE (unlike kernel.h, which #errors
 * without ARCH_X86_64): the parity lanes syntax-check every kernel/fs
 * translation unit as rv64/aarch64/i386, and procfs.c reads the version
 * for /proc/version -- pulling the x86-64 gate in through kernel.h broke
 * all three lanes (found by make test-unit on the O3 tree; shipped broken
 * in the O2 patch, fixed here). */

#ifndef AURALITE_VERSION
#define AURALITE_VERSION "0.0.1"
#endif

#endif /* AURALITE_KERNEL_VERSION_H */

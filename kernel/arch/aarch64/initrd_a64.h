/* kernel/arch/aarch64/initrd_a64.h -- read-only USTAR initrd lookup
 * (ARM64_PLAN A5c).
 *
 * The SAME initrd.tar the other two kernels mount: one archive,
 * four tenants -- /bina64 is this kernel's (A5b packed and audited it).
 * Each loader refuses the other's ELF class/machine (belt), the
 * directory layout keeps them apart by name (braces).  Bring-up
 * scope: find-by-name over the HHDM-mapped archive; the full VFS
 * port is A8 work.
 */

#ifndef AURALITE_ARCH_AARCH64_INITRD_A64_H
#define AURALITE_ARCH_AARCH64_INITRD_A64_H

#include <stdint.h>
#include "boot/shared/boot_info.h"

/* 0 on success (archive present and sane). */
int initrd_a64_init(const boot_info_t *bi);

/* Look up a regular file by path ("bina64/init"; leading "./" and "/"
 * in archive entries are normalised away).  Returns 1 and fills
 * data/size on hit, 0 on miss. */
int initrd_a64_find(const char *path, const uint8_t **data, uint64_t *size);

uint32_t initrd_a64_file_count(void);

#endif /* AURALITE_ARCH_AARCH64_INITRD_A64_H */

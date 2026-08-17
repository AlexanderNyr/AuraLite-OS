/* kernel/arch/riscv64/initrd_rv.h -- read-only USTAR initrd lookup
 * (RISCV_PLAN V5).
 *
 * The SAME initrd.tar the other two kernels mount: one archive,
 * three tenants -- /bin for x86_64, /bin32 for i386, /binrv here.
 * Each loader refuses the other's ELF class/machine (belt), the
 * directory layout keeps them apart by name (braces).  Bring-up
 * scope: find-by-name over the HHDM-mapped archive; the full VFS
 * port is V8 work.
 */

#ifndef AURALITE_ARCH_RISCV64_INITRD_RV_H
#define AURALITE_ARCH_RISCV64_INITRD_RV_H

#include <stdint.h>
#include "boot/shared/boot_info.h"

/* 0 on success (archive present and sane). */
int initrd_rv_init(const boot_info_t *bi);

/* Look up a regular file by path ("binrv/init"; leading "./" and "/"
 * in archive entries are normalised away).  Returns 1 and fills
 * data/size on hit, 0 on miss. */
int initrd_rv_find(const char *path, const uint8_t **data, uint64_t *size);

uint32_t initrd_rv_file_count(void);

#endif /* AURALITE_ARCH_RISCV64_INITRD_RV_H */

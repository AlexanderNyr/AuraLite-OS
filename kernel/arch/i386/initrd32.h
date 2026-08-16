/* kernel/arch/i386/initrd32.h -- read-only USTAR initrd lookup
 * (I386_PLAN I5).
 *
 * The SAME initrd.tar the 64-bit kernel mounts: mkinitrd.sh packs one
 * archive for both kernels, and the i386 files live under /bin32 so
 * neither kernel can accidentally exec the other's ELF class (each
 * loader also refuses the wrong class -- belt and braces).  Bring-up
 * scope: find-by-name over the direct-mapped archive; the full VFS
 * port is I6+ work.
 */

#ifndef AURALITE_ARCH_I386_INITRD32_H
#define AURALITE_ARCH_I386_INITRD32_H

#include <stdint.h>
#include "boot/shared/boot_info.h"

/* 0 on success (archive present and sane). */
int initrd32_init(const boot_info_t *bi);

/* Look up a regular file by path ("bin32/init32"; leading "./" and "/"
 * in archive entries are normalised away).  Returns 1 and fills
 * data/size on hit, 0 on miss. */
int initrd32_find(const char *path, const uint8_t **data, uint32_t *size);

uint32_t initrd32_file_count(void);

#endif /* AURALITE_ARCH_I386_INITRD32_H */

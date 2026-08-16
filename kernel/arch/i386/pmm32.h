/* kernel/arch/i386/pmm32.h -- physical frame allocator for the i386
 * kernel (I386_PLAN I3).
 *
 * The algorithms are kernel/lib/bitmap.h -- the SAME header the x86_64
 * PMM and the host unit test compile, which is the point: I3's memory
 * work reuses every line that was already width-clean and writes only
 * the arch glue.  Physical addresses are uint64_t (paddr_t discipline,
 * plan D6) even though this allocator caps tracking at the direct-map
 * horizon: E820 entries above 4 GiB exist on real machines and must be
 * *skipped*, not truncated into aliasing low memory.
 */

#ifndef AURALITE_ARCH_I386_PMM32_H
#define AURALITE_ARCH_I386_PMM32_H

#include <stdint.h>
#include "boot/shared/boot_info.h"

#define PAGE_SIZE_32       4096u

/* Direct-map horizon (plan D3): the kernel direct-maps [0, 896 MiB) at
 * 0xC0000000, so no frame above it is allocatable.  896 MiB is the
 * classic split ceiling; supported RAM is 512 MiB in practice. */
#define PMM32_HORIZON      (896u * 1024u * 1024u)

void     pmm32_init(const boot_info_t *bi);
uint32_t pmm32_alloc_frame(void);          /* phys addr, 0 on OOM */
void     pmm32_free_frame(uint32_t phys);
uint32_t pmm32_free_frames(void);
uint32_t pmm32_total_frames(void);
int      pmm32_selftest(void);

#endif /* AURALITE_ARCH_I386_PMM32_H */

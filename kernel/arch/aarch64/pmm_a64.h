/* kernel/arch/aarch64/pmm_a64.h -- bitmap frame allocator (ARM64_PLAN A3). */

#ifndef AURALITE_ARCH_AARCH64_PMM_A64_H
#define AURALITE_ARCH_AARCH64_PMM_A64_H

#include <stdint.h>

#include "boot/shared/boot_info.h"

/* Frames above this physical horizon are skipped with a log line,
 * never truncated (the D6 discipline, inherited through V3).  4 GiB
 * covers the virt machine's RAM window; boards beyond it grow the
 * constant. */
#define PMM_A64_HORIZON (4ULL * 1024 * 1024 * 1024)

void     pmm_a64_init(const boot_info_t *bi);
uint64_t pmm_a64_alloc_frame(void);          /* phys addr, 0 = OOM */
void     pmm_a64_free_frame(uint64_t phys);
uint64_t pmm_a64_free_frames(void);
int      pmm_a64_selftest(void);             /* the [pmm] gate */

#endif /* AURALITE_ARCH_AARCH64_PMM_A64_H */

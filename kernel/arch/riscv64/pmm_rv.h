/* kernel/arch/riscv64/pmm_rv.h -- bitmap frame allocator (RISCV_PLAN V3). */

#ifndef AURALITE_ARCH_RISCV64_PMM_RV_H
#define AURALITE_ARCH_RISCV64_PMM_RV_H

#include <stdint.h>

#include "boot/shared/boot_info.h"

/* Frames above this physical horizon are skipped with a log line,
 * never truncated (the D6 discipline, inherited).  4 GiB covers the
 * virt machine up to -m 3968M; boards beyond it grow the constant. */
#define PMM_RV_HORIZON (4ULL * 1024 * 1024 * 1024)

void     pmm_rv_init(const boot_info_t *bi);
uint64_t pmm_rv_alloc_frame(void);          /* phys addr, 0 = OOM */
void     pmm_rv_free_frame(uint64_t phys);
uint64_t pmm_rv_free_frames(void);
int      pmm_rv_selftest(void);             /* the [pmm] gate */

#endif /* AURALITE_ARCH_RISCV64_PMM_RV_H */

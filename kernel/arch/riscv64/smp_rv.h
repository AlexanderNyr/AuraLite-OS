/* smp_rv.h — PARITY P5: SBI HSM secondary bring-up (receipts, not
 * scheduling — D5). */
#ifndef AURALITE_ARCH_RISCV64_SMP_RV_H
#define AURALITE_ARCH_RISCV64_SMP_RV_H

#include <stdint.h>

#include "boot/shared/boot_info.h"

/* Start every stopped DTB hart, count the report-ins, run one IPI
 * round-trip.  Single-hart runs print an honest nothing-to-do. */
void smp_rv_bringup(uint64_t boot_hartid, const boot_info_t *bi);

/* The started hart's C half (called from boot.S's secondary path). */
void secondary_main_rv(uint64_t hartid);

/* R5: run binrv/init at U-mode on ONE parked secondary (strictly
 * serialized; the boot hart waits).  Returns the exit code, or
 * -1000 (no secondaries) / -1001 (timeout).  *out_hart names the
 * hart that ran it. */
int smp_rv_run_init_on_secondary(int *out_hart);

#endif /* AURALITE_ARCH_RISCV64_SMP_RV_H */

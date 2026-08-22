/* smp_a64.h — PARITY P6: PSCI CPU_ON secondary bring-up (receipts,
 * not scheduling — D5).  The x16 note lives in smp_a64.c: code max
 * is 16 like rv64, GICv2 runs cap at 8 (architectural), GICv3 is
 * the named residue. */
#ifndef AURALITE_ARCH_AARCH64_SMP_A64_H
#define AURALITE_ARCH_AARCH64_SMP_A64_H

#include <stdint.h>

#include "boot/shared/boot_info.h"

/* Start every powered-off DTB core, count report-ins, one SGI IPI
 * round-trip.  Bases are HHDM VAs of the GICv2 frames. */
void smp_a64_bringup(const boot_info_t *bi,
                     uint64_t gicd_va, uint64_t gicc_va);

/* The started core's C half (called from boot.S). */
void secondary_main_a64(void);

/* R5: run bina64/init at EL0 on ONE parked secondary (strictly
 * serialized).  Exit code, or -1000 (none) / -1001 (timeout). */
int smp_a64_run_init_on_secondary(int *out_core);

#endif /* AURALITE_ARCH_AARCH64_SMP_A64_H */

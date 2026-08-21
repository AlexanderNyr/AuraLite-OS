/* kernel/arch/riscv64/membench_rv.h -- boot-time string-ops bench
 * (HW_PLAN H0). */

#ifndef AURALITE_ARCH_RISCV64_MEMBENCH_RV_H
#define AURALITE_ARCH_RISCV64_MEMBENCH_RV_H

#include <stdint.h>

/* Bench the linked memcpy/memset/memmove, print MB/s at timebase_hz
 * resolution.  Verified passes; prints [bench] FAIL on miscopy. */
void membench_rv_run(uint64_t timebase_hz);

#endif /* AURALITE_ARCH_RISCV64_MEMBENCH_RV_H */

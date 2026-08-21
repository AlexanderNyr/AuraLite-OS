/* kernel/arch/aarch64/membench_a64.h -- boot-time string-ops bench
 * (HW_PLAN H0). */

#ifndef AURALITE_ARCH_AARCH64_MEMBENCH_A64_H
#define AURALITE_ARCH_AARCH64_MEMBENCH_A64_H

#include <stdint.h>

/* Bench the linked memcpy/memset/memmove, print MB/s at cntfrq_hz
 * resolution.  Verified passes; prints [bench] FAIL on miscopy. */
void membench_a64_run(uint64_t cntfrq_hz);

#endif /* AURALITE_ARCH_AARCH64_MEMBENCH_A64_H */

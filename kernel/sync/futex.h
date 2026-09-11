#ifndef AURALITE_KERNEL_SYNC_FUTEX_H
#define AURALITE_KERNEL_SYNC_FUTEX_H

#include <stdint.h>

/* Linux futex(2) operation numbers (asm/futex.h).  The COMMAND is the low
 * bits of `op`; the FLAGS are or-ed on top:
 *   FUTEX_PRIVATE_FLAG   128  — private per-process futex (glibc always uses
 *                              this; it changes only the hash domain on Linux,
 *                              which a single-kernel-image futex table does
 *                              not care about)
 *   FUTEX_CLOCK_REALTIME 256  — a WAIT timeout is against CLOCK_REALTIME
 *                              (AuraLite honours timeouts as indefinite; the
 *                              ladder apps use only untimed internal locks)
 */
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_WAIT_BITSET   9
#define FUTEX_WAKE_BITSET   10
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_BITSET_MATCH_ANY 0xFFFFFFFFu

int futex_wait(uint32_t *uaddr, uint32_t val);
int futex_wake(uint32_t *uaddr, int n);
int futex_wait_bitset(uint32_t *uaddr, uint32_t val, uint32_t bitset);
int futex_wake_bitset(uint32_t *uaddr, int n, uint32_t bitset);
int futex_requeue(uint32_t *uaddr, int nr_wake, uint32_t *uaddr2,
                  uint32_t cmpval);

#endif /* AURALITE_KERNEL_SYNC_FUTEX_H */

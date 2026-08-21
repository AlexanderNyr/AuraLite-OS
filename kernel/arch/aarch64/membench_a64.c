/* kernel/arch/aarch64/membench_a64.c -- boot-time string-ops bench
 * (HW_PLAN H0; membench_rv.c's shape at the fourth tenant -- see
 * that file for why buffers are 64 KiB and why every pass verifies).
 *
 * This tenant already links kernel/lib/string.c (A5a [AMEND-2]); the
 * bench measures exactly those linked bodies on the CNTVCT clock.
 */

#include <stdint.h>

#include "kernel/arch/aarch64/membench_a64.h"
#include "kernel/arch/aarch64/pl011.h"
#include "kernel/arch/aarch64/trap_a64.h"
#include "kernel/lib/string.h"

#define BUF_SZ  65536u
#define PASSES  16u             /* 16 x 64 KiB = the 1 MiB-eq rows */

static uint8_t src_buf[BUF_SZ];
static uint8_t dst_buf[BUF_SZ];

static void report(const char *name, uint64_t bytes, uint64_t ticks,
                   uint64_t hz)
{
    uint64_t mbps = (ticks == 0) ? 0 : (bytes * hz) / ticks / 1000000u;
    pl011_puts("[bench] ");
    pl011_puts(name);
    pl011_puts(": ");
    pl011_putdec64(mbps);
    pl011_puts(" MB/s\n");
}

void membench_a64_run(uint64_t cntfrq_hz)
{
    uint64_t t0, t1;
    uint32_t i;

    for (i = 0; i < BUF_SZ; i++)
        src_buf[i] = (uint8_t)(i * 131u + 7u);

    /* memcpy, one 64 KiB pass. */
    t0 = a64_cntvct();
    memcpy(dst_buf, src_buf, BUF_SZ);
    t1 = a64_cntvct();
    if (memcmp(dst_buf, src_buf, BUF_SZ) != 0) {
        pl011_puts("[bench] FAIL: memcpy verify\n");
        return;
    }
    report("memcpy 64KiB", BUF_SZ, t1 - t0, cntfrq_hz);

    /* memcpy, 1 MiB-eq (16 passes). */
    t0 = a64_cntvct();
    for (i = 0; i < PASSES; i++)
        memcpy(dst_buf, src_buf, BUF_SZ);
    t1 = a64_cntvct();
    report("memcpy 1MiB-eq", (uint64_t)BUF_SZ * PASSES, t1 - t0,
           cntfrq_hz);

    /* memset, 1 MiB-eq. */
    t0 = a64_cntvct();
    for (i = 0; i < PASSES; i++)
        memset(dst_buf, 0xA5, BUF_SZ);
    t1 = a64_cntvct();
    if (dst_buf[0] != 0xA5 || dst_buf[BUF_SZ - 1] != 0xA5) {
        pl011_puts("[bench] FAIL: memset verify\n");
        return;
    }
    report("memset 1MiB-eq", (uint64_t)BUF_SZ * PASSES, t1 - t0,
           cntfrq_hz);

    /* memmove, overlapping (dst > src => the backward path). */
    memcpy(dst_buf, src_buf, BUF_SZ);
    t0 = a64_cntvct();
    memmove(dst_buf + 8, dst_buf, BUF_SZ - 8);
    t1 = a64_cntvct();
    if (memcmp(dst_buf + 8, src_buf, BUF_SZ - 8) != 0) {
        pl011_puts("[bench] FAIL: memmove verify\n");
        return;
    }
    report("memmove-overlap 64KiB", BUF_SZ - 8, t1 - t0, cntfrq_hz);

    pl011_puts("[bench] done (linked string ops: kernel/lib/string.c)\n");
}

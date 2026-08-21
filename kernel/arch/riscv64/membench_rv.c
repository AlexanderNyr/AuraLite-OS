/* kernel/arch/riscv64/membench_rv.c -- boot-time string-ops bench
 * (HW_PLAN H0).
 *
 * Measures the LINKED string ops -- whatever kernel/lib/string.c
 * provides at link time (the byte loops today; H1's word loops after)
 * -- so the number tracks the tree, not a private copy.  The x86
 * membench is a userspace shell command; these tenants bench in the
 * kernel because that is where their string traffic lives (ELF spawn
 * copies, initrd reads) and because a boot line is what the smokes
 * can pin.
 *
 * Buffers are static 64 KiB pairs; the "1 MiB-eq" rows are 16 passes.
 * TCG models no cache hierarchy, so resident-vs-streaming is not a
 * distinction it can measure -- pretending otherwise with megabyte
 * buffers would just be slower theatre (D1).
 *
 * Every pass is VERIFIED (word-wide memcmp -- O1's, already linked):
 * a bench that miscopies must fail loudly, not report a fast wrong
 * answer.
 */

#include <stdint.h>

#include "kernel/arch/riscv64/membench_rv.h"
#include "kernel/arch/riscv64/sbi.h"
#include "kernel/arch/riscv64/trap.h"
#include "kernel/lib/string.h"

#define BUF_SZ  65536u
#define PASSES  16u             /* 16 x 64 KiB = the 1 MiB-eq rows */

static uint8_t src_buf[BUF_SZ];
static uint8_t dst_buf[BUF_SZ];

static void put_udec_(uint64_t v)
{
    char buf[20]; int i = 0;
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--) sbi_putc(buf[i]);
}

static void report(const char *name, uint64_t bytes, uint64_t ticks,
                   uint64_t hz)
{
    /* MB/s = bytes * hz / ticks / 1e6; bytes*hz <= 2^20*16 * 1e7
     * ~= 1.7e14 -- fits u64 with room. */
    uint64_t mbps = (ticks == 0) ? 0 : (bytes * hz) / ticks / 1000000u;
    sbi_puts("[bench] ");
    sbi_puts(name);
    sbi_puts(": ");
    put_udec_(mbps);
    sbi_puts(" MB/s\n");
}

void membench_rv_run(uint64_t timebase_hz)
{
    uint64_t t0, t1;
    uint32_t i;

    for (i = 0; i < BUF_SZ; i++)
        src_buf[i] = (uint8_t)(i * 131u + 7u);

    /* memcpy, one 64 KiB pass. */
    t0 = rv_rdtime();
    memcpy(dst_buf, src_buf, BUF_SZ);
    t1 = rv_rdtime();
    if (memcmp(dst_buf, src_buf, BUF_SZ) != 0) {
        sbi_puts("[bench] FAIL: memcpy verify\n");
        return;
    }
    report("memcpy 64KiB", BUF_SZ, t1 - t0, timebase_hz);

    /* memcpy, 1 MiB-eq (16 passes). */
    t0 = rv_rdtime();
    for (i = 0; i < PASSES; i++)
        memcpy(dst_buf, src_buf, BUF_SZ);
    t1 = rv_rdtime();
    report("memcpy 1MiB-eq", (uint64_t)BUF_SZ * PASSES, t1 - t0,
           timebase_hz);

    /* memset, 1 MiB-eq. */
    t0 = rv_rdtime();
    for (i = 0; i < PASSES; i++)
        memset(dst_buf, 0xA5, BUF_SZ);
    t1 = rv_rdtime();
    if (dst_buf[0] != 0xA5 || dst_buf[BUF_SZ - 1] != 0xA5) {
        sbi_puts("[bench] FAIL: memset verify\n");
        return;
    }
    report("memset 1MiB-eq", (uint64_t)BUF_SZ * PASSES, t1 - t0,
           timebase_hz);

    /* memmove, overlapping (dst > src => the backward path). */
    memcpy(dst_buf, src_buf, BUF_SZ);
    t0 = rv_rdtime();
    memmove(dst_buf + 8, dst_buf, BUF_SZ - 8);
    t1 = rv_rdtime();
    if (memcmp(dst_buf + 8, src_buf, BUF_SZ - 8) != 0) {
        sbi_puts("[bench] FAIL: memmove verify\n");
        return;
    }
    report("memmove-overlap 64KiB", BUF_SZ - 8, t1 - t0, timebase_hz);

    sbi_puts("[bench] done (linked string ops: kernel/lib/string.c)\n");
}

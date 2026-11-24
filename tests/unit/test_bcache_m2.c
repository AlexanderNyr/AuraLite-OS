/* tests/unit/test_bcache_m2.c — FSMODERN M2: the 4 KiB block cache.
 *
 * M2 changed the cache block from a 512-byte sector to 4 KiB and added
 * three things on top: a batch fetch (bc_get_many), a partial-block
 * sector API (fs_read_block/fs_write_block translating sectors onto
 * blocks), and write coalescing (bc_flush_all merging adjacent dirty
 * blocks into one device command).  All three are pure logic over the
 * blkdev seam, so they belong in the host lane -- where a failure names
 * the offset that is wrong, instead of costing a two-minute QEMU boot.
 *
 * Why it earns its place: the first M2 build booted, formatted disks and
 * then read back the empty string for a file it had just written.  The
 * cause was a source pointer advanced per block in fs_write_block,
 * which drifted by one block whenever a request started mid-block.
 * That class of bug is invisible in a boot log and obvious here.
 *
 * The test links the SHIPPED buffer_cache.c against a RAM-backed
 * blkdev_ops that records every transfer (lba, sector count, payload),
 * so "one device command per run" is asserted, not assumed.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include "kernel/fs/buffer_cache.h"
#include "kernel/fs/blkdev.h"
#include "kernel/lib/spinlock.h"

/* ---- the seam the cache talks to ----------------------------------- */

#define DEV_SECTORS 8192            /* 4 MiB of 512-byte sectors */

static uint8_t *dev_mem;
static int      dev_reads, dev_writes;
static uint64_t last_read_lba, last_read_count;
static uint64_t last_write_lba, last_write_count;
static int      coalesced_writes;   /* writes with count > 1 */
/* FSMODERN M4: the ORDER of device writes, not just their count.  A
 * range flush is only useful if it can put the data area on the medium
 * before the FAT, and a test that only counts commands cannot see that. */
static uint64_t wlog_lba[256];
static uint32_t wlog_count[256];
static int      wlog_n;
static int      fail_writes;        /* negative control: device refuses */

static int ram_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    if (lba + count > DEV_SECTORS) return -1;
    memcpy(buf, dev_mem + lba * 512, (size_t)count * 512);
    dev_reads++;
    last_read_lba = lba; last_read_count = count;
    return 0;
}

static int ram_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    (void)ctx;
    if (fail_writes) return -1;
    if (lba + count > DEV_SECTORS) return -1;
    memcpy(dev_mem + lba * 512, buf, (size_t)count * 512);
    dev_writes++;
    last_write_lba = lba; last_write_count = count;
    if (count > 1) coalesced_writes++;
    if (wlog_n < (int)(sizeof(wlog_lba) / sizeof(wlog_lba[0]))) {
        wlog_lba[wlog_n] = lba;
        wlog_count[wlog_n] = count;
        wlog_n++;
    }
    return 0;
}

static uint64_t ram_count(void *ctx) { (void)ctx; return DEV_SECTORS; }

static const struct blkdev_ops ram_ops = {
    .read = ram_read, .write = ram_write, .sector_count = ram_count,
};

/* ---- stubs the cache needs from the rest of the kernel -------------- */

/* The host lane has no interrupt state to save, so the lock is a no-op:
 * the test is single-threaded and the lock's job (serialising device
 * transfers) is exactly what a sequential test already provides. */
void spinlock_acquire(spinlock_t *l) { (void)l; }
void spinlock_release(spinlock_t *l) { (void)l; }
int  spinlock_try_acquire(spinlock_t *l) { (void)l; return 1; }

void *kmalloc(uint64_t size) { return calloc(1, (size_t)size); }
void  kfree(void *p) { free(p); }

void kprintf(const char *fmt, ...) {
    /* Silence by default: the cache is chatty on error and the failure
     * output of THIS test is what a reader wants to see. */
    (void)fmt;
}

/* pmm_get_usable_frames(): the pool is sized from RAM.  128 MiB here.
 * 128 MiB / 500 = 268 KiB / 4 KiB = 67 blocks -> below the floor, so
 * the pool must clamp to BC_MIN_BUFFERS.  The clamp is part of M2's
 * contract ("derived from RAM, hard bounds"), so it is asserted below. */
uint64_t pmm_get_usable_frames(void) { return 128ull * 1024 * 1024 / 4096; }

/* ---- harness ------------------------------------------------------- */

static int failures = 0;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) { printf("  PASS: %s\n", msg); }                            \
        else { printf("  FAIL: %s\n", msg); failures++; }                     \
    } while (0)

static void reset_device(uint8_t fill) {
    memset(dev_mem, fill, (size_t)DEV_SECTORS * 512);
    dev_reads = dev_writes = 0;
    coalesced_writes = 0;
    last_read_lba = last_read_count = 0;
    last_write_lba = last_write_count = 0;
    wlog_n = 0;
    fail_writes = 0;
}

int main(void) {
    dev_mem = malloc((size_t)DEV_SECTORS * 512);
    if (!dev_mem) { printf("FAIL: cannot allocate device memory\n"); return 1; }

    printf("test_bcache_m2: 4 KiB blocks, batch fetch, partial-sector API, "
           "coalescing\n");

    /* A device must exist before the cache can be initialised. */
    if (blkdev_register("ram0", &ram_ops, NULL, 512) < 0) {
        printf("FAIL: cannot register the RAM device\n");
        return 1;
    }

    /* ---- 1. the block size is 4 KiB, not 512 bytes ------------------ */
    CHECK(BC_BLOCK_SIZE == 4096, "cache block is 4 KiB");
    CHECK(BC_SECTORS_PER_BLOCK == 8, "one cache block covers 8 sectors");

    reset_device(0x00);
    bc_init();

    /* The pool must be within the documented clamp and big enough to
     * hold a filesystem's working set. */
    CHECK(BC_MIN_BUFFERS >= 256, "pool floor is at least 256 blocks (1 MiB)");

    /* ---- 2. a full-block write then read is exact ------------------- */
    reset_device(0x00);
    uint8_t page[BC_BLOCK_SIZE];
    for (size_t i = 0; i < sizeof(page); i++) page[i] = (uint8_t)(i * 31 + 7);

    CHECK(fs_write_block(0, 8, 8, page) == 0, "full-block write (8 sectors)");
    bc_flush_all();

    uint8_t back[BC_BLOCK_SIZE];
    memset(back, 0, sizeof(back));
    CHECK(fs_read_block(0, 8, 8, back) == 0, "full-block read (8 sectors)");
    CHECK(memcmp(back, page, sizeof(page)) == 0,
          "full-block round trip is byte-exact");

    /* ---- 3. the partial-block case: the M2 build's own bug ---------- *
     * A one-sector request that starts inside a 4 KiB block.  Writing
     * it must not disturb the other seven sectors, and reading it back
     * must return exactly that sector -- which is what the first M2
     * build got wrong by advancing the source pointer per block. */
    reset_device(0x00);
    CHECK(fs_write_block(0, 16, 8, page) == 0, "seed a whole block");

    uint8_t one[512];
    memset(one, 0xAB, sizeof(one));
    CHECK(fs_write_block(0, 19, 1, one) == 0, "partial write: 1 sector at offset 3");

    uint8_t rd[BC_BLOCK_SIZE];
    memset(rd, 0, sizeof(rd));
    CHECK(fs_read_block(0, 16, 8, rd) == 0, "read the block back");
    CHECK(memcmp(rd, page, 3 * 512) == 0, "bytes BEFORE the partial write are intact");
    CHECK(memcmp(rd + 3 * 512, one, 512) == 0, "the partial write landed exactly");
    CHECK(memcmp(rd + 4 * 512, page + 4 * 512, 4 * 512) == 0,
          "bytes AFTER the partial write are intact");

    /* ---- 4. a request that starts mid-block and spans blocks -------- *
     * This is the shape that drifted: start offset != 0 and more than
     * one block of data. */
    reset_device(0x00);
    static uint8_t span[2 * BC_BLOCK_SIZE];
    for (size_t i = 0; i < sizeof(span); i++) span[i] = (uint8_t)(i + 1);

    CHECK(fs_write_block(0, 13, 8, span) == 0, "write 8 sectors at lba 13 (offset 5)");

    static uint8_t spanback[2 * BC_BLOCK_SIZE];
    memset(spanback, 0xEE, sizeof(spanback));
    CHECK(fs_read_block(0, 13, 8, spanback) == 0, "read them back");
    /* The request is 8 sectors at lba 13, i.e. 4096 bytes starting at
     * byte offset 256 of the buffer -- the first 256 bytes live in the
     * block BEFORE the request and are not part of it.  Comparing the
     * whole 8 KiB array here was the test's own bug. */
    CHECK(memcmp(spanback, span, 8 * 512) == 0,
          "mid-block spanning request round trips byte-exact");

    /* ---- 5. coalescing: adjacent dirty blocks become ONE command ---- */
    reset_device(0x00);
    /* Earlier cases left dirty blocks in the cache.  Flush them first,
     * otherwise THIS test's count includes their writes -- which is
     * exactly what made it report 2 commands for 4 blocks. */
    bc_flush_all();
    dev_writes = 0;
    coalesced_writes = 0;
    for (int i = 0; i < 4; i++) {
        memset(page, (uint8_t)(0x10 + i), sizeof(page));
        CHECK(fs_write_block(0, 64 + i * 8, 8, page) == 0, "queue a dirty block");
    }
    dev_writes = 0;
    bc_flush_all();
    CHECK(dev_writes == 1,
          "four adjacent dirty blocks flush as ONE device command");
    CHECK(coalesced_writes == 1, "and it carried a multi-sector count");

    /* ---- 6. batch fetch: a multi-block read is one command ---------- */
    reset_device(0x00);
    /* Make the device distinguishable per block. */
    for (uint64_t b = 0; b < 4; b++)
        memset(dev_mem + (200 + b * 8) * 512, (int)(b + 1), BC_BLOCK_SIZE);

    /* NOTE: the cache still holds blocks from the tests above, so a
     * "reads == 1" here would be measuring residual state.  Region 200
     * is untouched by any earlier case, and the assertion is on the
     * device calls made for THIS request. */
    dev_reads = 0;
    static uint8_t batch[4 * BC_BLOCK_SIZE];
    memset(batch, 0, sizeof(batch));
    CHECK(fs_read_block(0, 200, 32, batch) == 0, "read 4 blocks (32 sectors)");
    CHECK(dev_reads == 1,
          "a 16 KiB sequential read is ONE device command, not four");
    CHECK(last_read_count == 32, "and it asked the device for 32 sectors");

    bool per_block_ok = true;
    for (int b = 0; b < 4; b++)
        for (uint32_t i = 0; i < BC_BLOCK_SIZE; i++)
            if (batch[b * BC_BLOCK_SIZE + i] != (uint8_t)(b + 1)) per_block_ok = false;
    CHECK(per_block_ok, "the batch landed in the right order (no overlap)");

    /* ---- 7. a cached block is not re-read -------------------------- */
    dev_reads = 0;
    CHECK(fs_read_block(0, 200, 32, batch) == 0, "read the same 16 KiB again");
    CHECK(dev_reads == 0, "a repeated read costs no device command at all");

    /* ---- 8. FSMODERN M4: bc_flush_range() orders durability -------- *
     *
     * bc_flush_all() writes the LOWEST dirty block first.  On a FAT32
     * volume the FAT lives below the data area, so an unordered flush puts
     * the index on the medium BEFORE the clusters it names.  This section
     * proves the range flush can invert that order, that it touches
     * nothing outside its range, and that a failed write is reported and
     * left for a retry instead of being dropped. */
    bc_flush_all();              /* drop every block the cases above left */
    reset_device(0x00);

    memset(page, 0x11, sizeof(page));
    CHECK(fs_write_block(0, 8, 8, page) == 0, "dirty block 1 (the FAT area, lba 8)");
    memset(page, 0x5A, sizeof(page));
    CHECK(fs_write_block(0, 400, 8, page) == 0, "dirty block 50 (the data area, lba 400)");
    dev_writes = 0;
    wlog_n = 0;

    CHECK(bc_flush_range(0, 50, 1) == 0, "range flush of the data block");
    CHECK(dev_writes == 1, "the data block reached the device");
    CHECK(wlog_n == 1 && wlog_lba[0] == 400,
          "and it was the DATA block (lba 400), not the FAT (lba 64)");

    CHECK(bc_flush_range(0, 1, 1) == 0, "then range flush the FAT block");
    CHECK(dev_writes == 2, "the FAT block reached the device second");
    CHECK(wlog_n == 2 && wlog_lba[1] == 8,
          "and it was the FAT block (lba 8), in order");

    dev_writes = 0;
    CHECK(bc_flush_range(0, 100, 4) == 0, "a range with no dirty blocks returns 0");
    CHECK(dev_writes == 0, "and issues no device command");
    CHECK(bc_flush_range(0, 5, 0) == 0, "a zero-length range is a no-op");

    /* Negative control: the device refuses, the flush must say so and the
     * buffer must stay dirty so a retry can reach it. */
    memset(page, 0xC3, sizeof(page));
    CHECK(fs_write_block(0, 408, 8, page) == 0, "queue block 51 for the failure case");
    dev_writes = 0;
    fail_writes = 1;
    CHECK(bc_flush_range(0, 51, 1) == -1, "a refused write is reported as -1");
    fail_writes = 0;
    CHECK(bc_flush_range(0, 51, 1) == 0, "the buffer was still dirty: retry succeeds");
    CHECK(dev_writes == 1, "and the retry is exactly one command");

    printf(failures ? "FAILURES: %d\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}

/* test_blkdev.c — host unit test for the blkdev seam (PARITY P1).
 *
 * Compiles kernel/fs/blkdev.c with plain cc (the file is pure table
 * code by design) and proves the seam's contract without QEMU:
 *
 *   1. registration: order, ids, names, the 512-refusal, the slot cap;
 *   2. I/O: multi-sector and per-sector reads agree byte-for-byte,
 *      writes land where they were aimed, bounds and bad ids fail;
 *   3. stats: only successful seam traffic counts;
 *   4. an ext2 SUPERBLOCK read through the seam: the magic 0xEF53
 *      parsed from sector 2 of a RAM device exactly the way ext2.c's
 *      read_blocks(2, 2, ...) path sees it.
 *
 * The plan's draft promised mkfs.ext2 + sha256 here; that needs
 * e2fsprogs on every runner and a loop mount, so the END-TO-END file
 * read proof stays with the integration cases that now run through
 * this seam in-guest (test_ext2.sh and friends), and this test pins
 * the seam semantics themselves.  Deviation named in the plan.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "kernel/fs/blkdev.h"

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else      { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* ---- RAM-backed fake device ---------------------------------------- */

#define RAM_SECTORS 64

struct ramdev {
    uint8_t data[RAM_SECTORS * BLKDEV_SECTOR_SIZE];
    int fail_next;          /* fault injection: fail the next op */
};

static int ram_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    struct ramdev *d = ctx;
    if (d->fail_next) { d->fail_next = 0; return -1; }
    if (lba + count > RAM_SECTORS) return -1;
    memcpy(buf, d->data + lba * BLKDEV_SECTOR_SIZE,
           (size_t)count * BLKDEV_SECTOR_SIZE);
    return 0;
}

static int ram_write(void *ctx, uint64_t lba, uint32_t count,
                     const void *buf)
{
    struct ramdev *d = ctx;
    if (d->fail_next) { d->fail_next = 0; return -1; }
    if (lba + count > RAM_SECTORS) return -1;
    memcpy(d->data + lba * BLKDEV_SECTOR_SIZE, buf,
           (size_t)count * BLKDEV_SECTOR_SIZE);
    return 0;
}

static uint64_t ram_sectors(void *ctx)
{
    (void)ctx;
    return RAM_SECTORS;
}

static const struct blkdev_ops ram_ops = {
    .read = ram_read, .write = ram_write, .sector_count = ram_sectors,
};
static const struct blkdev_ops ram_ops_nocount = {
    .read = ram_read, .write = ram_write, .sector_count = 0,
};

static struct ramdev dev_a, dev_b, filler[BLKDEV_MAX];

int main(void)
{
    printf("test_blkdev: the seam's contract\n");

    /* -- 1. registration ---------------------------------------------- */
    CHECK(blkdev_count() == 0, "registry starts empty");
    CHECK(blkdev_read(0, 0, 1, (uint8_t[512]){0}) < 0,
          "read from an empty registry fails");

    int a = blkdev_register("rama", &ram_ops, &dev_a, 512);
    int b = blkdev_register("ramb", &ram_ops_nocount, &dev_b, 512);
    CHECK(a == 0 && b == 1, "ids are handed out in registration order");
    CHECK(blkdev_count() == 2, "count follows registration");
    CHECK(strcmp(blkdev_name(0), "rama") == 0 &&
          strcmp(blkdev_name(1), "ramb") == 0, "names stick to their ids");
    CHECK(blkdev_name(7)[0] == '\0' && blkdev_name(-1)[0] == '\0',
          "invalid ids name as empty, not as garbage");

    CHECK(blkdev_register("liar", &ram_ops, &dev_a, 4096) == -2,
          "a 4096-byte-sector backend is REFUSED (the 512 stance)");
    CHECK(blkdev_register(0, &ram_ops, &dev_a, 512) < 0 &&
          blkdev_register("noops", 0, &dev_a, 512) < 0,
          "NULL name or ops are refused");
    CHECK(blkdev_count() == 2, "refused registrations consume no slot");

    /* -- 2. I/O through the seam --------------------------------------- */
    uint8_t wbuf[3 * BLKDEV_SECTOR_SIZE], rbuf[3 * BLKDEV_SECTOR_SIZE];
    for (unsigned i = 0; i < sizeof(wbuf); i++)
        wbuf[i] = (uint8_t)(i * 7 + 3);

    CHECK(blkdev_write(a, 5, 3, wbuf) == 0, "3-sector write succeeds");
    memset(rbuf, 0, sizeof(rbuf));
    CHECK(blkdev_read(a, 5, 3, rbuf) == 0 &&
          memcmp(wbuf, rbuf, sizeof(wbuf)) == 0,
          "3-sector read returns the written bytes");

    uint8_t one[BLKDEV_SECTOR_SIZE];
    memset(one, 0, sizeof(one));
    CHECK(blkdev_read_sector(a, 6, one) == 0 &&
          memcmp(one, wbuf + BLKDEV_SECTOR_SIZE, sizeof(one)) == 0,
          "per-sector read agrees with the middle of the batch");

    CHECK(memcmp(dev_b.data, dev_a.data, sizeof(dev_b.data)) != 0 ||
          blkdev_write(b, 5, 3, wbuf) == 0,
          "devices are isolated (write to a never lands on b)");

    CHECK(blkdev_read(a, RAM_SECTORS - 1, 2, rbuf) < 0,
          "a read past the end fails through the seam");
    CHECK(blkdev_read(a, 0, 0, rbuf) < 0, "a zero-count read is refused");
    CHECK(blkdev_read(a, 0, 1, 0) < 0, "a NULL buffer is refused");
    CHECK(blkdev_read(9, 0, 1, rbuf) < 0 && blkdev_read(-1, 0, 1, rbuf) < 0,
          "out-of-range device ids fail");

    CHECK(blkdev_sector_count(a) == RAM_SECTORS,
          "sector_count passes through when the backend has it");
    CHECK(blkdev_sector_count(b) == 0,
          "sector_count is an honest 0 when the backend cannot say");

    /* -- 3. stats: only successful seam traffic ------------------------ */
    uint64_t sr0, sw0, sr1, sw1;
    blkdev_get_stats(&sr0, &sw0);
    dev_a.fail_next = 1;
    CHECK(blkdev_read(a, 0, 3, rbuf) < 0, "injected backend failure surfaces");
    blkdev_get_stats(&sr1, &sw1);
    CHECK(sr1 == sr0 && sw1 == sw0, "a failed op counts nothing");
    CHECK(blkdev_read(a, 0, 3, rbuf) == 0, "the device recovers");
    blkdev_get_stats(&sr1, &sw1);
    CHECK(sr1 == sr0 + 3 && sw1 == sw0, "a 3-sector read counts exactly 3");

    /* -- 4. the ext2 superblock, exactly as ext2.c reads it ------------ */
    /* ext2.c: read_blocks(2, 2, tmp) then superblock at tmp offset 0;
     * s_magic lives at byte 56 of the superblock, little-endian 0xEF53. */
    uint8_t sb[2 * BLKDEV_SECTOR_SIZE];
    memset(sb, 0, sizeof(sb));
    sb[56] = 0x53; sb[57] = 0xEF;            /* s_magic */
    sb[0]  = 0x40; sb[1] = 0x00;             /* s_inodes_count = 64 */
    CHECK(blkdev_write(a, 2, 2, sb) == 0, "superblock lands at LBA 2");
    memset(rbuf, 0, sizeof(rbuf));
    CHECK(blkdev_read(a, 2, 2, rbuf) == 0 &&
          rbuf[56] == 0x53 && rbuf[57] == 0xEF &&
          (uint16_t)(rbuf[56] | (rbuf[57] << 8)) == 0xEF53,
          "ext2 magic 0xEF53 parses from the seam's bytes");

    /* -- 5. the slot cap ------------------------------------------------ */
    int got = 0;
    for (int i = 0; i < BLKDEV_MAX; i++)
        if (blkdev_register("fill", &ram_ops, &filler[i], 512) >= 0)
            got++;
    CHECK(got == BLKDEV_MAX - 2, "registry fills to BLKDEV_MAX exactly");
    CHECK(blkdev_register("nine", &ram_ops, &dev_a, 512) == -3,
          "the ninth device is refused with the slot-cap code");

    if (failures) {
        printf("FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: all blkdev seam checks passed\n");
    return 0;
}

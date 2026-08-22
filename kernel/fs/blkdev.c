/* blkdev.c — the block-device registry (PARITY_PLAN.md P1).
 *
 * Deliberately free of kprintf, locks and allocation: pure table
 * code, so the host unit test (tests/unit/test_blkdev.c) compiles
 * this file with plain cc and no kernel stubs.  Registration
 * messages are the registering driver's job; concurrent access is
 * the caller's problem exactly as it was when the callers held raw
 * AHCI port numbers.
 */

#include "kernel/fs/blkdev.h"

struct blkdev_slot {
    const char           *name;
    const struct blkdev_ops *ops;
    void                 *ctx;
    int                   used;
};

static struct blkdev_slot slots[BLKDEV_MAX];
static int nslots;

static uint64_t stat_sectors_read;
static uint64_t stat_sectors_written;

int blkdev_register(const char *name, const struct blkdev_ops *ops,
                    void *ctx, uint32_t sector_size)
{
    if (!name || !ops || !ops->read || !ops->write)
        return -1;
    if (sector_size != BLKDEV_SECTOR_SIZE)
        return -2;      /* the refuse-loudly stance: never lie about 512 */
    if (nslots >= BLKDEV_MAX)
        return -3;
    slots[nslots].name = name;
    slots[nslots].ops  = ops;
    slots[nslots].ctx  = ctx;
    slots[nslots].used = 1;
    return nslots++;
}

int blkdev_count(void)
{
    return nslots;
}

const char *blkdev_name(int dev)
{
    if (dev < 0 || dev >= nslots)
        return "";
    return slots[dev].name;
}

static const struct blkdev_slot *get(int dev)
{
    if (dev < 0 || dev >= nslots || !slots[dev].used)
        return 0;
    return &slots[dev];
}

int blkdev_read(int dev, uint64_t lba, uint32_t count, void *buf)
{
    const struct blkdev_slot *s = get(dev);
    if (!s || !buf || count == 0)
        return -1;
    int rc = s->ops->read(s->ctx, lba, count, buf);
    if (rc == 0)
        stat_sectors_read += count;
    return rc;
}

int blkdev_write(int dev, uint64_t lba, uint32_t count, const void *buf)
{
    const struct blkdev_slot *s = get(dev);
    if (!s || !buf || count == 0)
        return -1;
    int rc = s->ops->write(s->ctx, lba, count, buf);
    if (rc == 0)
        stat_sectors_written += count;
    return rc;
}

int blkdev_read_sector(int dev, uint64_t lba, void *buf512)
{
    return blkdev_read(dev, lba, 1, buf512);
}

int blkdev_write_sector(int dev, uint64_t lba, const void *buf512)
{
    return blkdev_write(dev, lba, 1, buf512);
}

uint64_t blkdev_sector_count(int dev)
{
    const struct blkdev_slot *s = get(dev);
    if (!s || !s->ops->sector_count)
        return 0;
    return s->ops->sector_count(s->ctx);
}

void blkdev_get_stats(uint64_t *out_sectors_read,
                      uint64_t *out_sectors_written)
{
    if (out_sectors_read)
        *out_sectors_read = stat_sectors_read;
    if (out_sectors_written)
        *out_sectors_written = stat_sectors_written;
}

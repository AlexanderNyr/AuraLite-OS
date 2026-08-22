#ifndef AURALITE_KERNEL_FS_BLKDEV_H
#define AURALITE_KERNEL_FS_BLKDEV_H

/*
 * blkdev — the one seam between filesystems and block drivers.
 *
 * PARITY_PLAN.md P1.  Before this file, kernel/fs/ called ahci_*
 * directly from 41 sites: portable filesystem code hard-wired to one
 * x86 SATA driver, which is the single reason no other port could
 * mount anything.  Every filesystem now takes a small integer device
 * id and calls through this table; drivers register themselves at
 * boot (AHCI on x86_64; virtio-mmio vblk on the DTB tenants from P2;
 * ATA PIO on i386 from P7).
 *
 * Deliberately narrow:
 *   - 512-byte sectors only.  All three backends present 512 today
 *     (AHCI_SECTOR_SIZE, vblk's buf512 contract, ata32's 256-word
 *     PIO loop — measured at P0).  A backend that reports anything
 *     else is REFUSED at registration rather than lied about.
 *   - multi-sector ops carry a count.  AHCI does one DMA for a
 *     multi-sector read; a per-sector-only seam would have silently
 *     split ext2's 2-sector superblock read into two DMAs.  Backends
 *     that only do single sectors (vblk today) loop on their side.
 *   - no partition parsing, no caching, no locking here.  The
 *     buffer cache stays where it is; drivers keep their own locks.
 *
 * This header is portable: no driver includes, no arch includes.
 * The DRIVER includes this file, never the other way around.
 */

#include <stdint.h>

#define BLKDEV_SECTOR_SIZE 512u
#define BLKDEV_MAX 8   /* the x86 boot already mounts seven disk-backed
                        * filesystems (kernel.c), plus one spare slot */

struct blkdev_ops {
    /* Read/write `count` sectors starting at `lba`.  Return 0 on
     * success, negative on error.  `count` >= 1. */
    int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf);
    int (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf);
    /* Optional (may be NULL): total sectors on the device. */
    uint64_t (*sector_count)(void *ctx);
};

/* Register a device.  `sector_size` must be BLKDEV_SECTOR_SIZE (the
 * refuse-loudly stance).  Returns the device id (0..BLKDEV_MAX-1) or
 * a negative error. */
int blkdev_register(const char *name, const struct blkdev_ops *ops,
                    void *ctx, uint32_t sector_size);

/* Number of registered devices (ids 0..count-1 are valid). */
int blkdev_count(void);

/* Device name as registered, or "" for an invalid id. */
const char *blkdev_name(int dev);

/* Multi-sector I/O through the seam.  0 on success, negative on
 * error (invalid dev, NULL buf, zero count, or backend failure). */
int blkdev_read(int dev, uint64_t lba, uint32_t count, void *buf);
int blkdev_write(int dev, uint64_t lba, uint32_t count, const void *buf);

/* Single-sector convenience wrappers (the buffer cache's shape). */
int blkdev_read_sector(int dev, uint64_t lba, void *buf512);
int blkdev_write_sector(int dev, uint64_t lba, const void *buf512);

/* Total sectors, or 0 when the backend does not say. */
uint64_t blkdev_sector_count(int dev);

/* RES-04 (ledger): partition-table sniff.  The seam mounts RAW
 * offsets and IGNORES partition tables by design (PARITY §6); this
 * probe exists so that ignoring is LOUD.  Reads sectors 0-1 through
 * the ops (pure logic; the host unit test runs it on a RAM device).
 * Returns 0 = no table, 1 = MBR with at least one non-empty entry,
 * 2 = GPT, negative = read error. */
#define BLKDEV_PART_NONE 0
#define BLKDEV_PART_MBR  1
#define BLKDEV_PART_GPT  2
int blkdev_partition_kind(int dev);

/* Cumulative sectors read/written through the seam, all devices.
 * Feeds /proc/diskstats.  NOTE the honest semantic shift from the
 * pre-seam counters: driver-internal traffic (ahci_self_test's MBR
 * probe, the driver's own sector-0 scan) no longer counts — these
 * are FILESYSTEM-LAYER counters now.  Either pointer may be NULL. */
void blkdev_get_stats(uint64_t *out_sectors_read,
                      uint64_t *out_sectors_written);

#endif /* AURALITE_KERNEL_FS_BLKDEV_H */

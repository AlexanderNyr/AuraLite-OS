/* kernel/arch/i386/ata32.c -- ATA PIO LBA28 (I386_PLAN I8).
 *
 * Primary channel, master drive, polled PIO -- the classic 0x1F0
 * register file (ATA-1 s.4; every PC and every QEMU IDE model).
 * IRQ-driven and DMA modes are consciously absent: at 512-byte
 * bring-up granularity, polling costs microseconds and buys the
 * self-test determinism (the same reason bl4's INT 13h reads poll).
 */

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/i386/ata32.h"
#include "kernel/arch/i386/portio.h"
#include "kernel/arch/i386/kprintf32.h"

#define ATA_DATA   0x1F0
#define ATA_ERR    0x1F1
#define ATA_COUNT  0x1F2
#define ATA_LBA0   0x1F3
#define ATA_LBA1   0x1F4
#define ATA_LBA2   0x1F5
#define ATA_DRIVE  0x1F6
#define ATA_STATUS 0x1F7        /* read: status; write: command */
#define ATA_CMD    0x1F7
#define ATA_CTRL   0x3F6

#define ST_ERR  (1 << 0)
#define ST_DRQ  (1 << 3)
#define ST_DF   (1 << 5)
#define ST_BSY  (1 << 7)

#define CMD_READ     0x20
#define CMD_WRITE    0x30
#define CMD_FLUSH    0xE7
#define CMD_IDENTIFY 0xEC

static uint32_t disk_sectors;

static int wait_not_busy(void)
{
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (!(st & ST_BSY))
            return 0;
    }
    return -1;
}

static int wait_drq(void)
{
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st & (ST_ERR | ST_DF))
            return -1;
        if (st & ST_DRQ)
            return 0;
    }
    return -1;
}

/* P7: drive 0 = master (0xE0), drive 1 = slave (0xF0, DEV bit).
 * The slave lane exists because the blkdev seam wants the ext2 disk
 * SEPARATE from the boot disk -- the same second-disk rule the
 * x86_64 boot has used since forever. */
static uint32_t slave_sectors;      /* 0 = no slave */

static void select_lba_d(int drive, uint32_t lba, uint8_t count)
{
    uint8_t dev = drive ? 0xF0 : 0xE0;
    outb(ATA_DRIVE, (uint8_t)(dev | ((lba >> 24) & 0x0F)));
    outb(ATA_COUNT, count);
    outb(ATA_LBA0, (uint8_t)lba);
    outb(ATA_LBA1, (uint8_t)(lba >> 8));
    outb(ATA_LBA2, (uint8_t)(lba >> 16));
}


int ata32_init(uint32_t *total_sectors)
{
    /* Presence probe: a floating bus reads 0xFF. */
    if (inb(ATA_STATUS) == 0xFF)
        return -1;

    outb(ATA_CTRL, 0x02);       /* nIEN: polled, no IRQs */

    outb(ATA_DRIVE, 0xE0);
    outb(ATA_COUNT, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_CMD, CMD_IDENTIFY);

    if (inb(ATA_STATUS) == 0)
        return -1;              /* no device */
    if (wait_not_busy() != 0 || wait_drq() != 0)
        return -1;

    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(ATA_DATA);

    /* Words 60-61: total LBA28 sectors. */
    disk_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (total_sectors)
        *total_sectors = disk_sectors;

    kprintf32("[ata] primary master: %u sectors (%u MiB), PIO LBA28\n",
              disk_sectors, disk_sectors / 2048);

    /* P7: probe the primary SLAVE the same way.  Absent slave reads
     * status 0 after IDENTIFY select -- an honest miss, not an error. */
    outb(ATA_DRIVE, 0xF0);
    outb(ATA_COUNT, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_CMD, CMD_IDENTIFY);
    if (inb(ATA_STATUS) != 0 &&
        wait_not_busy() == 0 && wait_drq() == 0) {
        uint16_t sid[256];
        for (int i = 0; i < 256; i++)
            sid[i] = inw(ATA_DATA);
        slave_sectors = (uint32_t)sid[60] | ((uint32_t)sid[61] << 16);
        kprintf32("[ata] primary slave: %u sectors (%u MiB), PIO LBA28\n",
                  slave_sectors, slave_sectors / 2048);
    } else {
        kprintf32("[ata] no primary slave (single-disk boot)\n");
    }
    return 0;
}

/* P7: the drive-parametrised pair the blkdev seam calls. */
uint32_t ata32_drive_sectors(int drive)
{
    return drive ? slave_sectors : disk_sectors;
}

int ata32_read_drv(int drive, uint32_t lba, uint8_t *buf512)
{
    if (lba >= ata32_drive_sectors(drive))
        return -1;
    if (wait_not_busy() != 0)
        return -1;
    select_lba_d(drive, lba, 1);
    outb(ATA_CMD, CMD_READ);
    if (wait_drq() != 0)
        return -1;
    uint16_t *w = (uint16_t *)buf512;
    for (int i = 0; i < 256; i++)
        w[i] = inw(ATA_DATA);
    return 0;
}

int ata32_write_drv(int drive, uint32_t lba, const uint8_t *buf512)
{
    if (lba >= ata32_drive_sectors(drive))
        return -1;
    if (wait_not_busy() != 0)
        return -1;
    select_lba_d(drive, lba, 1);
    outb(ATA_CMD, CMD_WRITE);
    if (wait_drq() != 0)
        return -1;
    const uint16_t *w = (const uint16_t *)buf512;
    for (int i = 0; i < 256; i++)
        outw(ATA_DATA, w[i]);
    outb(ATA_CMD, CMD_FLUSH);
    return wait_not_busy();
}

/* The pre-P7 master-only names, kept verbatim for every existing
 * caller (selftest, the I7/I8 gates). */
int ata32_read(uint32_t lba, uint8_t *buf512)
{
    return ata32_read_drv(0, lba, buf512);
}

int ata32_write(uint32_t lba, const uint8_t *buf512)
{
    return ata32_write_drv(0, lba, buf512);
}

int ata32_selftest(void)
{
    static uint8_t sec[512], pattern[512], readback[512];

    /* 1. LBA 0 is our own MBR: the boot signature proves the read
     * path against bytes we KNOW (Stage 1 booted from them). */
    if (ata32_read(0, sec) != 0)
        return -1;
    if (sec[510] != 0x55 || sec[511] != 0xAA) {
        kprintf32("[ata] FAIL: LBA 0 has no 0x55AA boot signature\n");
        return -1;
    }

    /* 2. Write/readback/restore on the last sector -- far from
     * anything the image uses, and restored afterwards because on
     * real hardware this is the user's USB stick. */
    uint32_t victim = disk_sectors - 1;
    if (ata32_read(victim, sec) != 0)
        return -1;
    for (int i = 0; i < 512; i++)
        pattern[i] = (uint8_t)(i ^ 0xA5);
    if (ata32_write(victim, pattern) != 0)
        return -1;
    if (ata32_read(victim, readback) != 0)
        return -1;
    for (int i = 0; i < 512; i++) {
        if (readback[i] != pattern[i]) {
            kprintf32("[ata] FAIL: readback mismatch at byte %u\n",
                      (uint32_t)i);
            return -1;
        }
    }
    if (ata32_write(victim, sec) != 0)      /* restore */
        return -1;

    kprintf32("[ata] PASS: boot-sector read + write/readback/restore "
              "on LBA %u\n", victim);
    return 0;
}

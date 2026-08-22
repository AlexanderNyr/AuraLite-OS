/* kernel/arch/i386/ata32.h -- ATA PIO storage (I386_PLAN I8).
 *
 * The honest storage layer for the i386 path: the boot medium IS an
 * IDE disk (every QEMU invocation in this repository attaches the
 * hybrid image with if=ide, and Stage 2 read it through INT 13h).
 * AHCI on i386 waits for the VFS port that would give it a consumer;
 * this driver gives the SAME guarantee the 64-bit AHCI self-test
 * gives -- sector read AND write proven at boot -- on the controller
 * the machine actually booted from.  LBA28, primary master, polled.
 */

#ifndef AURALITE_ARCH_I386_ATA32_H
#define AURALITE_ARCH_I386_ATA32_H

#include <stdint.h>

/* 0 on success.  Identify fills total LBA28 sector count. */
int ata32_init(uint32_t *total_sectors);
int ata32_read(uint32_t lba, uint8_t *buf512);

/* P7: drive-parametrised lane for the blkdev seam.  drive 0 =
 * primary master (the boot disk), 1 = primary slave (probed at
 * init; sectors 0 when absent). */
int ata32_read_drv(int drive, uint32_t lba, uint8_t *buf512);
int ata32_write_drv(int drive, uint32_t lba, const uint8_t *buf512);
uint32_t ata32_drive_sectors(int drive);
int ata32_write(uint32_t lba, const uint8_t *buf512);

/* Boot self-test: IDENTIFY, read LBA 0 (verify 0x55AA), then a
 * write/readback/restore cycle on the LAST sector -- restore matters:
 * on real hardware this runs against the user's USB stick. */
int ata32_selftest(void);

#endif /* AURALITE_ARCH_I386_ATA32_H */

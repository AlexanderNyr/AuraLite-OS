#ifndef AURALITE_DRIVERS_USB_MSC_H
#define AURALITE_DRIVERS_USB_MSC_H

#include <stdint.h>

/*
 * USB Mass Storage Class (MSC) driver — Bulk-Only Transport (BBB).
 *
 * Implements the SCSI-over-USB protocol for USB flash drives and similar
 * mass storage devices. Uses the UHCI/OHCI controller's transfer primitives.
 *
 * Protocol: each I/O operation consists of:
 *   1. Command Block Wrapper (CBW) sent via Bulk OUT
 *   2. Data transfer via Bulk IN (read) or Bulk OUT (write)
 *   3. Command Status Wrapper (CSW) received via Bulk IN
 *
 * Supported SCSI commands: INQUIRY, READ CAPACITY, READ(10), WRITE(10).
 */

#define MSC_SECTOR_SIZE 512

/* Initialise the MSC layer: enumerate USB devices and find the first MSC device.
 * Returns 0 on success. */
int msc_init(void);

/* Read sectors from the USB mass storage device.
 * @param lba     starting LBA
 * @param count   number of 512-byte sectors
 * @param buf     destination buffer (must be DMA-safe — allocated via PMM)
 * Returns 0 on success, -1 on error. */
int msc_read(uint64_t lba, uint32_t count, void *buf);

/* Write sectors to the USB mass storage device. */
int msc_write(uint64_t lba, uint32_t count, const void *buf);

/* Get the total number of sectors on the device. */
uint32_t msc_get_sector_count(void);

/* Gate self-test. */
void msc_self_test(void);

#endif /* AURALITE_DRIVERS_USB_MSC_H */

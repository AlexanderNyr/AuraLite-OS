#ifndef AURALITE_DRIVERS_AHCI_AHCI_H
#define AURALITE_DRIVERS_AHCI_AHCI_H

#include <stdint.h>

/*
 * AHCI (Advanced Host Controller Interface) SATA driver.
 *
 * Supports: port enumeration, device detection, and sector read/write via DMA.
 * Designed for QEMU's ICH9 AHCI controller (-device ahci).
 *
 * The driver allocates command lists, command tables, and PRDT entries from
 * the PMM (physical memory for DMA) and accesses them through the HHDM.
 */

#define AHCI_SECTOR_SIZE 512
#define AHCI_MAX_PORTS   32

/* Initialise the AHCI driver: find the controller on PCI, map ABAR,
 * enumerate ports, and set up command structures. Returns 0 on success. */
int ahci_init(void);

/*
 * Read `count` sectors (512 bytes each) from the SATA device on `port`,
 * starting at LBA `lba`, into `buf`. Returns 0 on success, -1 on error.
 */
int ahci_read(uint32_t port, uint64_t lba, uint32_t count, void *buf);

/*
 * Write `count` sectors to the SATA device on `port` at LBA `lba` from `buf`.
 * Returns 0 on success, -1 on error.
 */
int ahci_write(uint32_t port, uint64_t lba, uint32_t count, const void *buf);

/* Get the number of detected AHCI ports with devices attached. */
int ahci_get_port_count(void);

/* Gate self-test: read sector 0 (MBR) and verify it's non-empty. */
void ahci_self_test(void);

#endif /* AURALITE_DRIVERS_AHCI_AHCI_H */

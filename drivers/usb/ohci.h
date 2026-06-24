#ifndef AURALITE_DRIVERS_USB_OHCI_H
#define AURALITE_DRIVERS_USB_OHCI_H

#include <stdint.h>

/*
 * OHCI (Open Host Controller Interface) USB 1.1 driver.
 *
 * Targets USB controllers with PCI prog_if 0x10 (OHCI). Uses memory-mapped
 * registers (unlike UHCI's I/O ports). The HCCA (Host Controller Communications
 * Area) is a 256-byte structure the controller DMA accesses every frame.
 *
 * OHCI is common on real hardware (PowerMac, older PCs) and QEMU supports it
 * via `-device pci-ohci,id=ohci`.
 */

#define OHCI_MAX_PORTS  3   /* typical OHCI controllers have 2-15 ports */

/* Initialise the OHCI controller. Returns 0 on success. */
int ohci_init(void);

/* Get the number of ports with devices attached. */
int ohci_get_port_count(void);

/* Transfer backend API.  The current OHCI driver has controller/port bring-up;
 * these functions provide a stable interface for usb_core/class drivers and
 * return -1 until ED/TD scheduling is completed.
 */
int ohci_control_transfer(uint8_t dev_addr, int low_speed,
                          const void *setup, void *data,
                          uint16_t data_len, uint8_t max_packet0);
int ohci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint,
                       void *data, uint32_t len, int in, uint16_t max_packet);

/* Gate self-test. */
void ohci_self_test(void);

#endif /* AURALITE_DRIVERS_USB_OHCI_H */

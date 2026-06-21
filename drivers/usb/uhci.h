#ifndef AURALITE_DRIVERS_USB_UHCI_H
#define AURALITE_DRIVERS_USB_UHCI_H

#include <stdint.h>

/*
 * UHCI (Universal Host Controller Interface) USB 1.1 driver.
 *
 * Targets the Intel PIIX3/PIIX4 USB controller (QEMU default). Detects the
 * controller via PCI, initialises the frame list, enumerates ports for
 * attached devices, and supports basic CONTROL/BULK transfers.
 *
 * QEMU: the UHCI controller is built into the PIIX3 southbridge and appears
 * automatically when USB is enabled. No special -device flag needed.
 */

#define UHCI_MAX_PORTS   2   /* PIIX3 has 2 USB ports */

/* Initialise the UHCI controller: find on PCI, reset, set up frame list. */
int uhci_init(void);

/* Get the number of ports with devices attached. */
int uhci_get_port_count(void);

/* Check if a device is present on port N (0-indexed). */
int uhci_port_has_device(int port);

/* Gate self-test: reset, detect ports, report device count. */
void uhci_self_test(void);

#endif /* AURALITE_DRIVERS_USB_UHCI_H */

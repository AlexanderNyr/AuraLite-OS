#ifndef AURALITE_DRIVERS_USB_EHCI_H
#define AURALITE_DRIVERS_USB_EHCI_H

#include <stdint.h>

/*
 * EHCI (Enhanced Host Controller Interface) USB 2.0 driver.
 *
 * Targets high-speed (480 Mbps) USB devices via PCI class 0x0C/0x03/prog_if
 * 0x20. Uses memory-mapped registers and a periodic frame list with linked
 * lists of iTDs (isochronous), siTDs (split), and qTDs (async).
 *
 * Key EHCI concepts:
 *   - The async schedule is a circular list of Queue Heads (QH) linked via
 *     physical addresses. Each QH points to a chain of qTDs (queue Transfer
 *     Descriptors).
 *   - The periodic schedule uses a 1024-entry frame list, each pointing to
 *     QHs for interrupt endpoints.
 *   - Companion controllers (UHCI/OHCI) handle low/full-speed devices.
 *
 * QEMU: EHCI is the default high-speed USB controller. It appears
 * automatically when `-usb` is used (QEMU adds a built-in ICH9 EHCI).
 */

#define EHCI_MAX_PORTS  15   /* EHCI root hub can have up to 15 ports */

/* Initialise the EHCI controller. Returns 0 on success. */
int ehci_init(void);

/* Get the number of ports with devices attached. */
int ehci_get_port_count(void);

/* Gate self-test. */
void ehci_self_test(void);

#endif /* AURALITE_DRIVERS_USB_EHCI_H */

#ifndef AURALITE_DRIVERS_USB_XHCI_H
#define AURALITE_DRIVERS_USB_XHCI_H

#include <stdint.h>

/*
 * xHCI (eXtensible Host Controller Interface) — USB 3.0 / 2.0.
 *
 * The most advanced USB host controller interface: superseded UHCI, OHCI, and
 * EHCI. Handles all speeds (low/full/high/superSpeed) in a single driver.
 *
 * PCI class: 0x0C / subclass 0x03 / prog_if 0x30 (xHCI).
 *
 * QEMU: -device qemu-xhci,id=xhci (or -device nec-usb-xhci)
 */

#define XHCI_MAX_PORTS  32
#define XHCI_MAX_SLOTS  32

/* Initialise the xHCI controller. Returns 0 on success. */
int xhci_init(void);

/* Get the number of ports with devices attached. */
int xhci_get_port_count(void);

/* Gate self-test. */
void xhci_self_test(void);

#endif /* AURALITE_DRIVERS_USB_XHCI_H */

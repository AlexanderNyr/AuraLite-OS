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

/* Transfer backend API.  xHCI rings/contexts are scaffolded; these functions
 * return -1 until slot addressing and endpoint transfer rings are completed.
 */
int xhci_control_transfer(uint8_t dev_addr, int low_speed,
                          const void *setup, void *data,
                          uint16_t data_len, uint8_t max_packet0);
int xhci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint,
                       void *data, uint32_t len, int in, uint16_t max_packet);

/* Gate self-test. */
void xhci_self_test(void);

#endif /* AURALITE_DRIVERS_USB_XHCI_H */

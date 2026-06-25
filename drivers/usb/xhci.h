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
int xhci_port_has_device(int port);
int xhci_port_speed(int port);
int xhci_reset_port(int port);

/* Address a device attached to an xHCI root port.  xHCI does not use the USB
 * SET_ADDRESS request on the wire; usb_core calls this when it reaches the
 * SET_ADDRESS enumeration step. */
int xhci_address_device(uint8_t usb_addr, int port, int speed, uint8_t max_packet0);

/* Transfer backend API. */
int xhci_control_transfer(uint8_t dev_addr, int low_speed,
                          const void *setup, void *data,
                          uint16_t data_len, uint8_t max_packet0);
int xhci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint,
                       void *data, uint32_t len, int in, uint16_t max_packet);
int xhci_interrupt_transfer(uint8_t dev_addr, uint8_t endpoint,
                            int low_speed, uint16_t max_packet,
                            void *data, uint16_t len, int *toggle_io);

/* Gate self-test. */
void xhci_self_test(void);

#endif /* AURALITE_DRIVERS_USB_XHCI_H */

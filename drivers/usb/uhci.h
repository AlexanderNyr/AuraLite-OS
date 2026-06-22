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

/* Get the low-speed flag for a port. Returns 1=low-speed, 0=full-speed. */
int uhci_port_is_low_speed(int port);

/*
 * Execute a USB control transfer via UHCI.
 * Builds SETUP → DATA → STATUS TD chain and schedules it for one frame.
 *
 * @param dev_addr   USB device address
 * @param low_speed  1=low-speed device, 0=full-speed
 * @param setup      8-byte setup packet
 * @param data       data buffer (NULL for no-data phase)
 * @param data_len   number of data bytes
 * @return total bytes transferred, or -1 on error
 */
int uhci_control_transfer(uint8_t dev_addr, int low_speed,
                          const void *setup, void *data, uint16_t data_len);

/*
 * Execute a USB bulk transfer via UHCI.
 *
 * @param dev_addr   USB device address
 * @param endpoint   endpoint number (0-15, bit 7=IN direction)
 * @param data       data buffer
 * @param len        number of bytes
 * @return bytes transferred, or -1
 */
int uhci_bulk_transfer(uint8_t dev_addr, uint8_t endpoint,
                       void *data, uint32_t len);

/* Gate self-test: reset, detect ports, report device count. */
void uhci_self_test(void);

#endif /* AURALITE_DRIVERS_USB_UHCI_H */

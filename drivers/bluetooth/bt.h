#ifndef AURALITE_DRIVERS_BLUETOOTH_BT_H
#define AURALITE_DRIVERS_BLUETOOTH_BT_H

#include <stdint.h>

#include "drivers/bluetooth/bt_hci.h"

/*
 * Bluetooth HCI (Host Controller Interface) driver.
 *
 * Communicates with a Bluetooth controller via USB Bulk endpoints.
 * The controller appears as a USB device (class 0xE0 = Wireless, subclass
 * 0x01 = RF Controller, protocol 0x01 = Bluetooth). Three endpoints are used:
 *
 *   - Control endpoint 0: HCI commands (via usb_control_transfer)
 *   - Bulk IN endpoint:   HCI events + ACL data (host reads asynchronously)
 *   - Bulk OUT endpoint:  HCI ACL data (host writes)
 *   - Interrupt IN:       HCI events (alternative to bulk IN)
 *
 * This driver implements:
 *   - USB device detection (find the BT controller among enumerated devices)
 *   - HCI Reset command
 *   - HCI Read Buffer Size
 *   - HCI Read BD_ADDR (Bluetooth Device Address)
 *   - HCI Inquiry (scan for nearby BT devices)
 *
 * QEMU: add a virtual BT controller with:
 *   -device bt-tablet    (or -device bt-mouse)
 *   No explicit controller device needed — QEMU provides an internal HCI.
 */

/* Initialise the Bluetooth subsystem: find the controller, reset, read BD_ADDR. */
int bt_init(void);

/* Get the controller's Bluetooth Device Address. */
int bt_get_bd_addr(bd_addr_t *addr);

/* Perform a Bluetooth inquiry (device scan) for `duration` * 1.28 seconds. */
int bt_inquiry(uint8_t duration, int max_results);

/* Self-test. */
void bt_self_test(void);

#endif /* AURALITE_DRIVERS_BLUETOOTH_BT_H */

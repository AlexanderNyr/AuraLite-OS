#ifndef AURALITE_DRIVERS_BLUETOOTH_BT_H
#define AURALITE_DRIVERS_BLUETOOTH_BT_H

#include <stdint.h>

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

/* HCI packet types (first byte of each packet). */
#define HCI_CMD_PKT     0x01
#define HCI_ACL_PKT     0x02
#define HCI_SCO_PKT     0x03
#define HCI_EVT_PKT     0x04

/* HCI command opcodes (OGF << 10 | OCF). */
#define HCI_RESET                0x0C03
#define HCI_READ_BD_ADDR         0x1009
#define HCI_READ_BUFFER_SIZE     0x1005
#define HCI_READ_LOCAL_VERSION   0x1001
#define HCI_WRITE_SCAN_ENABLE    0x0C1A
#define HCI_INQUIRY              0x0401
#define HCI_INQUIRY_CANCEL       0x0402
#define HCI_SET_EVENT_MASK       0x0C01

/* HCI event codes. */
#define HCI_EVT_INQUIRY_COMPLETE   0x01
#define HCI_EVT_INQUIRY_RESULT     0x02
#define HCI_EVT_CMD_COMPLETE       0x0E
#define HCI_EVT_CMD_STATUS         0x0F
#define HCI_EVT_NUM_COMPLETED_PKTS 0x13
#define HCI_EVT_LE_META            0x3E

/* BD_ADDR (Bluetooth Device Address) — 6 bytes. */
typedef struct { uint8_t b[6]; } bd_addr_t;

/* Initialise the Bluetooth subsystem: find the controller, reset, read BD_ADDR. */
int bt_init(void);

/* Get the controller's Bluetooth Device Address. */
int bt_get_bd_addr(bd_addr_t *addr);

/* Perform a Bluetooth inquiry (device scan) for `duration` * 1.28 seconds. */
int bt_inquiry(uint8_t duration, int max_results);

/* Self-test. */
void bt_self_test(void);

#endif /* AURALITE_DRIVERS_BLUETOOTH_BT_H */

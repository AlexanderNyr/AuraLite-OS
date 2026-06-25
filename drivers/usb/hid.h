#ifndef AURALITE_DRIVERS_USB_HID_H
#define AURALITE_DRIVERS_USB_HID_H

#include <stdint.h>

/* USB HID boot keyboard/mouse class driver.
 *
 * Current transport backend: UHCI interrupt-IN endpoints.  The driver switches
 * HID boot devices to Boot Protocol, polls their interrupt endpoint from a
 * kernel thread, and injects decoded events into the existing keyboard/mouse
 * queues so the shell and GUI do not need a separate input path.
 */

#define USB_HID_MAX_DEVICES 8

int  usb_hid_init(void);
int  usb_hid_attach_device(void *usb_dev);
int  usb_hid_device_count(void);
void usb_hid_self_test(void);

#endif /* AURALITE_DRIVERS_USB_HID_H */

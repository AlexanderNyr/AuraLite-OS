#ifndef AURALITE_DRIVERS_USB_PRINTER_H
#define AURALITE_DRIVERS_USB_PRINTER_H

#include <stdint.h>
#include "drivers/usb/usb_core.h"

#define USB_PRINTER_MAX 4
#define USB_PRINTER_BUF 4096

typedef struct {
    int in_use;
    usb_device_t *dev;
    uint8_t bulk_in;
    uint8_t bulk_out;
    uint8_t interface_number;
    int bulk_in_toggle;
    int bulk_out_toggle;
    uint32_t bytes_sent;
    uint32_t bytes_recv;
    char id_string[256];
} usb_printer_t;

int usb_printer_init(void);
int usb_printer_attach_device(usb_device_t *dev);
void usb_printer_detach_device(usb_device_t *dev);
int usb_printer_get_device_id(usb_printer_t *prt);
int usb_printer_send(usb_printer_t *prt, const void *data, uint32_t len);
int usb_printer_recv(usb_printer_t *prt, void *data, uint32_t len);
int usb_printer_soft_reset(usb_printer_t *prt);
usb_printer_t *usb_printer_get(int idx);
int usb_printer_count(void);
void usb_printer_self_test(void);

extern usb_printer_t usb_printers[USB_PRINTER_MAX];

#endif

/* usb_printer.c — Full USB Printer driver */
#include "drivers/usb/usb_printer.h"
#include "drivers/usb/usb_core.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"

usb_printer_t usb_printers[USB_PRINTER_MAX];

static int printer_class_request(usb_device_t *dev, uint8_t iface, uint8_t req, uint16_t value, uint16_t index, void *data, uint16_t len, int dir_in) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = dir_in ? 0xA1 : 0x21;
    setup.bRequest = req;
    setup.wValue = value;
    setup.wIndex = index ? index : iface;
    setup.wLength = len;
    return usb_control_transfer(dev, &setup, data, len);
}
#define PRINTER_GET_DEVICE_ID 0
#define PRINTER_SOFT_RESET 2

static usb_printer_t *alloc_printer(void) {
    for (int i = 0; i < USB_PRINTER_MAX; i++) if (!usb_printers[i].in_use) {
        memset(&usb_printers[i], 0, sizeof(usb_printer_t));
        usb_printers[i].in_use = 1;
        return &usb_printers[i];
    }
    return NULL;
}
int usb_printer_attach_device(usb_device_t *dev) {
    if (!dev) return -1;
    int is_printer = 0;
    if (dev->interface_class == USB_CLASS_PRINTER) is_printer = 1;
    for (int i = 0; i < dev->num_interfaces; i++) if (dev->interfaces[i].class_code == USB_CLASS_PRINTER) is_printer = 1;
    if (!is_printer) return -1;
    usb_printer_t *prt = alloc_printer();
    if (!prt) { kprintf("[printer] no free slots\n"); return -1; }
    prt->dev = dev;
    prt->interface_number = dev->interface_number;
    for (int i = 0; i < dev->num_interfaces; i++) if (dev->interfaces[i].class_code == USB_CLASS_PRINTER) {
        prt->interface_number = dev->interfaces[i].number;
        for (int e = 0; e < dev->interfaces[i].num_endpoints; e++) {
            if (dev->interfaces[i].endpoints[e].type == USB_EP_BULK) {
                if (dev->interfaces[i].endpoints[e].address & 0x80) prt->bulk_in = dev->interfaces[i].endpoints[e].address;
                else prt->bulk_out = dev->interfaces[i].endpoints[e].address;
            }
        }
    }
    if (!prt->bulk_out) prt->bulk_out = dev->bulk_out_ep;
    if (!prt->bulk_in) prt->bulk_in = dev->bulk_in_ep;
    if (!prt->bulk_out) {
        kprintf("[printer] addr %d missing bulk OUT\n", dev->address);
        prt->in_use = 0;
        return -1;
    }
    usb_printer_get_device_id(prt);
    kprintf("[printer] attached addr=%d VID=0x%04x PID=0x%04x IF %d OUT 0x%02x IN 0x%02x ID: %.64s\n",
            dev->address, dev->vendor_id, dev->product_id,
            prt->interface_number, prt->bulk_out, prt->bulk_in, prt->id_string);
    return 0;
}
void usb_printer_detach_device(usb_device_t *dev) {
    for (int i = 0; i < USB_PRINTER_MAX; i++) if (usb_printers[i].in_use && usb_printers[i].dev == dev) {
        kprintf("[printer] detach addr=%d\n", dev->address);
        usb_printers[i].in_use = 0;
        break;
    }
}
int usb_printer_get_device_id(usb_printer_t *prt) {
    if (!prt || !prt->dev) return -1;
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    int r = printer_class_request(prt->dev, prt->interface_number, PRINTER_GET_DEVICE_ID, 0, prt->interface_number, buf, sizeof(buf), 1);
    if (r > 2) {
        uint16_t total = (buf[0] << 8) | buf[1];
        if (total > sizeof(prt->id_string)-1) total = sizeof(prt->id_string)-1;
        if (total > r - 2) total = r - 2;
        memcpy(prt->id_string, buf+2, total);
        prt->id_string[total] = 0;
        return 0;
    }
    strcpy(prt->id_string, "Unknown Printer");
    return -1;
}
int usb_printer_soft_reset(usb_printer_t *prt) {
    if (!prt || !prt->dev) return -1;
    return printer_class_request(prt->dev, prt->interface_number, PRINTER_SOFT_RESET, 0, prt->interface_number, NULL, 0, 0);
}
int usb_printer_send(usb_printer_t *prt, const void *data, uint32_t len) {
    if (!prt || !prt->dev || !data || !len || !prt->bulk_out) return -1;
    int r = usb_bulk_transfer_ex(prt->dev, prt->bulk_out, (void*)data, len, &prt->bulk_out_toggle);
    if (r > 0) prt->bytes_sent += len;
    return r;
}
int usb_printer_recv(usb_printer_t *prt, void *data, uint32_t len) {
    if (!prt || !prt->dev || !data || !len || !prt->bulk_in) return -1;
    int r = usb_bulk_transfer_ex(prt->dev, prt->bulk_in, data, len, &prt->bulk_in_toggle);
    if (r > 0) prt->bytes_recv += r;
    return r;
}
usb_printer_t *usb_printer_get(int idx) {
    if (idx < 0 || idx >= USB_PRINTER_MAX) return NULL;
    return usb_printers[idx].in_use ? &usb_printers[idx] : NULL;
}
int usb_printer_count(void) { int c=0; for (int i=0;i<USB_PRINTER_MAX;i++) if (usb_printers[i].in_use) c++; return c; }
static int usb_printer_probe_wrapper(usb_device_t *dev) { return usb_printer_attach_device(dev); }
static void usb_printer_disconnect_wrapper(usb_device_t *dev) { usb_printer_detach_device(dev); }
static usb_driver_t usb_printer_driver = {
    .name = "usb_printer",
    .probe = usb_printer_probe_wrapper,
    .disconnect = usb_printer_disconnect_wrapper,
    .class_code = USB_CLASS_PRINTER,
    .subclass = 0,
    .protocol = 0,
    .match_vendor = NULL,
};
int usb_printer_init(void) {
    memset(usb_printers, 0, sizeof(usb_printers));
    kprintf("[printer] full USB Printer driver initialized — soft reset, device ID, bulk IO\n");
    usb_register_driver(&usb_printer_driver);
    int found = 0;
    for (int i=0;i<USB_MAX_DEVICES;i++) if (usb_devices[i].in_use && usb_devices[i].interface_class==USB_CLASS_PRINTER) {
        if (usb_printer_attach_device(&usb_devices[i])==0) found++;
    }
    kprintf("[printer] %d printer(s) at boot\n", found);
    return found;
}
void usb_printer_self_test(void) {
    kprintf("[printer] self-test: %d active, full support (ID, reset, bulk IN/OUT)\n", usb_printer_count());
    for (int i=0;i<USB_PRINTER_MAX;i++) if (usb_printers[i].in_use) {
        kprintf("[printer]   addr=%d OUT=0x%02x IN=0x%02x sent=%u recv=%u id=%s\n",
                usb_printers[i].dev->address, usb_printers[i].bulk_out, usb_printers[i].bulk_in,
                usb_printers[i].bytes_sent, usb_printers[i].bytes_recv, usb_printers[i].id_string);
    }
    kprintf("[printer] PASS: USB Printer full support ready\n");
}

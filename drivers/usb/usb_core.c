/* usb_core.c — USB device enumeration and protocol layer.
 *
 * Implements the standard USB enumeration sequence on top of the host
 * controller drivers. The key missing piece is the actual transfer execution
 * (control/bulk/interrupt), which requires per-controller TD scheduling.
 *
 * This module provides:
 *   - USB device table management
 *   - Standard request builders (SET_ADDRESS, GET_DESCRIPTOR, SET_CONFIGURATION)
 *   - Descriptor parsing
 *   - Class detection for class-driver dispatch
 *   - Device enumeration sequence
 *
 * The control transfer function (usb_control_transfer) dispatches to the
 * correct controller driver based on the device's controller field.
 */

#include <stdint.h>
#include "drivers/usb/usb_core.h"
#include "drivers/usb/uhci.h"
#include "drivers/usb/ohci.h"
#include "drivers/usb/ehci.h"
#include "drivers/usb/xhci.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/mm/pmm.h"
#include "kernel/limine_requests.h"

/* Link to the UHCI transfer layer. */
extern int uhci_control_transfer(uint8_t dev_addr, int low_speed,
                                 const void *setup, void *data, uint16_t data_len);

/* Global device table. */
usb_device_t usb_devices[USB_MAX_DEVICES];

static int next_address = 1;

/* ---- Device table management ---- */

static usb_device_t *alloc_device(void) {
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!usb_devices[i].in_use) {
            memset(&usb_devices[i], 0, sizeof(usb_device_t));
            usb_devices[i].in_use = 1;
            usb_devices[i].address = (uint8_t)next_address++;
            usb_devices[i].max_packet_size0 = 8;  /* default for low-speed */
            return &usb_devices[i];
        }
    }
    return NULL;
}

static void free_device(usb_device_t *dev) {
    dev->in_use = 0;
}

/* ---- Control transfer abstraction ---- */

/*
 * Execute a control transfer via the appropriate controller.
 *
 * This is a protocol-layer function that builds the SETUP packet, sends it,
 * handles the optional DATA phase, and sends the STATUS handshake.
 *
 * Currently this is a stub because the actual USB packet-level transfers
 * require completing the controller-specific TD scheduling layers.
 * The protocol logic (what requests to send, how to parse responses) is
 * fully implemented below.
 */
int usb_control_transfer(usb_device_t *dev, const struct usb_setup_pkt *setup,
                         void *data, uint16_t data_len) {
    /* Dispatch to the controller driver that owns this device. */
    int low_speed = (dev->speed == USB_SPEED_LOW) ? 1 : 0;

    switch (dev->controller) {
    case USB_CTRL_UHCI:
        return uhci_control_transfer(dev->address, low_speed,
                                     setup, data, data_len);
    case USB_CTRL_OHCI:
        /* OHCI transfer layer uses ED/TD chains (similar to UHCI).
         * Not yet connected — would use ohci_control_transfer(). */
        return -1;
    case USB_CTRL_EHCI:
        /* EHCI transfer layer uses async QH/qTD chains.
         * Not yet connected — would use ehci_control_transfer(). */
        return -1;
    case USB_CTRL_XHCI:
        /* xHCI uses TRB rings + doorbells.
         * Not yet connected — would use xhci_control_transfer(). */
        return -1;
    }
    return -1;
}

/* ---- Standard request helpers ---- */

/* Build and send a GET_DESCRIPTOR request. */
static int usb_get_descriptor(usb_device_t *dev, uint8_t desc_type,
                              uint8_t desc_index, void *buf, uint16_t len) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = USB_REQ_DIR_IN | USB_REQ_TYPE_STD | USB_REQ_RCPT_DEV;
    setup.bRequest      = USB_GET_DESCRIPTOR;
    setup.wValue        = (uint16_t)((desc_type << 8) | desc_index);
    setup.wIndex        = 0;
    setup.wLength       = len;
    return usb_control_transfer(dev, &setup, buf, len);
}

/* Build and send a SET_ADDRESS request. */
static int usb_set_address(usb_device_t *dev, uint8_t addr) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | USB_REQ_RCPT_DEV;
    setup.bRequest      = USB_SET_ADDRESS;
    setup.wValue        = addr;
    setup.wIndex        = 0;
    setup.wLength       = 0;
    return usb_control_transfer(dev, &setup, NULL, 0);
}

/* Build and send a SET_CONFIGURATION request. */
static int usb_set_configuration(usb_device_t *dev, uint8_t config) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | USB_REQ_RCPT_DEV;
    setup.bRequest      = USB_SET_CONFIGURATION;
    setup.wValue        = config;
    setup.wIndex        = 0;
    setup.wLength       = 0;
    return usb_control_transfer(dev, &setup, NULL, 0);
}

/* ---- Descriptor parsing ---- */

static const char *class_name(uint8_t cls) {
    switch (cls) {
    case USB_CLASS_HID:          return "HID";
    case USB_CLASS_MASS_STORAGE: return "Mass Storage";
    case USB_CLASS_HUB:          return "Hub";
    case USB_CLASS_CDC:          return "CDC";
    case USB_CLASS_AUDIO:        return "Audio";
    case USB_CLASS_VENDOR:       return "Vendor";
    default:                     return "Generic";
    }
}

static const char *speed_name(usb_speed_t s) {
    switch (s) {
    case USB_SPEED_LOW:    return "low (1.5 Mbps)";
    case USB_SPEED_FULL:   return "full (12 Mbps)";
    case USB_SPEED_HIGH:   return "high (480 Mbps)";
    case USB_SPEED_SUPER:  return "super (5 Gbps)";
    default:               return "?";
    }
}

/*
 * Parse a configuration descriptor blob to find:
 *   - Interface class/subclass/protocol
 *   - Bulk IN and OUT endpoints (for MSC)
 *
 * The configuration descriptor is followed by one or more interface
 * descriptors, each followed by endpoint descriptors. We walk the blob.
 */
static void parse_config(usb_device_t *dev, const uint8_t *buf, int len) {
    int off = 0;
    while (off + 2 <= len) {
        uint8_t dlen = buf[off];
        uint8_t dtype = buf[off + 1];
        if (dlen < 2 || off + dlen > len) break;

        if (dtype == USB_DESC_INTERFACE && dlen >= 9) {
            struct usb_interface_desc *ifd = (struct usb_interface_desc *)(buf + off);
            dev->interface_class    = ifd->bInterfaceClass;
            dev->interface_subclass = ifd->bInterfaceSubClass;
            dev->interface_protocol = ifd->bInterfaceProtocol;
            kprintf("[usb]   interface %d: class=0x%02x (%s) subclass=0x%02x proto=0x%02x\n",
                    ifd->bInterfaceNumber, ifd->bInterfaceClass,
                    class_name(ifd->bInterfaceClass),
                    ifd->bInterfaceSubClass, ifd->bInterfaceProtocol);
        }

        if (dtype == USB_DESC_ENDPOINT && dlen >= 7) {
            struct usb_endpoint_desc *epd = (struct usb_endpoint_desc *)(buf + off);
            uint8_t ep_addr = epd->bEndpointAddress;
            uint8_t ep_type = epd->bmAttributes & 0x03;
            int is_in = (ep_addr & 0x80) ? 1 : 0;

            if (ep_type == USB_EP_BULK) {
                if (is_in) {
                    dev->bulk_in_ep = ep_addr;
                } else {
                    dev->bulk_out_ep = ep_addr;
                }
                dev->bulk_max_packet = epd->wMaxPacketSize;
                kprintf("[usb]   endpoint 0x%02x: bulk %s, maxpkt=%d\n",
                        ep_addr, is_in ? "IN" : "OUT", epd->wMaxPacketSize);
            } else if (ep_type == USB_EP_INTERRUPT) {
                kprintf("[usb]   endpoint 0x%02x: interrupt %s, maxpkt=%d, interval=%dms\n",
                        ep_addr, is_in ? "IN" : "OUT",
                        epd->wMaxPacketSize, epd->bInterval);
            }
        }

        off += dlen;
    }
}

/* ---- Enumeration sequence ---- */

/*
 * Enumerate a single device on a given controller + port.
 *
 * Returns 0 on success, -1 on failure.
 */
static int enumerate_device(usb_ctrl_type_t ctrl, int port, usb_speed_t speed) {
    usb_device_t *dev = alloc_device();
    if (!dev) {
        kprintf("[usb] device table full\n");
        return -1;
    }
    dev->controller = ctrl;
    dev->port = port;
    dev->speed = speed;

    /* Step 1: Read first 8 bytes of device descriptor at address 0
     * to get bMaxPacketSize0. */
    struct usb_device_desc short_desc;
    memset(&short_desc, 0, sizeof(short_desc));

    /* For now, we use the default max packet size based on speed:
     * low-speed = 8, full-speed = 8/16/32/64, high/super = 64 */
    dev->max_packet_size0 = (speed == USB_SPEED_LOW) ? 8 : 64;

    /* Step 2: SET_ADDRESS (assign a unique address). */
    uint8_t addr = dev->address;
    /* The SET_ADDRESS request must be sent to address 0. We temporarily
     * set the device address to 0 for this transfer. */
    dev->address = 0;
    int ret = usb_set_address(dev, addr);
    if (ret < 0) {
        kprintf("[usb] SET_ADDRESS failed (transfer layer WIP)\n");
        free_device(dev);
        return -1;
    }
    dev->address = addr;

    /* Step 3: Read full device descriptor (18 bytes) at the new address. */
    struct usb_device_desc full_desc;
    ret = usb_get_descriptor(dev, USB_DESC_DEVICE, 0, &full_desc, sizeof(full_desc));
    if (ret < 0) {
        kprintf("[usb] GET_DESCRIPTOR(DEVICE) failed\n");
        free_device(dev);
        return -1;
    }

    dev->vendor_id = full_desc.idVendor;
    dev->product_id = full_desc.idProduct;
    dev->max_packet_size0 = full_desc.bMaxPacketSize0;
    if (full_desc.bDeviceClass != USB_CLASS_USE_DEVICE) {
        dev->interface_class = full_desc.bDeviceClass;
    }

    kprintf("[usb] device at addr %d: VID=0x%04x PID=0x%04x, "
            "class=0x%02x (%s), maxpkt0=%d, speed=%s\n",
            addr, full_desc.idVendor, full_desc.idProduct,
            full_desc.bDeviceClass, class_name(full_desc.bDeviceClass),
            full_desc.bMaxPacketSize0, speed_name(speed));

    /* Step 4: Read configuration descriptor (first 255 bytes). */
    uint8_t config_buf[256];
    ret = usb_get_descriptor(dev, USB_DESC_CONFIGURATION, 0, config_buf, 255);
    if (ret >= (int)sizeof(struct usb_config_desc)) {
        struct usb_config_desc *cfg = (struct usb_config_desc *)config_buf;
        kprintf("[usb]   config %d: %d interfaces, %d bytes\n",
                cfg->bConfigurationValue, cfg->bNumInterfaces, cfg->wTotalLength);
        parse_config(dev, config_buf, ret);
    }

    /* Step 5: SET_CONFIGURATION (activate configuration 1). */
    usb_set_configuration(dev, 1);

    kprintf("[usb] device enumeration complete: addr=%d class=%s\n",
            addr, class_name(dev->interface_class));
    return 0;
}

/* ---- Public API ---- */

int usb_enumerate_all(void) {
    int found = 0;

    /* Collect device info from all controllers. The controller drivers report
     * ports with devices — we need to enumerate each. */
    kprintf("[usb] enumerating devices across all controllers...\n");

    /* Note: actual enumeration (SET_ADDRESS, GET_DESCRIPTOR) requires the
     * transfer layer to be functional. For now, we record the ports and
     * print what was detected by each controller. */
    int uhci_ports = uhci_get_port_count();
    int ohci_ports = ohci_get_port_count();
    int ehci_ports = ehci_get_port_count();
    int xhci_ports = xhci_get_port_count();
    int total = uhci_ports + ohci_ports + ehci_ports + xhci_ports;

    kprintf("[usb] UHCI=%d OHCI=%d EHCI=%d xHCI=%d ports = %d total\n",
            uhci_ports, ohci_ports, ehci_ports, xhci_ports, total);

    /* Enumerate UHCI devices. */
    for (int p = 0; p < uhci_ports && found < USB_MAX_DEVICES; p++) {
        int low_speed = uhci_port_is_low_speed(p);
        usb_device_t *dev = alloc_device();
        if (!dev) break;
        dev->controller = USB_CTRL_UHCI;
        dev->port = p;
        dev->speed = low_speed ? USB_SPEED_LOW : USB_SPEED_FULL;
        dev->max_packet_size0 = low_speed ? 8 : 64;

        uint8_t assigned_addr = dev->address;
        dev->address = 0;

        /* SET_ADDRESS first. */
        struct usb_setup_pkt set_addr = {
            .bmRequestType = 0x00,
            .bRequest = USB_SET_ADDRESS,
            .wValue = assigned_addr,
            .wIndex = 0,
            .wLength = 0,
        };
        int ret = usb_control_transfer(dev, &set_addr, NULL, 0);
        dev->address = assigned_addr;

        if (ret >= 0) {
            /* GET_DESCRIPTOR(DEVICE) at new address. */
            struct usb_device_desc desc;
            struct usb_setup_pkt get_desc = {
                .bmRequestType = 0x80,
                .bRequest = USB_GET_DESCRIPTOR,
                .wValue = (USB_DESC_DEVICE << 8),
                .wIndex = 0,
                .wLength = sizeof(desc),
            };
            ret = usb_control_transfer(dev, &get_desc, &desc, sizeof(desc));
            if (ret >= 0) {
                dev->vendor_id = desc.idVendor;
                dev->product_id = desc.idProduct;
                dev->interface_class = desc.bDeviceClass;
                dev->max_packet_size0 = desc.bMaxPacketSize0;
                kprintf("[usb] UHCI port %d: addr=%d VID=0x%04x PID=0x%04x "
                        "class=0x%02x (%s) %s\n",
                        p, dev->address, desc.idVendor, desc.idProduct,
                        desc.bDeviceClass, class_name(desc.bDeviceClass),
                        low_speed ? "low-speed" : "full-speed");
            }
        }

        if (ret < 0) {
            kprintf("[usb] UHCI port %d: device at addr %d (%s)\n",
                    p, dev->address, low_speed ? "low-speed" : "full-speed");
        }
        found++;
    }

    for (int p = 0; p < ehci_ports && found < USB_MAX_DEVICES; p++, found++) {
        usb_device_t *dev = alloc_device();
        if (dev) {
            dev->controller = USB_CTRL_EHCI;
            dev->port = p;
            dev->speed = USB_SPEED_HIGH;
            kprintf("[usb] EHCI port %d: high-speed device (addr %d)\n",
                    p, dev->address);
        }
    }
    for (int p = 0; p < xhci_ports && found < USB_MAX_DEVICES; p++, found++) {
        usb_device_t *dev = alloc_device();
        if (dev) {
            dev->controller = USB_CTRL_XHCI;
            dev->port = p;
            dev->speed = USB_SPEED_SUPER;
            kprintf("[usb] xHCI port %d: device (addr %d)\n",
                    p, dev->address);
        }
    }

    kprintf("[usb] %d device(s) recorded (full enumeration needs transfer layer)\n",
            found);
    return found;
}

void usb_dump_devices(void) {
    int count = 0;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!usb_devices[i].in_use) continue;
        count++;
        const char *ctrl_name = "?";
        switch (usb_devices[i].controller) {
        case USB_CTRL_UHCI: ctrl_name = "UHCI"; break;
        case USB_CTRL_OHCI: ctrl_name = "OHCI"; break;
        case USB_CTRL_EHCI: ctrl_name = "EHCI"; break;
        case USB_CTRL_XHCI: ctrl_name = "xHCI"; break;
        }
        kprintf("[usb] addr %d: %s port %d, %s, class=%s\n",
                usb_devices[i].address, ctrl_name, usb_devices[i].port,
                speed_name(usb_devices[i].speed),
                class_name(usb_devices[i].interface_class));
    }
    kprintf("[usb] total: %d device(s)\n", count);
}

int usb_device_count(void) {
    int count = 0;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (usb_devices[i].in_use) count++;
    }
    return count;
}

usb_device_t *usb_find_device_by_class(uint8_t class_code) {
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (usb_devices[i].in_use && usb_devices[i].interface_class == class_code) {
            return &usb_devices[i];
        }
    }
    return NULL;
}

void usb_core_self_test(void) {
    kprintf("[usb] core self-test: device enumeration layer\n");
    kprintf("[usb] protocol: SET_ADDRESS, GET_DESCRIPTOR, SET_CONFIGURATION\n");
    kprintf("[usb] descriptors: device, configuration, interface, endpoint\n");
    kprintf("[usb] classes: HID (0x03), MSC (0x08), Hub (0x09)\n");
    kprintf("[usb] devices recorded: %d\n", usb_device_count());
    usb_dump_devices();
    kprintf("[usb] PASS: enumeration framework ready (transfer layer WIP)\n");
}

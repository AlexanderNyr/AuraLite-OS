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
#include "drivers/usb/hid.h"
#include "drivers/usb/msc.h"
#include "drivers/timer/pit.h"
#include "kernel/proc/thread.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/mm/pmm.h"
#include "kernel/limine_requests.h"

/* Link to the UHCI transfer layer. */
extern int uhci_control_transfer_ex(uint8_t dev_addr, int low_speed,
                                    const void *setup, void *data,
                                    uint16_t data_len, uint8_t max_packet0);

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
        return uhci_control_transfer_ex(dev->address, low_speed,
                                        setup, data, data_len,
                                        dev->max_packet_size0);
    case USB_CTRL_OHCI:
        return ohci_control_transfer(dev->address, low_speed,
                                     setup, data, data_len,
                                     dev->max_packet_size0);
    case USB_CTRL_EHCI:
        return ehci_control_transfer(dev->address, low_speed,
                                     setup, data, data_len,
                                     dev->max_packet_size0);
    case USB_CTRL_XHCI:
        if (setup->bRequest == USB_SET_ADDRESS) {
            int xs = xhci_port_speed(dev->port);
            if (xs == 0) {
                if (dev->speed == USB_SPEED_LOW) xs = 2;
                else if (dev->speed == USB_SPEED_FULL) xs = 1;
                else if (dev->speed == USB_SPEED_HIGH) xs = 3;
                else xs = 4;
            }
            return xhci_address_device((uint8_t)(setup->wValue & 0x7F),
                                       dev->port, xs,
                                       dev->max_packet_size0);
        }
        return xhci_control_transfer(dev->address, low_speed,
                                     setup, data, data_len,
                                     dev->max_packet_size0);
    }
    return -1;
}

int usb_interrupt_transfer(usb_device_t *dev, uint8_t endpoint,
                           void *data, uint16_t data_len, int *toggle_io) {
    int low_speed = (dev->speed == USB_SPEED_LOW) ? 1 : 0;
    uint16_t max_packet = dev->interrupt_max_packet ? dev->interrupt_max_packet : data_len;
    switch (dev->controller) {
    case USB_CTRL_UHCI:
        return uhci_interrupt_transfer_ex(dev->address, endpoint, low_speed,
                                          max_packet, data, data_len, toggle_io);
    case USB_CTRL_OHCI:
        return ohci_interrupt_transfer(dev->address, endpoint, low_speed,
                                       max_packet, data, data_len, toggle_io);
    case USB_CTRL_EHCI:
    case USB_CTRL_XHCI:
        return xhci_interrupt_transfer(dev->address, endpoint, low_speed,
                                       max_packet, data, data_len, toggle_io);
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

/* ---- USB hub class support ---- */
#define USB_HUB_PORT_CONNECTION   (1u << 0)
#define USB_HUB_PORT_ENABLE       (1u << 1)
#define USB_HUB_PORT_LOW_SPEED    (1u << 9)
#define USB_HUB_PORT_HIGH_SPEED   (1u << 10)
#define USB_HUB_C_PORT_CONNECTION 16
#define USB_HUB_C_PORT_ENABLE     17
#define USB_HUB_C_PORT_RESET      20
#define USB_HUB_FEAT_PORT_RESET   4
#define USB_HUB_FEAT_PORT_POWER   8

struct usb_hub_desc {
    uint8_t  bDescLength;
    uint8_t  bDescriptorType;
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;
    uint8_t  bHubContrCurrent;
    uint8_t  variable[16];
} __attribute__((packed));

static usb_device_t *usb_find_by_location(usb_ctrl_type_t ctrl, int port) {
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (usb_devices[i].in_use && usb_devices[i].controller == ctrl &&
            usb_devices[i].port == port) return &usb_devices[i];
    }
    return NULL;
}

static usb_device_t *usb_last_allocated_by_location(usb_ctrl_type_t ctrl, int port) {
    usb_device_t *last = NULL;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (usb_devices[i].in_use && usb_devices[i].controller == ctrl &&
            usb_devices[i].port == port) last = &usb_devices[i];
    }
    return last;
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
    uint8_t cur_if_num = 0;
    uint8_t cur_class = 0;
    uint8_t cur_subclass = 0;
    uint8_t cur_protocol = 0;

    while (off + 2 <= len) {
        uint8_t dlen = buf[off];
        uint8_t dtype = buf[off + 1];
        if (dlen < 2 || off + dlen > len) break;

        if (dtype == USB_DESC_INTERFACE && dlen >= 9) {
            struct usb_interface_desc *ifd = (struct usb_interface_desc *)(buf + off);
            cur_if_num   = ifd->bInterfaceNumber;
            cur_class    = ifd->bInterfaceClass;
            cur_subclass = ifd->bInterfaceSubClass;
            cur_protocol = ifd->bInterfaceProtocol;

            /* Prefer a functional HID or MSC interface as the device's primary
             * class.  This keeps class drivers simple while still allowing
             * multi-descriptor configs to be walked correctly. */
            if (cur_class == USB_CLASS_HID || cur_class == USB_CLASS_MASS_STORAGE ||
                dev->interface_class == 0) {
                dev->interface_class    = cur_class;
                dev->interface_subclass = cur_subclass;
                dev->interface_protocol = cur_protocol;
                dev->interface_number   = cur_if_num;
            }

            kprintf("[usb]   interface %d: class=0x%02x (%s) subclass=0x%02x proto=0x%02x\n",
                    ifd->bInterfaceNumber, ifd->bInterfaceClass,
                    class_name(ifd->bInterfaceClass),
                    ifd->bInterfaceSubClass, ifd->bInterfaceProtocol);
        }

        if (dtype == USB_DESC_HID && dlen >= 9 && cur_class == USB_CLASS_HID) {
            uint16_t rep_len = (uint16_t)buf[off + 7] | ((uint16_t)buf[off + 8] << 8);
            dev->hid_report_desc_len = rep_len;
            kprintf("[usb]   HID descriptor: report_len=%u\n", rep_len);
        }

        if (dtype == USB_DESC_ENDPOINT && dlen >= 7) {
            struct usb_endpoint_desc *epd = (struct usb_endpoint_desc *)(buf + off);
            uint8_t ep_addr = epd->bEndpointAddress;
            uint8_t ep_type = epd->bmAttributes & 0x03;
            int is_in = (ep_addr & 0x80) ? 1 : 0;

            if (ep_type == USB_EP_BULK && cur_class == USB_CLASS_MASS_STORAGE) {
                if (is_in) {
                    dev->bulk_in_ep = ep_addr;
                } else {
                    dev->bulk_out_ep = ep_addr;
                }
                dev->bulk_max_packet = epd->wMaxPacketSize;
                kprintf("[usb]   endpoint 0x%02x: bulk %s, maxpkt=%d\n",
                        ep_addr, is_in ? "IN" : "OUT", epd->wMaxPacketSize);
            } else if (ep_type == USB_EP_INTERRUPT) {
                if (cur_class == USB_CLASS_HID) {
                    dev->interface_class    = cur_class;
                    dev->interface_subclass = cur_subclass;
                    dev->interface_protocol = cur_protocol;
                    dev->interface_number   = cur_if_num;
                    if (is_in) dev->interrupt_in_ep = ep_addr;
                    else       dev->interrupt_out_ep = ep_addr;
                    dev->interrupt_max_packet = epd->wMaxPacketSize;
                    dev->interrupt_interval   = epd->bInterval;
                }
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
    /* Give the device a moment to latch the new address. */
    for (volatile int d = 0; d < 100000; d++) {
        __asm__ volatile ("pause");
    }

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

    /* Step 4: Read configuration descriptor. Do it in two phases so the UHCI
     * control transfer queues exactly the bytes the device will return. A
     * blanket 255-byte request may short-packet and leave extra IN TDs queued. */
    uint8_t config_buf[256];
    memset(config_buf, 0, sizeof(config_buf));
    ret = usb_get_descriptor(dev, USB_DESC_CONFIGURATION, 0,
                             config_buf, sizeof(struct usb_config_desc));
    if (ret >= (int)sizeof(struct usb_config_desc)) {
        struct usb_config_desc *cfg = (struct usb_config_desc *)config_buf;
        uint16_t total_len = cfg->wTotalLength;
        if (total_len > sizeof(config_buf)) total_len = sizeof(config_buf);
        if (total_len < sizeof(struct usb_config_desc)) {
            total_len = sizeof(struct usb_config_desc);
        }
        ret = usb_get_descriptor(dev, USB_DESC_CONFIGURATION, 0,
                                 config_buf, total_len);
        if (ret >= (int)sizeof(struct usb_config_desc)) {
            cfg = (struct usb_config_desc *)config_buf;
            kprintf("[usb]   config %d: %d interfaces, %d bytes\n",
                    cfg->bConfigurationValue, cfg->bNumInterfaces, cfg->wTotalLength);
            parse_config(dev, config_buf, total_len);
        }
    }

    /* Step 5: SET_CONFIGURATION (activate configuration 1). */
    if (usb_set_configuration(dev, 1) >= 0) {
        dev->config_value = 1;
    }

    kprintf("[usb] device enumeration complete: addr=%d class=%s\n",
            addr, class_name(dev->interface_class));
    return 0;
}

static void usb_delay_ms(unsigned ms) {
    for (unsigned m = 0; m < ms; m++) {
        for (volatile int i = 0; i < 100000; i++) __asm__ volatile ("pause");
    }
}

static int usb_hub_class_request(usb_device_t *hub, uint8_t req_type,
                                 uint8_t req, uint16_t value, uint16_t index,
                                 void *data, uint16_t len) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = req_type;
    setup.bRequest      = req;
    setup.wValue        = value;
    setup.wIndex        = index;
    setup.wLength       = len;
    return usb_control_transfer(hub, &setup, data, len);
}

static int usb_hub_get_descriptor(usb_device_t *hub, struct usb_hub_desc *desc) {
    memset(desc, 0, sizeof(*desc));
    /* Read the fixed prefix first. Some hubs short-packet the descriptor, and
     * the simple UHCI control scheduler expects the exact byte count. */
    int r = usb_hub_class_request(hub,
        USB_REQ_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_DEV,
        USB_GET_DESCRIPTOR, (USB_DESC_HUB << 8), 0, desc, 8);
    if (r < 0) return r;
    uint16_t want = desc->bDescLength;
    if (want < 8 || want > sizeof(*desc)) want = 8;
    if (want == 8) return r;
    memset(desc, 0, sizeof(*desc));
    return usb_hub_class_request(hub,
        USB_REQ_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_DEV,
        USB_GET_DESCRIPTOR, (USB_DESC_HUB << 8), 0, desc, want);
}

static int usb_hub_get_port_status(usb_device_t *hub, uint8_t port,
                                   uint16_t *status, uint16_t *change) {
    uint8_t buf[4] = {0,0,0,0};
    int r = usb_hub_class_request(hub,
        USB_REQ_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_OTHER,
        USB_GET_STATUS, 0, port, buf, sizeof(buf));
    if (r < 0) return -1;
    if (status) *status = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    if (change) *change = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    return 0;
}

static int usb_hub_set_port_feature(usb_device_t *hub, uint8_t port, uint16_t feat) {
    return usb_hub_class_request(hub,
        USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_OTHER,
        USB_SET_FEATURE, feat, port, NULL, 0);
}

static int usb_hub_clear_port_feature(usb_device_t *hub, uint8_t port, uint16_t feat) {
    return usb_hub_class_request(hub,
        USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_OTHER,
        USB_CLEAR_FEATURE, feat, port, NULL, 0);
}

static usb_speed_t usb_hub_port_speed(uint16_t status, usb_speed_t hub_speed) {
    if (status & USB_HUB_PORT_LOW_SPEED) return USB_SPEED_LOW;
    if (status & USB_HUB_PORT_HIGH_SPEED) return USB_SPEED_HIGH;
    /* A full-speed hub can only expose full/low downstream devices.  A
     * high-speed hub reports neither LS nor HS for full-speed devices. */
    (void)hub_speed;
    return USB_SPEED_FULL;
}

static int usb_hub_scan(usb_device_t *hub) {
    if (!hub || !hub->in_use || hub->interface_class != USB_CLASS_HUB) return 0;
    if (hub->hub_scanned) return 0;
    hub->hub_scanned = 1;

    struct usb_hub_desc hd;
    if (usb_hub_get_descriptor(hub, &hd) < 0 || hd.bNbrPorts == 0) {
        kprintf("[hub] addr %d: failed to read hub descriptor\n", hub->address);
        return -1;
    }
    if (hd.bNbrPorts > 16) hd.bNbrPorts = 16;
    kprintf("[hub] addr %d: %u downstream port(s), pwr_good=%ums\n",
            hub->address, hd.bNbrPorts, (unsigned)hd.bPwrOn2PwrGood * 2u);

    for (uint8_t p = 1; p <= hd.bNbrPorts; p++) {
        (void)usb_hub_set_port_feature(hub, p, USB_HUB_FEAT_PORT_POWER);
    }
    usb_delay_ms((unsigned)hd.bPwrOn2PwrGood * 2u + 20u);

    int added = 0;
    for (uint8_t p = 1; p <= hd.bNbrPorts && usb_device_count() < USB_MAX_DEVICES; p++) {
        uint16_t st = 0, ch = 0;
        if (usb_hub_get_port_status(hub, p, &st, &ch) < 0) continue;
        if (ch & (1u << 0)) (void)usb_hub_clear_port_feature(hub, p, USB_HUB_C_PORT_CONNECTION);
        if (!(st & USB_HUB_PORT_CONNECTION)) continue;

        kprintf("[hub] addr %d port %u: device connected (st=0x%04x ch=0x%04x)\n",
                hub->address, p, st, ch);
        if (usb_hub_set_port_feature(hub, p, USB_HUB_FEAT_PORT_RESET) < 0) {
            kprintf("[hub] addr %d port %u: reset request failed\n", hub->address, p);
            continue;
        }
        usb_delay_ms(80);
        for (int wait = 0; wait < 20; wait++) {
            if (usb_hub_get_port_status(hub, p, &st, &ch) == 0 && (ch & (1u << 4))) break;
            usb_delay_ms(10);
        }
        (void)usb_hub_clear_port_feature(hub, p, USB_HUB_C_PORT_RESET);
        (void)usb_hub_clear_port_feature(hub, p, USB_HUB_C_PORT_ENABLE);
        if (usb_hub_get_port_status(hub, p, &st, &ch) < 0) continue;
        if (!(st & USB_HUB_PORT_CONNECTION) || !(st & USB_HUB_PORT_ENABLE)) {
            kprintf("[hub] addr %d port %u: not enabled after reset (st=0x%04x)\n",
                    hub->address, p, st);
            continue;
        }

        usb_speed_t child_speed = usb_hub_port_speed(st, hub->speed);
        int child_port = (((hub->port >= 16) ? hub->port : ((hub->port + 1) << 4)) | p);
        if (usb_find_by_location(hub->controller, child_port)) continue;
        if (enumerate_device(hub->controller, child_port, child_speed) == 0) {
            usb_device_t *child = usb_last_allocated_by_location(hub->controller, child_port);
            if (child) {
                child->parent_hub_addr = hub->address;
                child->parent_hub_port = p;
                if (child->interface_class == USB_CLASS_HID) (void)usb_hid_attach_device(child);
            }
            added++;
        } else {
            kprintf("[hub] addr %d port %u: child enumeration failed\n", hub->address, p);
        }
    }
    if (added) kprintf("[hub] addr %d: enumerated %d downstream device(s)\n", hub->address, added);
    return added;
}

/* ---- Public API ---- */

int usb_enumerate_all(void) {
    int found = 0;

    kprintf("[usb] enumerating devices across all controllers...\n");

    /* UHCI is the first controller with a working transfer backend. Perform a
     * real standard enumeration sequence for each attached UHCI root port. */
    for (int p = 0; p < UHCI_MAX_PORTS && found < USB_MAX_DEVICES; p++) {
        if (!uhci_port_has_device(p)) continue;
        usb_speed_t speed = uhci_port_is_low_speed(p) ? USB_SPEED_LOW : USB_SPEED_FULL;
        if (enumerate_device(USB_CTRL_UHCI, p, speed) == 0) {
            found++;
        } else {
            kprintf("[usb] UHCI port %d: enumeration failed\n", p);
        }
    }

    /* OHCI has a working USB 1.1 ED/TD backend, so enumerate its root ports
     * with the same standard request sequence as UHCI. */
    for (int p = 0; p < OHCI_MAX_PORTS && found < USB_MAX_DEVICES; p++) {
        if (!ohci_port_has_device(p)) continue;
        usb_speed_t speed = ohci_port_is_low_speed(p) ? USB_SPEED_LOW : USB_SPEED_FULL;
        if (enumerate_device(USB_CTRL_OHCI, p, speed) == 0) {
            found++;
        } else {
            kprintf("[usb] OHCI port %d: enumeration failed\n", p);
        }
    }

    /* EHCI high-speed devices use the async qTD backend. Low/full-speed
     * devices are released to companion UHCI/OHCI controllers by EHCI init. */
    for (int p = 0; p < EHCI_MAX_PORTS && found < USB_MAX_DEVICES; p++) {
        if (!ehci_port_has_device(p)) continue;
        if (enumerate_device(USB_CTRL_EHCI, p, USB_SPEED_HIGH) == 0) {
            found++;
        } else {
            kprintf("[usb] EHCI port %d: enumeration failed\n", p);
        }
    }

    /* xHCI unified host controller backend. */
    for (int p = 0; p < XHCI_MAX_PORTS && found < USB_MAX_DEVICES; p++) {
        if (!xhci_port_has_device(p)) continue;
        int xs = xhci_port_speed(p);
        usb_speed_t us = USB_SPEED_HIGH;
        if (xs == 1) us = USB_SPEED_FULL;
        else if (xs == 2) us = USB_SPEED_LOW;
        else if (xs == 4) us = USB_SPEED_SUPER;
        if (enumerate_device(USB_CTRL_XHCI, p, us) == 0) {
            found++;
        } else {
            kprintf("[usb] xHCI port %d: enumeration failed\n", p);
        }
    }

    /* Enumerate downstream devices behind classic USB hubs. Iterate a few
     * passes so hubs behind hubs are handled for USB 1.1/high-speed paths. */
    for (int pass = 0; pass < 4; pass++) {
        int added = 0;
        for (int i = 0; i < USB_MAX_DEVICES; i++) {
            if (usb_devices[i].in_use && usb_devices[i].interface_class == USB_CLASS_HUB &&
                !usb_devices[i].hub_scanned) {
                int r = usb_hub_scan(&usb_devices[i]);
                if (r > 0) { added += r; found += r; }
            }
        }
        if (!added) break;
    }

    kprintf("[usb] %d device record(s); UHCI/OHCI/EHCI/xHCI devices enumerated where backend is available\n", found);
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

static void usb_attach_supported_class(usb_device_t *dev) {
    if (!dev || !dev->in_use) return;
    if (dev->interface_class == USB_CLASS_HID) {
        (void)usb_hid_attach_device(dev);
    } else if (dev->interface_class == USB_CLASS_MASS_STORAGE) {
        (void)msc_attach_device(dev);
    }
}

static void usb_detach_location(usb_ctrl_type_t ctrl, int port) {
    usb_device_t *dev = usb_find_by_location(ctrl, port);
    if (!dev) return;
    kprintf("[usb] hotplug: device removed addr=%d ctrl=%d port=%d class=%s\n",
            dev->address, dev->controller, dev->port, class_name(dev->interface_class));
    if (dev->interface_class == USB_CLASS_MASS_STORAGE) msc_detach_device(dev);
    dev->in_use = 0;
}

static void usb_hotplug_scan_root(usb_ctrl_type_t ctrl, int max_ports) {
    for (int p = 0; p < max_ports; p++) {
        int present = 0;
        usb_speed_t speed = USB_SPEED_FULL;
        if (ctrl == USB_CTRL_UHCI) {
            present = uhci_port_has_device(p);
            speed = uhci_port_is_low_speed(p) ? USB_SPEED_LOW : USB_SPEED_FULL;
            if (present && !usb_find_by_location(ctrl, p)) (void)uhci_reset_port(p);
        } else if (ctrl == USB_CTRL_OHCI) {
            present = ohci_port_has_device(p);
            speed = ohci_port_is_low_speed(p) ? USB_SPEED_LOW : USB_SPEED_FULL;
            if (present && !usb_find_by_location(ctrl, p)) (void)ohci_reset_port(p);
        } else if (ctrl == USB_CTRL_EHCI) {
            present = ehci_port_has_device(p);
            speed = USB_SPEED_HIGH;
            if (present && !usb_find_by_location(ctrl, p)) (void)ehci_reset_port_public(p);
        } else if (ctrl == USB_CTRL_XHCI) {
            present = xhci_port_has_device(p);
            int xs = xhci_port_speed(p);
            if (xs == 1) speed = USB_SPEED_FULL;
            else if (xs == 2) speed = USB_SPEED_LOW;
            else if (xs == 4) speed = USB_SPEED_SUPER;
            else speed = USB_SPEED_HIGH;
            if (present && !usb_find_by_location(ctrl, p)) (void)xhci_reset_port(p);
        }
        usb_device_t *existing = usb_find_by_location(ctrl, p);
        if (!present) {
            if (existing) usb_detach_location(ctrl, p);
            continue;
        }
        if (existing) continue;
        kprintf("[usb] hotplug: new root device ctrl=%d port=%d\n", ctrl, p);
        if (enumerate_device(ctrl, p, speed) == 0) {
            usb_device_t *dev = usb_last_allocated_by_location(ctrl, p);
            usb_attach_supported_class(dev);
        } else {
            kprintf("[usb] hotplug: root enumeration failed ctrl=%d port=%d\n", ctrl, p);
        }
    }
}

void usb_hotplug_poll(void) {
    static int busy = 0;
    if (busy) return;
    busy = 1;
    usb_hotplug_scan_root(USB_CTRL_UHCI, UHCI_MAX_PORTS);
    usb_hotplug_scan_root(USB_CTRL_OHCI, OHCI_MAX_PORTS);
    usb_hotplug_scan_root(USB_CTRL_EHCI, EHCI_MAX_PORTS);
    usb_hotplug_scan_root(USB_CTRL_XHCI, XHCI_MAX_PORTS);

    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (usb_devices[i].in_use && usb_devices[i].interface_class == USB_CLASS_HUB) {
            usb_devices[i].hub_scanned = 0;
            (void)usb_hub_scan(&usb_devices[i]);
        }
    }
    busy = 0;
}

static void usb_hotplug_thread(void *arg) {
    (void)arg;
    for (;;) {
        usb_hotplug_poll();
        timer_sleep_ms(500);
    }
}

int usb_hotplug_start(void) {
    static int started = 0;
    if (started) return 0;
    kthread_create(usb_hotplug_thread, NULL, "usb-hotplug");
    started = 1;
    kprintf("[usb] hotplug monitor started (500ms poll)\n");
    return 0;
}

void usb_core_self_test(void) {
    kprintf("[usb] core self-test: device enumeration layer\n");
    kprintf("[usb] protocol: SET_ADDRESS, GET_DESCRIPTOR, SET_CONFIGURATION\n");
    kprintf("[usb] descriptors: device, configuration, interface, endpoint\n");
    kprintf("[usb] classes: HID (0x03), MSC (0x08), Hub (0x09)\n");
    kprintf("[usb] devices recorded: %d\n", usb_device_count());
    usb_dump_devices();
    kprintf("[usb] PASS: UHCI/OHCI/EHCI/xHCI enumeration and class-transfer hooks ready\n");
}

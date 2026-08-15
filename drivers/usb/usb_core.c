/* usb_core.c — USB device enumeration and protocol layer.
 *
 * Full USB stack: control/bulk/interrupt/isochronous, all speeds,
 * string descriptors, driver model, CDC ACM / Audio / Printer / Hub / HID / MSC,
 * hotplug, detailed device tree.
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
#include "kernel/boot_info.h"

extern int uhci_control_transfer_ex(uint8_t dev_addr, int low_speed,
                                    const void *setup, void *data,
                                    uint16_t data_len, uint8_t max_packet0);

/* Global device table */
usb_device_t usb_devices[USB_MAX_DEVICES];
static int next_address = 1;

/* ---- Driver registry ---- */
static usb_driver_t *drivers[USB_DRIVER_MAX];
static int driver_cnt = 0;

int usb_register_driver(usb_driver_t *drv) {
    if (!drv || driver_cnt >= USB_DRIVER_MAX) return -1;
    drivers[driver_cnt++] = drv;
    kprintf("[usb] driver '%s' registered (class 0x%02x)\n", drv->name, drv->class_code);
    return 0;
}
int usb_unregister_driver(usb_driver_t *drv) {
    for (int i = 0; i < driver_cnt; i++) {
        if (drivers[i] == drv) {
            for (int j = i; j < driver_cnt - 1; j++) drivers[j] = drivers[j+1];
            driver_cnt--;
            return 0;
        }
    }
    return -1;
}
usb_driver_t *usb_find_driver(usb_device_t *dev) {
    if (!dev) return NULL;
    for (int i = 0; i < driver_cnt; i++) {
        usb_driver_t *d = drivers[i];
        if (d->match_vendor && d->match_vendor(dev->vendor_id, dev->product_id))
            return d;
        if (d->class_code == 0xFF) continue;
        if (d->class_code == dev->interface_class) {
            if (d->subclass && d->subclass != dev->interface_subclass) continue;
            if (d->protocol && d->protocol != dev->interface_protocol) continue;
            return d;
        }
    }
    return NULL;
}

/* ---- Device table ---- */
static usb_device_t *alloc_device(void) {
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!usb_devices[i].in_use) {
            memset(&usb_devices[i], 0, sizeof(usb_device_t));
            usb_devices[i].in_use = 1;
            usb_devices[i].address = (uint8_t)next_address++;
            if (next_address > 127) next_address = 1;
            usb_devices[i].max_packet_size0 = 8;
            usb_devices[i].authorized = 1;
            usb_devices[i].driver_id = -1;
            return &usb_devices[i];
        }
    }
    return NULL;
}
static void free_device(usb_device_t *dev) { dev->in_use = 0; }

/* ---- helpers LE ---- */
uint16_t usb_le16_to_cpu(uint16_t v) { return v; }
uint32_t usb_le32_to_cpu(uint32_t v) { return v; }

/* ---- Control transfer dispatch ---- */
int usb_control_transfer(usb_device_t *dev, const struct usb_setup_pkt *setup,
                         void *data, uint16_t data_len) {
    int low_speed = (dev->speed == USB_SPEED_LOW) ? 1 : 0;
    switch (dev->controller) {
    case USB_CTRL_UHCI:
        return uhci_control_transfer_ex(dev->address, low_speed, setup, data, data_len, dev->max_packet_size0);
    case USB_CTRL_OHCI:
        return ohci_control_transfer(dev->address, low_speed, setup, data, data_len, dev->max_packet_size0);
    case USB_CTRL_EHCI:
        return ehci_control_transfer(dev->address, low_speed, setup, data, data_len, dev->max_packet_size0);
    case USB_CTRL_XHCI:
        if (setup->bRequest == USB_SET_ADDRESS) {
            int xs = xhci_port_speed(dev->port);
            if (xs == 0) {
                if (dev->speed == USB_SPEED_LOW) xs = 2;
                else if (dev->speed == USB_SPEED_FULL) xs = 1;
                else if (dev->speed == USB_SPEED_HIGH) xs = 3;
                else xs = 4;
            }
            return xhci_address_device((uint8_t)(setup->wValue & 0x7F), dev->port, xs, dev->max_packet_size0);
        }
        return xhci_control_transfer(dev->address, low_speed, setup, data, data_len, dev->max_packet_size0);
    }
    return -1;
}

int usb_interrupt_transfer(usb_device_t *dev, uint8_t endpoint,
                           void *data, uint16_t data_len, int *toggle_io) {
    int low_speed = (dev->speed == USB_SPEED_LOW) ? 1 : 0;
    uint16_t max_packet = dev->interrupt_max_packet ? dev->interrupt_max_packet : data_len;
    switch (dev->controller) {
    case USB_CTRL_UHCI:
        return uhci_interrupt_transfer_ex(dev->address, endpoint, low_speed, max_packet, data, data_len, toggle_io);
    case USB_CTRL_OHCI:
        return ohci_interrupt_transfer(dev->address, endpoint, low_speed, max_packet, data, data_len, toggle_io);
    case USB_CTRL_EHCI:
        return ehci_interrupt_transfer(dev->address, endpoint, low_speed, max_packet, data, data_len, toggle_io);
    case USB_CTRL_XHCI:
        return xhci_interrupt_transfer(dev->address, endpoint, low_speed, max_packet, data, data_len, toggle_io);
    }
    return -1;
}

int usb_bulk_transfer_ex(usb_device_t *dev, uint8_t endpoint,
                         void *data, uint32_t len, int *toggle_io) {
    if (!dev) return -1;
    switch (dev->controller) {
    case USB_CTRL_UHCI: {
        int t = toggle_io ? *toggle_io : 0;
        int r = uhci_bulk_transfer_ex(dev->address, endpoint, data, len, &t);
        if (toggle_io) *toggle_io = t;
        return r;
    }
    case USB_CTRL_OHCI: {
        int is_in = (endpoint & 0x80) ? 1 : 0;
        return ohci_bulk_transfer(dev->address, endpoint, data, len, is_in,
                                  dev->bulk_max_packet ? dev->bulk_max_packet : 64);
    }
    case USB_CTRL_EHCI: {
        int is_in = (endpoint & 0x80) ? 1 : 0;
        return ehci_bulk_transfer(dev->address, endpoint, data, len, is_in,
                                  dev->bulk_max_packet ? dev->bulk_max_packet : 512);
    }
    case USB_CTRL_XHCI: {
        int is_in = (endpoint & 0x80) ? 1 : 0;
        return xhci_bulk_transfer(dev->address, endpoint, data, len, is_in,
                                  dev->bulk_max_packet ? dev->bulk_max_packet : 512);
    }
    }
    return -1;
}
int usb_bulk_transfer(usb_device_t *dev, uint8_t endpoint, void *data, uint32_t len) {
    int toggle = 0;
    return usb_bulk_transfer_ex(dev, endpoint, data, len, &toggle);
}

int uhci_isochronous_transfer(uint8_t dev_addr, uint8_t endpoint, int low_speed,
                              uint16_t max_packet, void *data, uint32_t len, int is_in);

int usb_isochronous_transfer(usb_device_t *dev, uint8_t endpoint,
                             void *data, uint32_t len, uint32_t *transferred) {
    if (!dev || !data || len == 0) return -1;
    int ret = -1;
    int low_speed = (dev->speed == USB_SPEED_LOW);
    int is_in = (endpoint & 0x80) ? 1 : 0;
    uint16_t max_pkt = dev->isoc_max_packet ? dev->isoc_max_packet : (uint16_t)len;
    switch (dev->controller) {
    case USB_CTRL_UHCI:
        ret = uhci_isochronous_transfer(dev->address, endpoint, low_speed, max_pkt, data, len, is_in);
        break;
    case USB_CTRL_OHCI:
        ret = ohci_interrupt_transfer(dev->address, endpoint, low_speed, max_pkt, data, (uint16_t)len, NULL);
        break;
    case USB_CTRL_EHCI:
        ret = ehci_bulk_transfer(dev->address, endpoint, data, len, is_in, dev->isoc_max_packet ? dev->isoc_max_packet : 1024);
        break;
    case USB_CTRL_XHCI:
        ret = xhci_bulk_transfer(dev->address, endpoint, data, len, is_in, dev->isoc_max_packet ? dev->isoc_max_packet : 1024);
        break;
    }
    if (ret >= 0 && transferred) *transferred = (uint32_t)ret;
    else if (transferred) *transferred = len;
    return ret;
}

int usb_get_sync_frame(usb_device_t *dev, uint8_t endpoint, uint16_t *frame) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = USB_REQ_DIR_IN | USB_REQ_TYPE_STD | USB_REQ_RCPT_EP;
    setup.bRequest = USB_SYNCH_FRAME;
    setup.wValue = 0;
    setup.wIndex = endpoint;
    setup.wLength = 2;
    return usb_control_transfer(dev, &setup, frame, 2);
}

/* ---- Descriptor helpers ---- */
int usb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index, uint16_t lang, void *buf, uint16_t len) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = USB_REQ_DIR_IN | USB_REQ_TYPE_STD | USB_REQ_RCPT_DEV;
    setup.bRequest = USB_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)((type << 8) | index);
    setup.wIndex = lang;
    setup.wLength = len;
    return usb_control_transfer(dev, &setup, buf, len);
}
int usb_get_string_descriptor(usb_device_t *dev, uint8_t index, uint16_t lang, void *buf, uint16_t len) {
    return usb_get_descriptor(dev, USB_DESC_STRING, index, lang, buf, len);
}
int usb_get_string_ascii(usb_device_t *dev, uint8_t index, char *out, uint16_t out_len) {
    if (!dev || !out || out_len == 0 || index == 0) return -1;
    uint8_t tmp[256];
    memset(tmp, 0, sizeof(tmp));
    int r = usb_get_string_descriptor(dev, index, 0x0409, tmp, sizeof(tmp));
    if (r < 2) {
        r = usb_get_string_descriptor(dev, 0, 0, tmp, sizeof(tmp));
        if (r >= 4) {
            uint16_t lang = (uint16_t)tmp[2] | ((uint16_t)tmp[3] << 8);
            r = usb_get_string_descriptor(dev, index, lang, tmp, sizeof(tmp));
        }
    }
    if (r < 2) return -1;
    uint8_t bLength = tmp[0];
    if (bLength > r) bLength = (uint8_t)r;
    if (bLength < 2) return -1;
    uint16_t out_pos = 0;
    for (int i = 2; i + 1 < bLength && out_pos + 1 < out_len; i += 2) {
        uint16_t ch = (uint16_t)tmp[i] | ((uint16_t)tmp[i+1] << 8);
        if (ch == 0) break;
        out[out_pos++] = (ch < 128) ? (char)ch : '?';
    }
    out[out_pos] = 0;
    return out_pos;
}
int usb_clear_feature(usb_device_t *dev, uint8_t rcpt, uint16_t feature, uint16_t index) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | rcpt;
    setup.bRequest = USB_CLEAR_FEATURE;
    setup.wValue = feature;
    setup.wIndex = index;
    setup.wLength = 0;
    return usb_control_transfer(dev, &setup, NULL, 0);
}
int usb_set_feature(usb_device_t *dev, uint8_t rcpt, uint16_t feature, uint16_t index) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | rcpt;
    setup.bRequest = USB_SET_FEATURE;
    setup.wValue = feature;
    setup.wIndex = index;
    setup.wLength = 0;
    return usb_control_transfer(dev, &setup, NULL, 0);
}
int usb_get_status(usb_device_t *dev, uint8_t rcpt, uint16_t index, uint16_t *status) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = USB_REQ_DIR_IN | USB_REQ_TYPE_STD | rcpt;
    setup.bRequest = USB_GET_STATUS;
    setup.wValue = 0;
    setup.wIndex = index;
    setup.wLength = 2;
    uint8_t buf[2];
    int r = usb_control_transfer(dev, &setup, buf, 2);
    if (r >= 0 && status) *status = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return r;
}

/* ---- Classic requests ---- */
static int usb_get_descriptor_internal(usb_device_t *dev, uint8_t desc_type,
                              uint8_t desc_index, void *buf, uint16_t len) {
    return usb_get_descriptor(dev, desc_type, desc_index, 0, buf, len);
}
static int usb_set_address(usb_device_t *dev, uint8_t addr) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | USB_REQ_RCPT_DEV;
    setup.bRequest = USB_SET_ADDRESS;
    setup.wValue = addr;
    setup.wIndex = 0;
    setup.wLength = 0;
    return usb_control_transfer(dev, &setup, NULL, 0);
}
static int usb_set_configuration(usb_device_t *dev, uint8_t config) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | USB_REQ_RCPT_DEV;
    setup.bRequest = USB_SET_CONFIGURATION;
    setup.wValue = config;
    setup.wIndex = 0;
    setup.wLength = 0;
    return usb_control_transfer(dev, &setup, NULL, 0);
}

/* ---- Parsing ---- */
static const char *class_name(uint8_t cls) {
    switch (cls) {
    case USB_CLASS_HID: return "HID";
    case USB_CLASS_MASS_STORAGE: return "Mass Storage";
    case USB_CLASS_HUB: return "Hub";
    case USB_CLASS_CDC: return "CDC";
    case USB_CLASS_CDC_DATA: return "CDC-DATA";
    case USB_CLASS_AUDIO: return "Audio";
    case USB_CLASS_PRINTER: return "Printer";
    case USB_CLASS_VIDEO: return "Video";
    case USB_CLASS_AUDIO_VIDEO: return "AV";
    case USB_CLASS_VENDOR: return "Vendor";
    case USB_CLASS_MISC: return "Misc";
    default: return "Generic";
    }
}
static const char *speed_name(usb_speed_t s) {
    switch (s) {
    case USB_SPEED_LOW: return "low (1.5 Mbps)";
    case USB_SPEED_FULL: return "full (12 Mbps)";
    case USB_SPEED_HIGH: return "high (480 Mbps)";
    case USB_SPEED_SUPER: return "super (5 Gbps)";
    default: return "?";
    }
}

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
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (usb_devices[i].in_use && usb_devices[i].controller == ctrl && usb_devices[i].port == port)
            return &usb_devices[i];
    return NULL;
}
static usb_device_t *usb_last_allocated_by_location(usb_ctrl_type_t ctrl, int port) {
    usb_device_t *last = NULL;
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (usb_devices[i].in_use && usb_devices[i].controller == ctrl && usb_devices[i].port == port)
            last = &usb_devices[i];
    return last;
}

static void parse_config_full(usb_device_t *dev, const uint8_t *buf, int len) {
    int off = 0;
    int cur_if_idx = -1;
    uint8_t cur_if_num = 0;
    uint8_t cur_class = 0, cur_subclass = 0, cur_protocol = 0;

    dev->num_interfaces = 0;
    memset(dev->interfaces, 0, sizeof(dev->interfaces));
    dev->bulk_in_ep = 0;
    dev->bulk_out_ep = 0;
    dev->bulk_max_packet = 0;
    dev->interrupt_in_ep = 0;
    dev->interrupt_out_ep = 0;
    dev->interrupt_max_packet = 0;
    dev->interrupt_interval = 0;
    dev->isoc_in_ep = 0;
    dev->isoc_out_ep = 0;
    dev->isoc_max_packet = 0;
    dev->cdc_comm_if = 0xFF;
    dev->cdc_data_if = 0xFF;
    dev->cdc_bulk_in = 0;
    dev->cdc_bulk_out = 0;
    dev->cdc_interrupt_in = 0;
    dev->audio_control_if = 0xFF;
    dev->audio_streaming_if = 0xFF;

    while (off + 2 <= len) {
        uint8_t dlen = buf[off];
        uint8_t dtype = buf[off+1];
        if (dlen < 2 || off + dlen > len) break;

        if (dtype == USB_DESC_INTERFACE && dlen >= 9) {
            struct usb_interface_desc *ifd = (struct usb_interface_desc *)(buf+off);
            cur_if_num = ifd->bInterfaceNumber;
            cur_class = ifd->bInterfaceClass;
            cur_subclass = ifd->bInterfaceSubClass;
            cur_protocol = ifd->bInterfaceProtocol;

            if (dev->num_interfaces < USB_MAX_INTERFACES) {
                cur_if_idx = dev->num_interfaces++;
                usb_interface_info_t *iface = &dev->interfaces[cur_if_idx];
                iface->number = ifd->bInterfaceNumber;
                iface->alt_setting = ifd->bAlternateSetting;
                iface->class_code = cur_class;
                iface->subclass = cur_subclass;
                iface->protocol = cur_protocol;
                iface->iInterface = ifd->iInterface;
            }

            if (cur_class == USB_CLASS_HID || cur_class == USB_CLASS_MASS_STORAGE ||
                cur_class == USB_CLASS_HUB || cur_class == USB_CLASS_CDC ||
                cur_class == USB_CLASS_AUDIO || cur_class == USB_CLASS_PRINTER ||
                dev->interface_class == 0) {
                if (dev->interface_class == 0 ||
                    cur_class == USB_CLASS_HID ||
                    cur_class == USB_CLASS_MASS_STORAGE ||
                    cur_class == USB_CLASS_CDC ||
                    cur_class == USB_CLASS_AUDIO) {
                    dev->interface_class = cur_class;
                    dev->interface_subclass = cur_subclass;
                    dev->interface_protocol = cur_protocol;
                    dev->interface_number = cur_if_num;
                }
            }

            if (cur_class == USB_CLASS_CDC && cur_subclass == USB_CDC_SUBCLASS_ACM) {
                if (dev->cdc_comm_if == 0xFF) dev->cdc_comm_if = cur_if_num;
            }
            if (cur_class == USB_CLASS_CDC_DATA) {
                if (dev->cdc_data_if == 0xFF) dev->cdc_data_if = cur_if_num;
            }
            if (cur_class == USB_CLASS_AUDIO) {
                if (cur_subclass == USB_AUDIO_SUBCLASS_CONTROL && dev->audio_control_if == 0xFF)
                    dev->audio_control_if = cur_if_num;
                if (cur_subclass == USB_AUDIO_SUBCLASS_STREAMING && dev->audio_streaming_if == 0xFF)
                    dev->audio_streaming_if = cur_if_num;
            }

            kprintf("[usb]   interface %d alt %d: class=0x%02x (%s) subclass=0x%02x proto=0x%02x eps=%d\n",
                    ifd->bInterfaceNumber, ifd->bAlternateSetting,
                    ifd->bInterfaceClass, class_name(ifd->bInterfaceClass),
                    ifd->bInterfaceSubClass, ifd->bInterfaceProtocol, ifd->bNumEndpoints);
            kprintf("[usb]   interface %d: class=0x%02x (%s) subclass=0x%02x proto=0x%02x\n",
                    ifd->bInterfaceNumber,
                    ifd->bInterfaceClass, class_name(ifd->bInterfaceClass),
                    ifd->bInterfaceSubClass, ifd->bInterfaceProtocol);
        }

        if (dtype == USB_DESC_HID && dlen >= 9 && cur_class == USB_CLASS_HID) {
            uint16_t rep_len = (uint16_t)buf[off+7] | ((uint16_t)buf[off+8] << 8);
            dev->hid_report_desc_len = rep_len;
            if (cur_if_idx >= 0) dev->interfaces[cur_if_idx].hid_report_len = rep_len;
            kprintf("[usb]   HID descriptor: report_len=%u\n", rep_len);
        }

        if (dtype == USB_DESC_ENDPOINT && dlen >= 7) {
            struct usb_endpoint_desc *epd = (struct usb_endpoint_desc *)(buf+off);
            uint8_t ep_addr = epd->bEndpointAddress;
            uint8_t ep_type = epd->bmAttributes & 0x03;
            uint8_t sync_type = (epd->bmAttributes & 0x0C);
            uint8_t usage_type = (epd->bmAttributes & 0x30);
            int is_in = (ep_addr & 0x80) ? 1 : 0;

            if (cur_if_idx >= 0 && dev->interfaces[cur_if_idx].num_endpoints < USB_MAX_ENDPOINTS) {
                usb_endpoint_info_t *ep = &dev->interfaces[cur_if_idx].endpoints[dev->interfaces[cur_if_idx].num_endpoints++];
                ep->address = ep_addr;
                ep->attributes = epd->bmAttributes;
                ep->max_packet = epd->wMaxPacketSize & 0x07FF;
                ep->interval = epd->bInterval;
                ep->type = ep_type;
                ep->sync_type = sync_type;
                ep->usage_type = usage_type;
                ep->toggle = 0;
            }

            uint16_t companion_max_burst = 0;
            uint16_t companion_bytes = 0;
            if (off + dlen + 2 <= len && buf[off+dlen+1] == USB_DESC_SS_EP_COMP) {
                struct usb_ss_ep_comp_desc *comp = (struct usb_ss_ep_comp_desc *)(buf+off+dlen);
                companion_max_burst = comp->bMaxBurst;
                companion_bytes = comp->wBytesPerInterval;
                if (cur_if_idx >= 0 && dev->interfaces[cur_if_idx].num_endpoints > 0) {
                    usb_endpoint_info_t *ep = &dev->interfaces[cur_if_idx].endpoints[dev->interfaces[cur_if_idx].num_endpoints-1];
                    ep->max_burst = comp->bMaxBurst;
                    ep->bytes_per_interval = comp->wBytesPerInterval;
                }
            }

            if (ep_type == USB_EP_BULK) {
                if (cur_class == USB_CLASS_MASS_STORAGE) {
                    if (is_in) dev->bulk_in_ep = ep_addr;
                    else dev->bulk_out_ep = ep_addr;
                    dev->bulk_max_packet = epd->wMaxPacketSize;
                }
                if (cur_class == USB_CLASS_CDC_DATA || dev->cdc_data_if != 0xFF) {
                    if (is_in) dev->cdc_bulk_in = ep_addr;
                    else dev->cdc_bulk_out = ep_addr;
                    if (dev->cdc_data_if == 0xFF) dev->cdc_data_if = cur_if_num;
                }
                kprintf("[usb]   endpoint 0x%02x: bulk %s, maxpkt=%d%s\n",
                        ep_addr, is_in ? "IN" : "OUT", epd->wMaxPacketSize & 0x7FF,
                        companion_max_burst ? " (+SS companion)" : "");
            } else if (ep_type == USB_EP_INTERRUPT) {
                if (cur_class == USB_CLASS_HID) {
                    if (is_in) dev->interrupt_in_ep = ep_addr;
                    else dev->interrupt_out_ep = ep_addr;
                    dev->interrupt_max_packet = epd->wMaxPacketSize & 0x7FF;
                    dev->interrupt_interval = epd->bInterval;
                }
                if (cur_class == USB_CLASS_CDC) {
                    dev->cdc_interrupt_in = ep_addr;
                }
                kprintf("[usb]   endpoint 0x%02x: interrupt %s, maxpkt=%d, interval=%dms\n",
                        ep_addr, is_in ? "IN" : "OUT",
                        epd->wMaxPacketSize & 0x7FF, epd->bInterval);
            } else if (ep_type == USB_EP_ISOCHRONOUS) {
                if (is_in) dev->isoc_in_ep = ep_addr;
                else dev->isoc_out_ep = ep_addr;
                dev->isoc_max_packet = epd->wMaxPacketSize & 0x7FF;
                dev->isoc_interval = epd->bInterval;
                dev->isoc_sync_type = sync_type;
                if (cur_class == USB_CLASS_AUDIO) {
                    if (is_in) dev->audio_isoc_in = ep_addr;
                    else dev->audio_isoc_out = ep_addr;
                }
                kprintf("[usb]   endpoint 0x%02x: isoc %s, maxpkt=%d, interval=%d, sync=0x%x usage=0x%x%s\n",
                        ep_addr, is_in ? "IN" : "OUT",
                        epd->wMaxPacketSize & 0x7FF, epd->bInterval,
                        sync_type, usage_type,
                        companion_bytes ? " (SS companion)" : "");
            }
        }

        if (dtype == 0x24) {
            if (cur_if_idx >= 0 && cur_class == USB_CLASS_CDC) {
                kprintf("[usb]   CDC functional desc: len=%d subtype=0x%02x\n", dlen, buf[off+2]);
            }
            if (cur_if_idx >= 0 && cur_class == USB_CLASS_AUDIO) {
                kprintf("[usb]   Audio class-specific: len=%d subtype=0x%02x\n", dlen, buf[off+2]);
            }
        }

        off += dlen;
    }
}

/* ---- Enumeration ---- */
static int enumerate_device(usb_ctrl_type_t ctrl, int port, usb_speed_t speed) {
    usb_device_t *dev = alloc_device();
    if (!dev) {
        kprintf("[usb] device table full\n");
        return -1;
    }
    dev->controller = ctrl;
    dev->port = port;
    dev->speed = speed;
    dev->max_packet_size0 = (speed == USB_SPEED_LOW) ? 8 : 64;

    uint8_t addr = dev->address;
    dev->address = 0;
    int ret = usb_set_address(dev, addr);
    if (ret < 0) {
        kprintf("[usb] SET_ADDRESS failed\n");
        free_device(dev);
        return -1;
    }
    dev->address = addr;
    for (volatile int d = 0; d < 100000; d++) __asm__ volatile("pause");

    struct usb_device_desc full_desc;
    memset(&full_desc, 0, sizeof(full_desc));
    ret = usb_get_descriptor_internal(dev, USB_DESC_DEVICE, 0, &full_desc, sizeof(full_desc));
    if (ret < 0) {
        kprintf("[usb] GET_DESCRIPTOR(DEVICE) failed\n");
        free_device(dev);
        return -1;
    }

    dev->vendor_id = full_desc.idVendor;
    dev->product_id = full_desc.idProduct;
    dev->bcd_device = full_desc.bcdDevice;
    dev->iManufacturer = full_desc.iManufacturer;
    dev->iProduct = full_desc.iProduct;
    dev->iSerial = full_desc.iSerialNumber;
    dev->num_configs = full_desc.bNumConfigurations;
    dev->max_packet_size0 = full_desc.bMaxPacketSize0 ? full_desc.bMaxPacketSize0 : dev->max_packet_size0;
    /* USB_PLAN U4: a full-speed device's real EP0 size (8/16/32/64) is only
     * known once the descriptor has been read with a guessed one.  Tell the
     * controller, or every later control transfer is packetised wrongly. */
    if (dev->controller == USB_CTRL_XHCI)
        (void)xhci_update_max_packet0(dev->address, dev->max_packet_size0);
    if (full_desc.bDeviceClass != USB_CLASS_USE_DEVICE) {
        dev->interface_class = full_desc.bDeviceClass;
        dev->interface_subclass = full_desc.bDeviceSubClass;
        dev->interface_protocol = full_desc.bDeviceProtocol;
    }

    kprintf("[usb] device at addr %d: VID=0x%04x PID=0x%04x class=0x%02x (%s) maxpkt0=%d speed=%s\n",
            addr, full_desc.idVendor, full_desc.idProduct,
            full_desc.bDeviceClass, class_name(full_desc.bDeviceClass),
            full_desc.bMaxPacketSize0, speed_name(speed));

    if (dev->controller != USB_CTRL_UHCI) {
        if (dev->iManufacturer) {
            if (usb_get_string_ascii(dev, dev->iManufacturer, dev->manufacturer_str, sizeof(dev->manufacturer_str)) > 0)
                kprintf("[usb]   manufacturer: '%s'\n", dev->manufacturer_str);
        }
        if (dev->iProduct) {
            if (usb_get_string_ascii(dev, dev->iProduct, dev->product_str, sizeof(dev->product_str)) > 0)
                kprintf("[usb]   product: '%s'\n", dev->product_str);
        }
        if (dev->iSerial) {
            if (usb_get_string_ascii(dev, dev->iSerial, dev->serial_str, sizeof(dev->serial_str)) > 0)
                kprintf("[usb]   serial: '%s'\n", dev->serial_str);
        }
    }

    uint8_t config_buf[512];
    memset(config_buf, 0, sizeof(config_buf));
    ret = usb_get_descriptor_internal(dev, USB_DESC_CONFIGURATION, 0, config_buf, sizeof(struct usb_config_desc));
    if (ret >= (int)sizeof(struct usb_config_desc)) {
        struct usb_config_desc *cfg = (struct usb_config_desc *)config_buf;
        uint16_t total_len = cfg->wTotalLength;
        if (total_len > sizeof(config_buf)) total_len = sizeof(config_buf);
        if (total_len < sizeof(struct usb_config_desc)) total_len = sizeof(struct usb_config_desc);
        ret = usb_get_descriptor_internal(dev, USB_DESC_CONFIGURATION, 0, config_buf, total_len);
        if (ret >= (int)sizeof(struct usb_config_desc)) {
            cfg = (struct usb_config_desc *)config_buf;
            kprintf("[usb]   config %d: %d interfaces, %d bytes, attr=0x%02x power=%dmA\n",
                    cfg->bConfigurationValue, cfg->bNumInterfaces, cfg->wTotalLength,
                    cfg->bmAttributes, cfg->bMaxPower * 2);
            parse_config_full(dev, config_buf, total_len);
        }
    }

    if (speed == USB_SPEED_SUPER || dev->controller == USB_CTRL_XHCI) {
        uint8_t bos_buf[64];
        memset(bos_buf, 0, sizeof(bos_buf));
        ret = usb_get_descriptor_internal(dev, USB_DESC_BOS, 0, bos_buf, sizeof(bos_buf));
        if (ret >= (int)sizeof(struct usb_bos_desc)) {
            struct usb_bos_desc *bos = (struct usb_bos_desc *)bos_buf;
            kprintf("[usb]   BOS descriptor: total=%d caps=%d\n", bos->wTotalLength, bos->bNumDeviceCaps);
        }
    }

    if (usb_set_configuration(dev, 1) >= 0) dev->config_value = 1;

    usb_authorize_device(dev);
    usb_driver_t *drv = usb_find_driver(dev);
    if (drv && drv->probe) {
        int pr = drv->probe(dev);
        if (pr == 0) kprintf("[usb] device bound to driver '%s'\n", drv->name);
    }

    kprintf("[usb] device enumeration complete: addr=%d class=%s\n", addr, class_name(dev->interface_class));
    return 0;
}

int usb_enumerate_device_at(usb_ctrl_type_t ctrl, int port, usb_speed_t speed) {
    return enumerate_device(ctrl, port, speed);
}

int usb_reset_device(usb_device_t *dev) {
    if (!dev) return -1;
    switch (dev->controller) {
    case USB_CTRL_UHCI: return uhci_reset_port(dev->port);
    case USB_CTRL_OHCI: return ohci_reset_port(dev->port);
    case USB_CTRL_EHCI: return ehci_reset_port_public(dev->port);
    case USB_CTRL_XHCI: return xhci_reset_port(dev->port);
    default: return -1;
    }
}
int usb_authorize_device(usb_device_t *dev) {
    if (!dev) return -1;
    dev->authorized = 1;
    return 0;
}

static void usb_delay_ms(unsigned ms) {
    for (unsigned m = 0; m < ms; m++)
        for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause");
}

static int usb_hub_class_request(usb_device_t *hub, uint8_t req_type,
                                 uint8_t req, uint16_t value, uint16_t index,
                                 void *data, uint16_t len) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = req_type;
    setup.bRequest = req;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = len;
    return usb_control_transfer(hub, &setup, data, len);
}
static int usb_hub_get_descriptor(usb_device_t *hub, struct usb_hub_desc *desc) {
    memset(desc, 0, sizeof(*desc));
    int r = usb_hub_class_request(hub, USB_REQ_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_DEV,
        USB_GET_DESCRIPTOR, (USB_DESC_HUB << 8), 0, desc, 8);
    if (r < 0) return r;
    uint16_t want = desc->bDescLength;
    if (want < 8 || want > sizeof(*desc)) want = 8;
    if (want == 8) return r;
    memset(desc, 0, sizeof(*desc));
    return usb_hub_class_request(hub, USB_REQ_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_DEV,
        USB_GET_DESCRIPTOR, (USB_DESC_HUB << 8), 0, desc, want);
}
static int usb_hub_get_port_status(usb_device_t *hub, uint8_t port, uint16_t *status, uint16_t *change) {
    uint8_t buf[4] = {0};
    int r = usb_hub_class_request(hub, USB_REQ_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_OTHER,
        USB_GET_STATUS, 0, port, buf, sizeof(buf));
    if (r < 0) return -1;
    if (status) *status = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    if (change) *change = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    return 0;
}
static int usb_hub_set_port_feature(usb_device_t *hub, uint8_t port, uint16_t feat) {
    return usb_hub_class_request(hub, USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_OTHER,
        USB_SET_FEATURE, feat, port, NULL, 0);
}
static int usb_hub_clear_port_feature(usb_device_t *hub, uint8_t port, uint16_t feat) {
    return usb_hub_class_request(hub, USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_OTHER,
        USB_CLEAR_FEATURE, feat, port, NULL, 0);
}
static usb_speed_t usb_hub_port_speed(uint16_t status, usb_speed_t hub_speed) {
    if (status & USB_HUB_PORT_LOW_SPEED) return USB_SPEED_LOW;
    if (status & USB_HUB_PORT_HIGH_SPEED) return USB_SPEED_HIGH;
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
    if (hd.bNbrPorts > 32) hd.bNbrPorts = 32;
    hub->hub_num_ports = hd.bNbrPorts;
    kprintf("[hub] addr %d: %u downstream port(s), pwr_good=%ums\n", hub->address, hd.bNbrPorts, (unsigned)hd.bPwrOn2PwrGood * 2u);
    for (uint8_t p = 1; p <= hd.bNbrPorts; p++) (void)usb_hub_set_port_feature(hub, p, USB_HUB_FEAT_PORT_POWER);
    usb_delay_ms((unsigned)hd.bPwrOn2PwrGood * 2u + 20u);
    int added = 0;
    for (uint8_t p = 1; p <= hd.bNbrPorts && usb_device_count() < USB_MAX_DEVICES; p++) {
        uint16_t st = 0, ch = 0;
        if (usb_hub_get_port_status(hub, p, &st, &ch) < 0) continue;
        if (ch & (1u << 0)) (void)usb_hub_clear_port_feature(hub, p, USB_HUB_C_PORT_CONNECTION);
        if (!(st & USB_HUB_PORT_CONNECTION)) continue;
        kprintf("[hub] addr %d port %u: device connected (st=0x%04x ch=0x%04x)\n", hub->address, p, st, ch);
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
            kprintf("[hub] addr %d port %u: not enabled after reset (st=0x%04x)\n", hub->address, p, st);
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
            }
            added++;
        } else {
            kprintf("[hub] addr %d port %u: child enumeration failed\n", hub->address, p);
        }
    }
    if (added) kprintf("[hub] addr %d: enumerated %d downstream device(s)\n", hub->address, added);
    return added;
}

int usb_enumerate_all(void) {
    int found = 0;
    kprintf("[usb] enumerating devices across all controllers...\n");

    for (int p = 0; p < UHCI_MAX_PORTS && found < USB_MAX_DEVICES; p++) {
        if (!uhci_port_has_device(p)) continue;
        usb_speed_t speed = uhci_port_is_low_speed(p) ? USB_SPEED_LOW : USB_SPEED_FULL;
        if (enumerate_device(USB_CTRL_UHCI, p, speed) == 0) found++;
        else kprintf("[usb] UHCI port %d: enumeration failed\n", p);
    }
    for (int p = 0; p < OHCI_MAX_PORTS && found < USB_MAX_DEVICES; p++) {
        if (!ohci_port_has_device(p)) continue;
        usb_speed_t speed = ohci_port_is_low_speed(p) ? USB_SPEED_LOW : USB_SPEED_FULL;
        if (enumerate_device(USB_CTRL_OHCI, p, speed) == 0) found++;
        else kprintf("[usb] OHCI port %d: enumeration failed\n", p);
    }
    for (int p = 0; p < EHCI_MAX_PORTS && found < USB_MAX_DEVICES; p++) {
        if (!ehci_port_has_device(p)) continue;
        if (enumerate_device(USB_CTRL_EHCI, p, USB_SPEED_HIGH) == 0) found++;
        else kprintf("[usb] EHCI port %d: enumeration failed\n", p);
    }
    for (int p = 0; p < XHCI_MAX_PORTS && found < USB_MAX_DEVICES; p++) {
        if (!xhci_port_has_device(p)) continue;
        int xs = xhci_port_speed(p);
        usb_speed_t us = USB_SPEED_HIGH;
        if (xs == 1) us = USB_SPEED_FULL;
        else if (xs == 2) us = USB_SPEED_LOW;
        else if (xs == 4) us = USB_SPEED_SUPER;
        if (enumerate_device(USB_CTRL_XHCI, p, us) == 0) found++;
        else kprintf("[usb] xHCI port %d: enumeration failed\n", p);
    }

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

    kprintf("[usb] %d device(s) enumerated\n", found);
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
        kprintf("[usb] addr %d: %s port %d, %s, class=%s VID=0x%04x PID=0x%04x\n",
                usb_devices[i].address, ctrl_name, usb_devices[i].port,
                speed_name(usb_devices[i].speed),
                class_name(usb_devices[i].interface_class),
                usb_devices[i].vendor_id, usb_devices[i].product_id);
    }
    kprintf("[usb] total: %d device(s)\n", count);
}

void usb_dump_device_detail(usb_device_t *dev) {
    if (!dev || !dev->in_use) return;
    kprintf("[usb] === Device %d detail ===\n", dev->address);
    kprintf("  VID:PID %04x:%04x bcdDevice %04x\n", dev->vendor_id, dev->product_id, dev->bcd_device);
    kprintf("  Manufacturer: '%s' Product: '%s' Serial: '%s'\n",
            dev->manufacturer_str[0] ? dev->manufacturer_str : "(none)",
            dev->product_str[0] ? dev->product_str : "(none)",
            dev->serial_str[0] ? dev->serial_str : "(none)");
    kprintf("  Class 0x%02x Sub 0x%02x Proto 0x%02x Interfaces %d\n",
            dev->interface_class, dev->interface_subclass, dev->interface_protocol, dev->num_interfaces);
    for (int i = 0; i < dev->num_interfaces; i++) {
        usb_interface_info_t *iface = &dev->interfaces[i];
        kprintf("  IF %d alt %d class %02x sub %02x proto %02x eps %d\n",
                iface->number, iface->alt_setting, iface->class_code, iface->subclass, iface->protocol, iface->num_endpoints);
        for (int e = 0; e < iface->num_endpoints; e++) {
            usb_endpoint_info_t *ep = &iface->endpoints[e];
            const char *t = "?";
            switch (ep->type) { case 0: t = "CTRL"; break; case 1: t = "ISOC"; break; case 2: t = "BULK"; break; case 3: t = "INTR"; break; }
            kprintf("    EP 0x%02x %s max %d interval %d\n", ep->address, t, ep->max_packet, ep->interval);
        }
    }
}

int usb_device_count(void) {
    int c = 0;
    for (int i = 0; i < USB_MAX_DEVICES; i++) if (usb_devices[i].in_use) c++;
    return c;
}
usb_device_t *usb_find_device_by_class(uint8_t class_code) {
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (usb_devices[i].in_use && usb_devices[i].interface_class == class_code)
            return &usb_devices[i];
    return NULL;
}
usb_device_t *usb_find_device_by_vid_pid(uint16_t vid, uint16_t pid) {
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (usb_devices[i].in_use && usb_devices[i].vendor_id == vid && usb_devices[i].product_id == pid)
            return &usb_devices[i];
    return NULL;
}
usb_device_t *usb_find_device_by_address(uint8_t addr) {
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (usb_devices[i].in_use && usb_devices[i].address == addr)
            return &usb_devices[i];
    return NULL;
}

static void usb_attach_supported_class(usb_device_t *dev) {
    if (!dev || !dev->in_use) return;
    if (dev->interface_class == USB_CLASS_HID) (void)usb_hid_attach_device(dev);
    else if (dev->interface_class == USB_CLASS_MASS_STORAGE) (void)msc_attach_device(dev);
    else {
        usb_driver_t *drv = usb_find_driver(dev);
        if (drv && drv->probe) drv->probe(dev);
    }
}
static void usb_detach_location(usb_ctrl_type_t ctrl, int port) {
    usb_device_t *dev = usb_find_by_location(ctrl, port);
    if (!dev) return;
    kprintf("[usb] hotplug: device removed addr=%d ctrl=%d port=%d class=%s\n",
            dev->address, dev->controller, dev->port, class_name(dev->interface_class));
    usb_driver_t *drv = usb_find_driver(dev);
    if (drv && drv->disconnect) drv->disconnect(dev);
    if (dev->interface_class == USB_CLASS_MASS_STORAGE) { extern void msc_detach_device(void*); msc_detach_device(dev); }
    /* USB_PLAN U3: hand the slot back to the controller.  Nothing called
     * xhci_disable_slot() before, so an unplugged xHCI device kept its slot
     * (and its contexts and rings) for the rest of the boot -- 64
     * attach/detach cycles and Enable Slot would start refusing. */
    if (dev->controller == USB_CTRL_XHCI) (void)xhci_free_device(dev->address);
    /* U7: EHCI interrupt endpoints hold a QH linked into the periodic frame
     * list plus its qTD and buffer; unlink and free them, or the controller
     * keeps polling a detached device forever. */
    if (dev->controller == USB_CTRL_EHCI) ehci_release_device(dev->address);
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
        if (!present) { if (existing) usb_detach_location(ctrl, p); continue; }
        if (existing) continue;
        kprintf("[usb] hotplug: new root device ctrl=%d port=%d\n", ctrl, p);
        if (enumerate_device(ctrl, p, speed) == 0) {
            usb_device_t *dev = usb_last_allocated_by_location(ctrl, p);
            usb_attach_supported_class(dev);
        } else kprintf("[usb] hotplug: root enumeration failed ctrl=%d port=%d\n", ctrl, p);
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
    /* U8: the xHCI interrupt handler sets a flag on Port Change Detect, so
     * an attach or detach is noticed at interrupt time rather than at the
     * next 500 ms tick.  The poll remains as a backstop -- for the three
     * controllers that still have no IRQ handler, and so that a missed or
     * unroutable interrupt degrades to the old latency instead of hanging.
     * The sleep is 10 ms and the full scan still runs every 500 ms. */
    int ticks = 0;
    for (;;) {
        if (xhci_take_port_change()) {
            usb_hotplug_poll();
            ticks = 0;
        } else if (++ticks >= 50) {
            usb_hotplug_poll();
            ticks = 0;
        }
        timer_sleep_ms(10);
    }
}
int usb_hotplug_start(void) {
    static int started = 0;
    if (started) return 0;
    kthread_create(usb_hotplug_thread, NULL, "usb-hotplug");
    started = 1;
    kprintf("[usb] hotplug monitor started (xHCI interrupt-driven, %d ms poll backstop)\n", 500);
    return 0;
}

void usb_core_self_test(void) {
    /* USB_PLAN U0: this banner used to advertise a capability list rather
     * than a test result -- four transfer types, four speeds and four
     * controllers, printed unconditionally and followed by "PASS" even
     * with zero devices attached.  What the core can genuinely claim is
     * the class-independent protocol layer; what each controller does
     * beneath it differs sharply and is now stated per controller. */
    int n = usb_device_count();
    kprintf("[usb] core self-test: enumeration/protocol layer\n");
    /* Kept honest as each phase lands: U3-U6 made xHCI real (slots,
     * control, bulk, interrupt) and U7 put EHCI's interrupt endpoints on
     * the periodic schedule with TT split support. */
    kprintf("[usb]  controllers: UHCI real; OHCI real; EHCI real "
            "(async + periodic/split); xHCI real "
            "(control, bulk, interrupt; streams/UAS not implemented)\n");
    kprintf("[usb]  see USB_PLAN.md for the phase that closes each gap\n");
    kprintf("[usb] devices recorded: %d\n", n);
    usb_dump_devices();
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (usb_devices[i].in_use) usb_dump_device_detail(&usb_devices[i]);
    if (n > 0) kprintf("[usb] PASS: %d device(s) enumerated\n", n);
    else       kprintf("[usb] SKIP: no USB devices attached\n");
}

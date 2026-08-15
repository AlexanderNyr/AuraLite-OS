/* usb_hub.c — Full USB Hub driver */

#include "drivers/usb/usb_hub.h"
#include "drivers/usb/usb_core.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "drivers/timer/pit.h"

usb_hub_t usb_hubs[USB_HUB_MAX_HUBS];

static int hub_class_request(usb_device_t *hub, uint8_t req_type, uint8_t req,
                             uint16_t value, uint16_t index, void *data, uint16_t len) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = req_type;
    setup.bRequest = req;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = len;
    return usb_control_transfer(hub, &setup, data, len);
}
static int hub_get_status(usb_device_t *hub, uint8_t port, uint16_t *status, uint16_t *change) {
    uint8_t buf[4] = {0};
    int r = hub_class_request(hub, 0xA3, 0, 0, port, buf, 4);
    if (r < 0) return -1;
    if (status) *status = buf[0] | (buf[1] << 8);
    if (change) *change = buf[2] | (buf[3] << 8);
    return 0;
}
static int hub_set_port_feature(usb_device_t *hub, uint8_t port, uint16_t feat) {
    return hub_class_request(hub, 0x23, 3, feat, port, NULL, 0);
}
static int hub_clear_port_feature(usb_device_t *hub, uint8_t port, uint16_t feat) {
    return hub_class_request(hub, 0x23, 1, feat, port, NULL, 0);
}

static usb_hub_t *alloc_hub(void) {
    for (int i = 0; i < USB_HUB_MAX_HUBS; i++) if (!usb_hubs[i].in_use) {
        memset(&usb_hubs[i], 0, sizeof(usb_hub_t));
        usb_hubs[i].in_use = 1;
        return &usb_hubs[i];
    }
    return NULL;
}

static int usb_hub_probe_wrapper(usb_device_t *dev);
static void usb_hub_disconnect_wrapper(usb_device_t *dev);
static usb_driver_t usb_hub_driver = {
    .name = "usb_hub",
    .probe = usb_hub_probe_wrapper,
    .disconnect = usb_hub_disconnect_wrapper,
    .class_code = USB_CLASS_HUB,
    .subclass = 0,
    .protocol = 0,
    .match_vendor = NULL,
};

int usb_hub_attach_device(usb_device_t *dev) {
    if (!dev || dev->interface_class != USB_CLASS_HUB) return -1;
    usb_hub_t *h = alloc_hub();
    if (!h) { kprintf("[hub] no free hub slots\n"); return -1; }
    h->dev = dev;
    h->depth = 0;
    usb_device_t *cur = dev;
    int depth = 0;
    while (cur->parent_hub_addr && depth < USB_HUB_MAX_DEPTH) {
        usb_device_t *parent = usb_find_device_by_address(cur->parent_hub_addr);
        if (!parent) break;
        depth++;
        cur = parent;
    }
    h->depth = depth;
    struct usb_hub_descriptor_full desc;
    memset(&desc, 0, sizeof(desc));
    int r = hub_class_request(dev, 0xA0, 6, (USB_DESC_HUB << 8), 0, &desc, 8);
    if (r < 0) { kprintf("[hub] addr %d: failed hub descriptor\n", dev->address); h->in_use = 0; return -1; }
    uint8_t want = desc.bDescLength;
    if (want > sizeof(desc)) want = sizeof(desc);
    if (want > 8) {
        memset(&desc, 0, sizeof(desc));
        r = hub_class_request(dev, 0xA0, 6, (USB_DESC_HUB << 8), 0, &desc, want);
        if (r < 0) { h->in_use = 0; return -1; }
    }
    h->num_ports = desc.bNbrPorts;
    if (h->num_ports > USB_HUB_MAX_PORTS) h->num_ports = USB_HUB_MAX_PORTS;
    h->characteristics = desc.wHubCharacteristics;
    h->power_good_ms = desc.bPwrOn2PwrGood * 2;
    h->tt_think_time = (desc.wHubCharacteristics >> 5) & 0x03;
    dev->hub_num_ports = h->num_ports;
    dev->hub_tt_think_time = h->tt_think_time;
    kprintf("[hub] new hub addr=%d ports=%d pwr_good=%dms chars=0x%04x depth=%d TT=%d\n",
            dev->address, h->num_ports, h->power_good_ms, h->characteristics, h->depth, h->tt_think_time);
    for (int p = 1; p <= h->num_ports; p++) hub_set_port_feature(dev, p, 8);
    timer_sleep_ms(h->power_good_ms + 20);
    for (int i = 0; i < dev->num_interfaces; i++) {
        for (int e = 0; e < dev->interfaces[i].num_endpoints; e++) {
            if (dev->interfaces[i].endpoints[e].type == USB_EP_INTERRUPT &&
                (dev->interfaces[i].endpoints[e].address & 0x80)) {
                h->has_interrupt_ep = 1;
                h->interrupt_ep = dev->interfaces[i].endpoints[e].address;
                h->max_packet = dev->interfaces[i].endpoints[e].max_packet;
            }
        }
    }
    kprintf("[hub] hub addr=%d ready — interrupt EP 0x%02x present=%d\n",
            dev->address, h->interrupt_ep, h->has_interrupt_ep);
    return 0;
}
void usb_hub_detach_device(usb_device_t *dev) {
    for (int i = 0; i < USB_HUB_MAX_HUBS; i++) if (usb_hubs[i].in_use && usb_hubs[i].dev == dev) {
        kprintf("[hub] detach hub addr=%d\n", dev->address);
        usb_hubs[i].in_use = 0;
        break;
    }
}
int usb_hub_power_on_port(usb_device_t *hub, uint8_t port) {
    return hub_set_port_feature(hub, port, 8);
}
int usb_hub_reset_port(usb_device_t *hub, uint8_t port) {
    if (hub_set_port_feature(hub, port, 4) < 0) return -1;
    timer_sleep_ms(80);
    for (int wait = 0; wait < 20; wait++) {
        uint16_t st, ch;
        if (hub_get_status(hub, port, &st, &ch) == 0 && (ch & (1 << 4))) break;
        timer_sleep_ms(10);
    }
    hub_clear_port_feature(hub, port, 20);
    hub_clear_port_feature(hub, port, 17);
    uint16_t st, ch;
    if (hub_get_status(hub, port, &st, &ch) < 0) return -1;
    return (st & 0x1) && (st & 0x2) ? 0 : -1;
}
int usb_hub_port_has_device(usb_device_t *hub, uint8_t port) {
    uint16_t st, ch;
    if (hub_get_status(hub, port, &st, &ch) < 0) return 0;
    return (st & 0x1) ? 1 : 0;
}
int usb_hub_scan_all(void) {
    int total = 0;
    for (int i = 0; i < USB_HUB_MAX_HUBS; i++) if (usb_hubs[i].in_use) {
        usb_device_t *hub = usb_hubs[i].dev;
        for (uint8_t p = 1; p <= usb_hubs[i].num_ports && usb_device_count() < USB_MAX_DEVICES; p++) {
            uint16_t st, ch;
            if (hub_get_status(hub, p, &st, &ch) < 0) continue;
            if (ch & 1) hub_clear_port_feature(hub, p, 16);
            if (!(st & 1)) continue;
            int child_port = usb_loc_child(hub->port, p);
            if (child_port < 0) {
                kprintf("[hub] addr %d port %u: deeper than the USB limit of %d "
                        "hub tiers; not enumerated\n",
                        hub->address, p, USB_LOC_DEPTH_MAX);
                continue;
            }
            usb_speed_t speed = USB_SPEED_FULL;
            if (st & (1<<9)) speed = USB_SPEED_LOW;
            else if (st & (1<<10)) speed = USB_SPEED_HIGH;
            if (usb_enumerate_device_at(hub->controller, child_port, speed) == 0) total++;
        }
    }
    return total;
}
static int usb_hub_probe_wrapper(usb_device_t *dev) { return usb_hub_attach_device(dev); }
static void usb_hub_disconnect_wrapper(usb_device_t *dev) { usb_hub_detach_device(dev); }

int usb_hub_init(void) {
    memset(usb_hubs, 0, sizeof(usb_hubs));
    kprintf("[hub] full hub driver initialized — max %d hubs, %d ports each, depth %d\n",
            USB_HUB_MAX_HUBS, USB_HUB_MAX_PORTS, USB_HUB_MAX_DEPTH);
    usb_register_driver(&usb_hub_driver);
    int found = 0;
    for (int i = 0; i < USB_MAX_DEVICES; i++) if (usb_devices[i].in_use && usb_devices[i].interface_class == USB_CLASS_HUB) {
        if (usb_hub_attach_device(&usb_devices[i]) == 0) found++;
    }
    kprintf("[hub] %d hub(s) attached at boot\n", found);
    return found;
}
int usb_hub_get_port_count(usb_device_t *hub) {
    for (int i = 0; i < USB_HUB_MAX_HUBS; i++) if (usb_hubs[i].in_use && usb_hubs[i].dev == hub) return usb_hubs[i].num_ports;
    return hub->hub_num_ports;
}
void usb_hub_self_test(void) {
    int active = 0;
    for (int i = 0; i < USB_HUB_MAX_HUBS; i++) if (usb_hubs[i].in_use) active++;
    kprintf("[hub] self-test: %d active hub(s)\n", active);
    for (int i = 0; i < USB_HUB_MAX_HUBS; i++) if (usb_hubs[i].in_use) {
        kprintf("[hub]   hub addr=%d ports=%d depth=%d\n", usb_hubs[i].dev->address, usb_hubs[i].num_ports, usb_hubs[i].depth);
    }
    if (active > 0) kprintf("[hub] PASS: %d hub(s) attached\n", active);
    else            kprintf("[hub] SKIP: no hubs attached\n");
}

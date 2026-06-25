/* hid.c — USB HID keyboard/mouse driver.
 *
 * Supports HID Boot Protocol keyboards/mice and a small generic HID report
 * parser for mouse/tablet-like pointer devices.  Decoded input is injected into
 * the existing keyboard/mouse queues so the shell and GUI use one input path.
 */

#include <stdint.h>
#include "drivers/usb/hid.h"
#include "drivers/usb/usb_core.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/mouse/mouse.h"
#include "drivers/framebuffer/graphics.h"
#include "drivers/timer/pit.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/proc/thread.h"

#define HID_SUBCLASS_BOOT       0x01
#define HID_PROTO_KEYBOARD      0x01
#define HID_PROTO_MOUSE         0x02

#define HID_REQ_SET_IDLE        0x0A
#define HID_REQ_SET_PROTOCOL    0x0B
#define HID_BOOT_PROTOCOL       0x00

#define HID_USAGE_PAGE_GENERIC  0x01
#define HID_USAGE_PAGE_BUTTON   0x09
#define HID_USAGE_PAGE_KEYBOARD 0x07
#define HID_USAGE_MOUSE         0x02
#define HID_USAGE_POINTER       0x01
#define HID_USAGE_KEYBOARD      0x06
#define HID_USAGE_X             0x30
#define HID_USAGE_Y             0x31
#define HID_USAGE_WHEEL         0x38

#define HID_FIELD_MAX           32
#define HID_REPORT_MAX          64
#define HID_REPORT_DESC_MAX     256

typedef struct {
    uint16_t usage_page;
    uint16_t usage;
    uint16_t usage_min;
    uint16_t usage_max;
    uint16_t bit_offset;
    uint8_t  size;
    int32_t  logical_min;
    int32_t  logical_max;
    uint8_t  flags;
} hid_field_t;

typedef struct {
    int is_mouse;
    int is_keyboard;
    int has_report_id;
    uint8_t report_id;
    uint16_t report_bits;
    hid_field_t fields[HID_FIELD_MAX];
    uint8_t field_count;
} hid_report_parser_t;

typedef struct {
    int          in_use;
    usb_device_t *dev;
    uint8_t      protocol;
    uint8_t      interface_number;
    uint8_t      ep_in;
    uint16_t     report_len;
    uint8_t      interval_ms;
    int          toggle;
    uint8_t      last_report[HID_REPORT_MAX];
    uint8_t      last_mods;
    uint8_t      last_keys[16];
    uint8_t      last_key_count;
    int          generic;  /* 0=boot, 1=generic pointer, 2=generic keyboard */
    hid_report_parser_t parser;
} hid_dev_t;

static hid_dev_t hid_devs[USB_HID_MAX_DEVICES];
static int hid_count = 0;
static int hid_thread_started = 0;

static int hid_class_request(usb_device_t *dev, uint8_t iface,
                             uint8_t request, uint16_t value) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_IF;
    setup.bRequest      = request;
    setup.wValue        = value;
    setup.wIndex        = iface;
    setup.wLength       = 0;
    return usb_control_transfer(dev, &setup, 0, 0);
}

static int hid_get_report_descriptor(usb_device_t *dev, uint8_t iface,
                                     void *buf, uint16_t len) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = USB_REQ_DIR_IN | USB_REQ_TYPE_STD | USB_REQ_RCPT_IF;
    setup.bRequest      = USB_GET_DESCRIPTOR;
    setup.wValue        = (uint16_t)(USB_DESC_REPORT << 8);
    setup.wIndex        = iface;
    setup.wLength       = len;
    return usb_control_transfer(dev, &setup, buf, len);
}

static int32_t sign_extend(uint32_t v, uint8_t bits) {
    if (bits == 0 || bits >= 32) return (int32_t)v;
    uint32_t sign = 1u << (bits - 1);
    if (v & sign) v |= (~0u << bits);
    return (int32_t)v;
}

static uint32_t get_bits(const uint8_t *buf, uint16_t bit, uint8_t size) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < size && i < 32; i++) {
        uint16_t b = bit + i;
        if (buf[b >> 3] & (1u << (b & 7))) v |= (1u << i);
    }
    return v;
}

static int32_t get_field_value(const uint8_t *buf, const hid_field_t *f) {
    uint32_t raw = get_bits(buf, f->bit_offset, f->size);
    if (f->logical_min < 0) return sign_extend(raw, f->size);
    return (int32_t)raw;
}

static int item_sdata(const uint8_t *p, int size) {
    if (size == 0) return 0;
    if (size == 1) return (int8_t)p[0];
    if (size == 2) return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}
static uint32_t item_udata(const uint8_t *p, int size) {
    if (size == 0) return 0;
    if (size == 1) return p[0];
    if (size == 2) return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int parse_hid_report(hid_report_parser_t *rp, const uint8_t *d, uint16_t len) {
    memset(rp, 0, sizeof(*rp));
    uint16_t usage_page = 0;
    int32_t logical_min = 0, logical_max = 1;
    uint8_t report_size = 0, report_count = 0, report_id = 0;
    uint16_t bitpos = 0;
    uint16_t usages[32];
    uint8_t usage_count = 0;
    uint16_t usage_min = 0, usage_max = 0;
    int have_range = 0;
    int mouse_collection = 0;

    for (uint16_t off = 0; off < len;) {
        uint8_t b = d[off++];
        if (b == 0xFE) { if (off + 2 > len) break; uint8_t n = d[off]; off += 2 + n; continue; }
        int sz = b & 3; if (sz == 3) sz = 4;
        int type = (b >> 2) & 3;
        int tag = (b >> 4) & 0xF;
        if (off + sz > len) break;
        const uint8_t *payload = d + off;
        uint32_t uval = item_udata(payload, sz);
        int32_t sval = item_sdata(payload, sz);
        off += sz;

        if (type == 1) { /* global */
            switch (tag) {
            case 0x0: usage_page = (uint16_t)uval; break;
            case 0x1: logical_min = sval; break;
            case 0x2: logical_max = sval; break;
            case 0x7: report_size = (uint8_t)uval; break;
            case 0x8:
                report_id = (uint8_t)uval;
                rp->has_report_id = 1;
                rp->report_id = report_id;
                bitpos = 8;
                break;
            case 0x9: report_count = (uint8_t)uval; break;
            default: break;
            }
        } else if (type == 2) { /* local */
            switch (tag) {
            case 0x0:
                if (usage_count < 32) usages[usage_count++] = (uint16_t)uval;
                if (usage_page == HID_USAGE_PAGE_GENERIC &&
                    (uval == HID_USAGE_MOUSE || uval == HID_USAGE_POINTER))
                    rp->is_mouse = 1;
                if (usage_page == HID_USAGE_PAGE_GENERIC && uval == HID_USAGE_KEYBOARD)
                    rp->is_keyboard = 1;
                break;
            case 0x1: usage_min = (uint16_t)uval; have_range = 1; break;
            case 0x2: usage_max = (uint16_t)uval; have_range = 1; break;
            default: break;
            }
        } else if (type == 0) { /* main */
            if (tag == 0xA) { /* Collection */
                if (usage_page == HID_USAGE_PAGE_GENERIC && usage_count &&
                    (usages[usage_count - 1] == HID_USAGE_MOUSE ||
                     usages[usage_count - 1] == HID_USAGE_POINTER))
                    mouse_collection++;
                usage_count = 0; have_range = 0;
            } else if (tag == 0xC) {
                if (mouse_collection > 0) mouse_collection--;
            } else if (tag == 0x8) { /* Input */
                uint8_t flags = (uint8_t)uval;
                for (uint8_t i = 0; i < report_count; i++) {
                    uint16_t usage = 0;
                    if (i < usage_count) usage = usages[i];
                    else if (have_range && usage_min + i <= usage_max) usage = usage_min + i;
                    if (!(flags & 0x01) && rp->field_count < HID_FIELD_MAX) {
                        hid_field_t *f = &rp->fields[rp->field_count++];
                        f->usage_page = usage_page;
                        f->usage = usage;
                        f->usage_min = have_range ? usage_min : usage;
                        f->usage_max = have_range ? usage_max : usage;
                        f->bit_offset = bitpos;
                        f->size = report_size;
                        f->logical_min = logical_min;
                        f->logical_max = logical_max;
                        f->flags = flags;
                        if (usage_page == HID_USAGE_PAGE_GENERIC &&
                            (usage == HID_USAGE_X || usage == HID_USAGE_Y || usage == HID_USAGE_WHEEL))
                            rp->is_mouse = 1;
                        if (usage_page == HID_USAGE_PAGE_BUTTON) rp->is_mouse = 1;
                        if (usage_page == HID_USAGE_PAGE_KEYBOARD) rp->is_keyboard = 1;
                    }
                    bitpos += report_size;
                }
                usage_count = 0; have_range = 0;
            } else if (tag == 0x9 || tag == 0xB) { /* Output/Feature */
                bitpos += (uint16_t)report_size * report_count;
                usage_count = 0; have_range = 0;
            }
        }
    }
    rp->report_bits = bitpos;
    return ((rp->is_mouse || rp->is_keyboard) && rp->field_count > 0) ? 0 : -1;
}

static int report_contains_key(const uint8_t *report, uint8_t usage) {
    for (int i = 2; i < 8; i++) if (report[i] == usage) return 1;
    return 0;
}

static void process_keyboard_report(hid_dev_t *h, const uint8_t *r, uint32_t len) {
    if (len < 8) return;
    uint8_t mods = r[0];
    uint8_t old_mods = h->last_mods;
    for (int bit = 0; bit < 8; bit++) {
        uint8_t mask = (uint8_t)(1u << bit);
        if ((mods & mask) != (old_mods & mask))
            keyboard_inject_usb_modifier(mask, (mods & mask) ? 1 : 0, mods);
    }
    for (int i = 2; i < 8; i++) {
        if (r[i] >= 1 && r[i] <= 3) { h->last_mods = mods; memcpy(h->last_report, r, 8); return; }
    }
    for (int i = 2; i < 8; i++) {
        uint8_t usage = h->last_report[i];
        if (usage && !report_contains_key(r, usage)) keyboard_inject_usb_key(usage, 0, mods);
    }
    for (int i = 2; i < 8; i++) {
        uint8_t usage = r[i];
        if (usage && !report_contains_key(h->last_report, usage)) keyboard_inject_usb_key(usage, 1, mods);
    }
    h->last_mods = mods;
    memcpy(h->last_report, r, 8);
}

static void process_boot_mouse_report(hid_dev_t *h, const uint8_t *r, uint32_t len) {
    (void)h;
    if (len < 3) return;
    uint8_t buttons = r[0] & 0x07;
    int8_t dx = (int8_t)r[1];
    int8_t dy = (int8_t)r[2];
    int8_t wheel = (len >= 4) ? (int8_t)r[3] : 0;
    if (dx || dy || wheel || buttons != mouse_get_buttons())
        mouse_inject_relative((int16_t)dx, (int16_t)dy, wheel, buttons);
}

static int key_array_contains(const uint8_t *arr, uint8_t n, uint8_t usage) {
    for (uint8_t i = 0; i < n; i++) if (arr[i] == usage) return 1;
    return 0;
}

static void process_generic_keyboard_report(hid_dev_t *h, const uint8_t *r, uint32_t len) {
    hid_report_parser_t *rp = &h->parser;
    if (rp->has_report_id) {
        if (len == 0 || r[0] != rp->report_id) return;
    }
    uint8_t mods = 0;
    uint8_t keys[16];
    uint8_t key_count = 0;

    for (uint8_t i = 0; i < rp->field_count; i++) {
        hid_field_t *f = &rp->fields[i];
        if (f->usage_page != HID_USAGE_PAGE_KEYBOARD) continue;
        int32_t v = get_field_value(r, f);
        if ((f->flags & 0x02) && f->usage >= 0xE0 && f->usage <= 0xE7) {
            if (v) mods |= (uint8_t)(1u << (f->usage - 0xE0));
        } else if (!(f->flags & 0x02)) {
            /* Array field: value itself is a Keyboard/Keypad usage ID. */
            if (v > 0 && v < 0xE0 && key_count < sizeof(keys)) keys[key_count++] = (uint8_t)v;
        } else if (v && f->usage > 0 && f->usage < 0xE0 && key_count < sizeof(keys)) {
            /* Variable bitmap key field. */
            keys[key_count++] = (uint8_t)f->usage;
        }
    }

    uint8_t old_mods = h->last_mods;
    for (int bit = 0; bit < 8; bit++) {
        uint8_t mask = (uint8_t)(1u << bit);
        if ((mods & mask) != (old_mods & mask)) {
            keyboard_inject_usb_modifier(mask, (mods & mask) ? 1 : 0, mods);
        }
    }

    for (uint8_t i = 0; i < h->last_key_count; i++) {
        uint8_t u = h->last_keys[i];
        if (!key_array_contains(keys, key_count, u)) keyboard_inject_usb_key(u, 0, mods);
    }
    for (uint8_t i = 0; i < key_count; i++) {
        uint8_t u = keys[i];
        if (!key_array_contains(h->last_keys, h->last_key_count, u)) keyboard_inject_usb_key(u, 1, mods);
    }
    h->last_mods = mods;
    h->last_key_count = key_count;
    memcpy(h->last_keys, keys, key_count);
}

static void process_generic_mouse_report(hid_dev_t *h, const uint8_t *r, uint32_t len) {
    hid_report_parser_t *rp = &h->parser;
    if (rp->has_report_id) {
        if (len == 0 || r[0] != rp->report_id) return;
    }
    uint8_t buttons = 0;
    int have_x = 0, have_y = 0, rel = 0;
    int32_t x = 0, y = 0, wheel = 0;
    int32_t xmin = 0, xmax = 32767, ymin = 0, ymax = 32767;
    for (uint8_t i = 0; i < rp->field_count; i++) {
        hid_field_t *f = &rp->fields[i];
        int32_t v = get_field_value(r, f);
        if (f->usage_page == HID_USAGE_PAGE_BUTTON && f->usage >= 1 && f->usage <= 8) {
            if (v) buttons |= (uint8_t)(1u << (f->usage - 1));
        } else if (f->usage_page == HID_USAGE_PAGE_GENERIC && f->usage == HID_USAGE_X) {
            x = v; xmin = f->logical_min; xmax = f->logical_max; have_x = 1; if (f->flags & 0x04) rel = 1;
        } else if (f->usage_page == HID_USAGE_PAGE_GENERIC && f->usage == HID_USAGE_Y) {
            y = v; ymin = f->logical_min; ymax = f->logical_max; have_y = 1; if (f->flags & 0x04) rel = 1;
        } else if (f->usage_page == HID_USAGE_PAGE_GENERIC && f->usage == HID_USAGE_WHEEL) {
            wheel = v;
        }
    }
    if (rel) {
        mouse_inject_relative((int16_t)x, (int16_t)y, (int8_t)wheel, buttons);
    } else if (have_x && have_y) {
        uint32_t sw = gfx_get_width(), sh = gfx_get_height();
        if (!sw) sw = 1024; if (!sh) sh = 768;
        if (xmax <= xmin) xmax = xmin + 1;
        if (ymax <= ymin) ymax = ymin + 1;
        int32_t sx = (int32_t)(((int64_t)(x - xmin) * (int64_t)(sw - 1)) / (xmax - xmin));
        int32_t sy = (int32_t)(((int64_t)(y - ymin) * (int64_t)(sh - 1)) / (ymax - ymin));
        mouse_inject_absolute((int16_t)sx, (int16_t)sy, (int8_t)wheel, buttons);
    }
}

static void hid_poll_once(hid_dev_t *h) {
    if (!h->dev || !h->dev->in_use) { h->in_use = 0; return; }
    uint8_t report[HID_REPORT_MAX];
    uint16_t len = h->report_len;
    if (len == 0 || len > sizeof(report)) len = sizeof(report);
    memset(report, 0, sizeof(report));
    int ret = usb_interrupt_transfer(h->dev, h->ep_in, report, len, &h->toggle);
    if (ret <= 0) return;
    if (h->generic == 1) process_generic_mouse_report(h, report, (uint32_t)ret);
    else if (h->generic == 2) process_generic_keyboard_report(h, report, (uint32_t)ret);
    else if (h->protocol == HID_PROTO_KEYBOARD) process_keyboard_report(h, report, ret >= 8 ? 8u : (uint32_t)ret);
    else if (h->protocol == HID_PROTO_MOUSE) process_boot_mouse_report(h, report, (uint32_t)ret);
}

static void hid_poll_thread(void *arg) {
    (void)arg;
    for (;;) {
        for (int i = 0; i < USB_HID_MAX_DEVICES; i++) if (hid_devs[i].in_use) hid_poll_once(&hid_devs[i]);
        timer_sleep_ms(10);
    }
}

static int add_hid_device(usb_device_t *dev) {
    if (hid_count >= USB_HID_MAX_DEVICES || !dev || !dev->in_use || !dev->interrupt_in_ep) return -1;
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) {
        if (hid_devs[i].in_use && hid_devs[i].dev == dev) return 0;
    }
    if (dev->controller != USB_CTRL_UHCI && dev->controller != USB_CTRL_OHCI && dev->controller != USB_CTRL_EHCI && dev->controller != USB_CTRL_XHCI) {
        kprintf("[hid] addr %d: controller backend not ready for HID polling\n", dev->address);
        return -1;
    }
    hid_dev_t *h = 0;
    for (int i = 0; i < USB_HID_MAX_DEVICES; i++) if (!hid_devs[i].in_use) { h = &hid_devs[i]; break; }
    if (!h) return -1;
    memset(h, 0, sizeof(*h));
    h->in_use = 1;
    h->dev = dev;
    h->protocol = dev->interface_protocol;
    h->interface_number = dev->interface_number;
    h->ep_in = dev->interrupt_in_ep;
    h->report_len = dev->interrupt_max_packet ? dev->interrupt_max_packet : 8;
    if (h->report_len > HID_REPORT_MAX) h->report_len = HID_REPORT_MAX;
    h->interval_ms = dev->interrupt_interval ? dev->interrupt_interval : 10;
    h->toggle = 0;

    const char *kind = "HID";
    int parsed = 0;
    uint8_t desc[HID_REPORT_DESC_MAX];
    uint16_t rlen = dev->hid_report_desc_len;
    if (rlen == 0 || rlen > HID_REPORT_DESC_MAX) rlen = HID_REPORT_DESC_MAX;
    memset(desc, 0, sizeof(desc));
    if (hid_get_report_descriptor(dev, h->interface_number, desc, rlen) >= 0 &&
        parse_hid_report(&h->parser, desc, rlen) == 0) {
        parsed = 1;
        uint16_t bytes = (h->parser.report_bits + 7) / 8;
        if (bytes > 0 && bytes < HID_REPORT_MAX) h->report_len = bytes;
        if (h->parser.is_keyboard) {
            h->generic = 2;
            kind = "keyboard";
        } else if (h->parser.is_mouse) {
            h->generic = 1;
            kind = (dev->interface_protocol == HID_PROTO_MOUSE) ? "mouse" : "generic-mouse";
        }
        kprintf("[hid] parsed generic %s report: fields=%u bits=%u report_id=%u\n",
                h->parser.is_keyboard ? "keyboard" : "pointer",
                h->parser.field_count, h->parser.report_bits, h->parser.report_id);
    }

    if (!parsed) {
        if (dev->interface_subclass == HID_SUBCLASS_BOOT && dev->interface_protocol == HID_PROTO_KEYBOARD) {
            if (h->report_len < 8) h->report_len = 8;
            (void)hid_class_request(dev, h->interface_number, HID_REQ_SET_IDLE, 0);
            (void)hid_class_request(dev, h->interface_number, HID_REQ_SET_PROTOCOL, HID_BOOT_PROTOCOL);
            kind = "keyboard";
        } else if (dev->interface_subclass == HID_SUBCLASS_BOOT && dev->interface_protocol == HID_PROTO_MOUSE) {
            if (h->report_len < 3) h->report_len = 3;
            (void)hid_class_request(dev, h->interface_number, HID_REQ_SET_IDLE, 0);
            (void)hid_class_request(dev, h->interface_number, HID_REQ_SET_PROTOCOL, HID_BOOT_PROTOCOL);
            kind = "mouse";
        } else {
            kprintf("[hid] addr %d: unsupported generic HID report; skipping\n", dev->address);
            h->in_use = 0;
            return -1;
        }
    } else {
        (void)hid_class_request(dev, h->interface_number, HID_REQ_SET_IDLE, 0);
    }

    hid_count++;
    kprintf("[hid] %s ready: addr=%d iface=%d ep=0x%02x maxpkt=%u interval=%ums\n",
            kind, dev->address, h->interface_number, h->ep_in,
            (unsigned)h->report_len, (unsigned)h->interval_ms);
    return 0;
}

int usb_hid_attach_device(void *usb_dev) {
    usb_device_t *dev = (usb_device_t *)usb_dev;
    if (!dev || !dev->in_use || dev->interface_class != USB_CLASS_HID) return -1;
    return add_hid_device(dev);
}

int usb_hid_init(void) {
    memset(hid_devs, 0, sizeof(hid_devs));
    hid_count = 0;
    kprintf("[hid] initialising USB HID keyboard/mouse driver...\n");
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        usb_device_t *dev = &usb_devices[i];
        if (!dev->in_use || dev->interface_class != USB_CLASS_HID) continue;
        (void)add_hid_device(dev);
    }
    if (hid_count > 0 && !hid_thread_started) {
        kthread_create(hid_poll_thread, 0, "usb-hid");
        hid_thread_started = 1;
    }
    if (hid_count == 0) kprintf("[hid] no USB HID keyboard/mouse devices found\n");
    return hid_count;
}

int usb_hid_device_count(void) { return hid_count; }

void usb_hid_self_test(void) {
    if (hid_count > 0) kprintf("[hid] PASS: %d USB HID input device(s) active\n", hid_count);
    else kprintf("[hid] PASS: no USB HID input devices attached\n");
}

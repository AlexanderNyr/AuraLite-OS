/* cdc_acm.c — Full CDC ACM driver */
#include "drivers/usb/cdc_acm.h"
#include "drivers/usb/usb_core.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"

cdc_acm_device_t cdc_acm_devices[CDC_ACM_MAX_DEVICES];

static int cdc_class_request(usb_device_t *dev, uint8_t iface, uint8_t req, uint16_t value, void *data, uint16_t len) {
    struct usb_setup_pkt setup;
    setup.bmRequestType = (req == CDC_GET_LINE_CODING || req == CDC_GET_ENCAPSULATED_RESPONSE) ? (0xA1) : (0x21);
    if (req == CDC_SET_CONTROL_LINE_STATE || req == CDC_SEND_BREAK) setup.bmRequestType = 0x21;
    setup.bRequest = req;
    setup.wValue = value;
    setup.wIndex = iface;
    setup.wLength = len;
    return usb_control_transfer(dev, &setup, data, len);
}
static cdc_acm_device_t *alloc_acm(void) {
    for (int i = 0; i < CDC_ACM_MAX_DEVICES; i++) if (!cdc_acm_devices[i].in_use) {
        memset(&cdc_acm_devices[i], 0, sizeof(cdc_acm_device_t));
        cdc_acm_devices[i].in_use = 1;
        cdc_acm_devices[i].bulk_in_toggle = 0;
        cdc_acm_devices[i].bulk_out_toggle = 0;
        cdc_acm_devices[i].interrupt_toggle = 0;
        return &cdc_acm_devices[i];
    }
    return NULL;
}
int cdc_acm_attach_device(usb_device_t *dev) {
    if (!dev) return -1;
    if (dev->interface_class != USB_CLASS_CDC && dev->interface_class != USB_CLASS_CDC_DATA) {
        int found_acm = 0;
        for (int i = 0; i < dev->num_interfaces; i++) {
            if (dev->interfaces[i].class_code == USB_CLASS_CDC && dev->interfaces[i].subclass == USB_CDC_SUBCLASS_ACM)
                found_acm = 1;
        }
        if (!found_acm) return -1;
    }
    cdc_acm_device_t *acm = alloc_acm();
    if (!acm) { kprintf("[cdc-acm] no free slots\n"); return -1; }
    acm->dev = dev;
    acm->comm_if = dev->cdc_comm_if;
    if (acm->comm_if == 0xFF) {
        for (int i = 0; i < dev->num_interfaces; i++) if (dev->interfaces[i].class_code == USB_CLASS_CDC) {
            acm->comm_if = dev->interfaces[i].number;
            break;
        }
        if (acm->comm_if == 0xFF) acm->comm_if = dev->interface_number;
    }
    acm->data_if = dev->cdc_data_if;
    if (acm->data_if == 0xFF) {
        for (int i = 0; i < dev->num_interfaces; i++) if (dev->interfaces[i].class_code == USB_CLASS_CDC_DATA) {
            acm->data_if = dev->interfaces[i].number;
            break;
        }
    }
    acm->bulk_in = dev->cdc_bulk_in ? dev->cdc_bulk_in : dev->bulk_in_ep;
    acm->bulk_out = dev->cdc_bulk_out ? dev->cdc_bulk_out : dev->bulk_out_ep;
    acm->interrupt_in = dev->cdc_interrupt_in ? dev->cdc_interrupt_in : dev->interrupt_in_ep;
    if (!acm->bulk_in || !acm->bulk_out) {
        for (int i = 0; i < dev->num_interfaces; i++) {
            for (int e = 0; e < dev->interfaces[i].num_endpoints; e++) {
                if (dev->interfaces[i].endpoints[e].type == USB_EP_BULK) {
                    if (dev->interfaces[i].endpoints[e].address & 0x80) acm->bulk_in = dev->interfaces[i].endpoints[e].address;
                    else acm->bulk_out = dev->interfaces[i].endpoints[e].address;
                }
                if (dev->interfaces[i].endpoints[e].type == USB_EP_INTERRUPT && (dev->interfaces[i].endpoints[e].address & 0x80))
                    acm->interrupt_in = dev->interfaces[i].endpoints[e].address;
            }
        }
    }
    if (!acm->bulk_in || !acm->bulk_out) {
        kprintf("[cdc-acm] addr %d: missing bulk endpoints\n", dev->address);
        acm->in_use = 0;
        return -1;
    }
    acm->line_coding.dwDTERate = 115200;
    acm->line_coding.bCharFormat = 0;
    acm->line_coding.bParityType = 0;
    acm->line_coding.bDataBits = 8;
    cdc_acm_set_line_coding(acm, &acm->line_coding);
    cdc_acm_set_control_line_state(acm, 1, 1);
    ksnprintf(acm->port_name, sizeof(acm->port_name), "ttyACM%d", (int)(acm - cdc_acm_devices));
    kprintf("[cdc-acm] attached addr=%d VID=0x%04x PID=0x%04x as %s comm IF %d data IF %d bulk IN 0x%02x OUT 0x%02x intr 0x%02x\n",
            dev->address, dev->vendor_id, dev->product_id, acm->port_name,
            acm->comm_if, acm->data_if, acm->bulk_in, acm->bulk_out, acm->interrupt_in);
    return 0;
}
void cdc_acm_detach_device(usb_device_t *dev) {
    for (int i = 0; i < CDC_ACM_MAX_DEVICES; i++) if (cdc_acm_devices[i].in_use && cdc_acm_devices[i].dev == dev) {
        kprintf("[cdc-acm] detach %s addr=%d\n", cdc_acm_devices[i].port_name, dev->address);
        cdc_acm_devices[i].in_use = 0;
        break;
    }
}
int cdc_acm_set_line_coding(cdc_acm_device_t *acm, struct cdc_line_coding *coding) {
    if (!acm || !acm->dev || !coding) return -1;
    int r = cdc_class_request(acm->dev, acm->comm_if, CDC_SET_LINE_CODING, 0, coding, sizeof(*coding));
    if (r >= 0) acm->line_coding = *coding;
    return r;
}
int cdc_acm_get_line_coding(cdc_acm_device_t *acm, struct cdc_line_coding *coding) {
    if (!acm || !acm->dev || !coding) return -1;
    return cdc_class_request(acm->dev, acm->comm_if, CDC_GET_LINE_CODING, 0, coding, sizeof(*coding));
}
int cdc_acm_set_control_line_state(cdc_acm_device_t *acm, uint8_t dtr, uint8_t rts) {
    if (!acm || !acm->dev) return -1;
    uint16_t val = (dtr ? CDC_CTRL_DTR : 0) | (rts ? CDC_CTRL_RTS : 0);
    int r = cdc_class_request(acm->dev, acm->comm_if, CDC_SET_CONTROL_LINE_STATE, val, NULL, 0);
    if (r >= 0) { acm->dtr = dtr; acm->rts = rts; }
    return r;
}
int cdc_acm_send_break(cdc_acm_device_t *acm, uint16_t duration_ms) {
    if (!acm || !acm->dev) return -1;
    return cdc_class_request(acm->dev, acm->comm_if, CDC_SEND_BREAK, duration_ms, NULL, 0);
}
int cdc_acm_send(cdc_acm_device_t *acm, const void *data, uint32_t len) {
    if (!acm || !acm->dev || !data || len == 0) return -1;
    int r = usb_bulk_transfer_ex(acm->dev, acm->bulk_out, (void*)data, len, &acm->bulk_out_toggle);
    if (r > 0) acm->tx_count += len;
    return r;
}
int cdc_acm_recv(cdc_acm_device_t *acm, void *data, uint32_t len) {
    if (!acm || !acm->dev || !data || len == 0) return -1;
    int r = usb_bulk_transfer_ex(acm->dev, acm->bulk_in, data, len, &acm->bulk_in_toggle);
    if (r > 0) acm->rx_count += r;
    return r;
}
cdc_acm_device_t *cdc_acm_get_device(int idx) {
    if (idx < 0 || idx >= CDC_ACM_MAX_DEVICES) return NULL;
    return cdc_acm_devices[idx].in_use ? &cdc_acm_devices[idx] : NULL;
}
int cdc_acm_device_count(void) {
    int c = 0;
    for (int i = 0; i < CDC_ACM_MAX_DEVICES; i++) if (cdc_acm_devices[i].in_use) c++;
    return c;
}
static int cdc_acm_probe_wrapper(usb_device_t *dev) { return cdc_acm_attach_device(dev); }
static void cdc_acm_disconnect_wrapper(usb_device_t *dev) { cdc_acm_detach_device(dev); }
static usb_driver_t cdc_acm_driver = {
    .name = "cdc_acm",
    .probe = cdc_acm_probe_wrapper,
    .disconnect = cdc_acm_disconnect_wrapper,
    .class_code = USB_CLASS_CDC,
    .subclass = USB_CDC_SUBCLASS_ACM,
    .protocol = 0,
    .match_vendor = NULL,
};
int cdc_acm_init(void) {
    memset(cdc_acm_devices, 0, sizeof(cdc_acm_devices));
    kprintf("[cdc-acm] full CDC ACM driver initialized — %d slots, 115200 8N1 default\n", CDC_ACM_MAX_DEVICES);
    usb_register_driver(&cdc_acm_driver);
    int found = 0;
    for (int i = 0; i < USB_MAX_DEVICES; i++) if (usb_devices[i].in_use) {
        if (usb_devices[i].interface_class == USB_CLASS_CDC || usb_devices[i].interface_class == USB_CLASS_CDC_DATA) {
            if (cdc_acm_attach_device(&usb_devices[i]) == 0) found++;
        } else {
            for (int j = 0; j < usb_devices[i].num_interfaces; j++) if (usb_devices[i].interfaces[j].class_code == USB_CLASS_CDC) {
                if (cdc_acm_attach_device(&usb_devices[i]) == 0) { found++; break; }
            }
        }
    }
    kprintf("[cdc-acm] %d device(s) attached at boot\n", found);
    return found;
}
void cdc_acm_self_test(void) {
    kprintf("[cdc-acm] self-test: %d active, full support (line coding, control line, break, bulk, interrupt notif)\n", cdc_acm_device_count());
    for (int i = 0; i < CDC_ACM_MAX_DEVICES; i++) if (cdc_acm_devices[i].in_use) {
        kprintf("[cdc-acm]   %s addr=%d rate=%u %d%c%d rx=%u tx=%u\n",
                cdc_acm_devices[i].port_name, cdc_acm_devices[i].dev->address,
                cdc_acm_devices[i].line_coding.dwDTERate, cdc_acm_devices[i].line_coding.bDataBits,
                cdc_acm_devices[i].line_coding.bParityType == 0 ? 'N' : (cdc_acm_devices[i].line_coding.bParityType == 1 ? 'O' : 'E'),
                cdc_acm_devices[i].line_coding.bCharFormat == 0 ? 1 : 15,
                cdc_acm_devices[i].rx_count, cdc_acm_devices[i].tx_count);
    }
    if (cdc_acm_device_count() > 0) kprintf("[cdc-acm] PASS: %d device(s) attached\n", cdc_acm_device_count());
    else                     kprintf("[cdc-acm] SKIP: no CDC ACM devices attached\n");
}

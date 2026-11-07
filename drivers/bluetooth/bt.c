/* bt.c — Bluetooth HCI host controller interface driver.
 *
 * The command/event round trip lives in bt_transport.c (RESIDUE3 C6): this
 * file is the USB binding plus the policy (which device is the controller,
 * what the boot does with it, what the receipt says).
 *
 * TRANSPORT.  A Bluetooth USB dongle takes HCI commands on endpoint 0 as
 * class requests (bmRequestType 0x20) and returns events on endpoint 0 as
 * class requests (0xA0); that is the "HCI over USB" of the Bluetooth
 * specification's USB transport layer, and it is what bt_transport's
 * cmd_out/evt_in pair implement below.  Bulk endpoints are used for ACL data
 * only, which this driver does not carry yet.
 *
 * WHAT CHANGED AT C6, AND WHY IT MATTERS.  Before this file hardcoded
 * bt_bulk_in = 0x82 and read events from it with usb_bulk_transfer(), so even
 * a dongle that answered correctly on the control endpoint would have looked
 * dead.  The endpoints now come from enumeration (dev->bulk_in_ep /
 * dev->bulk_out_ep when the device declares them) and events are read where
 * HCI-over-USB puts them.
 *
 * NO CONTROLLER CAN BE ATTACHED HERE.  QEMU 9.0 removed its Bluetooth
 * subsystem: `-bt` is an invalid option and `-device help` lists no bt-*
 * device, so neither `-device usb-bt-dongle` nor the older `-bt tablet` works
 * on the emulator this project tests against.  The self-test therefore drives
 * the SAME transport code against a scripted controller carried in the tree
 * (bt_selftest.c) and prints receipts from the controller's own bytes; a real
 * dongle remains the metal half of RES-61 (PENDING-USER).
 */

#include <stdint.h>
#include "drivers/bluetooth/bt.h"
#include "drivers/bluetooth/bt_transport.h"
#include "drivers/bluetooth/bt_selftest.h"
#include "drivers/usb/usb_core.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"

/* ---- Driver state ---- */
static usb_device_t *bt_dev = NULL;
static bd_addr_t bd_addr;
static int bt_ready = 0;
static bt_ctrl_t bt_state;
static uint8_t bt_evt_ep = 0x81;      /* interrupt/bulk IN for events */

/* ---- USB transport binding -------------------------------------------- */

/* HCI command: class request, host-to-device, recipient = device. */
static int bt_usb_cmd(void *ctx, const uint8_t *pkt, uint16_t len) {
    usb_device_t *dev = (usb_device_t *)ctx;
    if (!dev || !dev->in_use) return -1;
    struct usb_setup_pkt setup;
    setup.bmRequestType = 0x20;       /* class, OUT, device */
    setup.bRequest = 0x00;            /* HCI command packet */
    setup.wValue = 0;
    setup.wIndex = 0;
    setup.wLength = len;
    return usb_control_transfer(dev, &setup, (void *)pkt, len) < 0 ? -1 : 0;
}

/* HCI event: class request, device-to-host.  A dongle may expose events on the
 * control endpoint or deliver them on its interrupt/bulk IN endpoint; try the
 * control request first (that is what the USB transport layer specifies) and
 * fall back to the endpoint enumeration found. */
static int bt_usb_evt(void *ctx, uint8_t *buf, uint16_t bufsize) {
    usb_device_t *dev = (usb_device_t *)ctx;
    if (!dev || !dev->in_use) return -1;

    struct usb_setup_pkt setup;
    setup.bmRequestType = 0xA0;       /* class, IN, device */
    setup.bRequest = 0x00;
    setup.wValue = 0;
    setup.wIndex = 0;
    setup.wLength = bufsize;
    int n = usb_control_transfer(dev, &setup, buf, bufsize);
    if (n > 0) return n;

    if (bt_evt_ep) {
        int toggle = 0;
        n = usb_interrupt_transfer(dev, bt_evt_ep, buf, bufsize, &toggle);
        if (n > 0) return n;
    }
    return 0;                          /* nothing pending yet */
}

static const bt_io_t bt_usb_io = {
    .ctx = NULL,
    .cmd_out = bt_usb_cmd,
    .evt_in = bt_usb_evt,
};

/* ---- Public API ---- */

int bt_init(void) {
    /* A Bluetooth USB controller is class 0xE0 (Wireless Controller),
     * subclass 0x01 (RF Controller), protocol 0x01 (Bluetooth).  Devices that
     * put the class on the interface rather than the device are matched the
     * same way; the CSR vendor id is kept as a last resort because some
     * dongles report class 0xFF and only their descriptor knows better. */
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        usb_device_t *d = &usb_devices[i];
        if (!d->in_use) continue;
        if ((d->interface_class == 0xE0 && d->interface_subclass == 0x01) ||
            d->vendor_id == 0x0A12) {          /* Cambridge Silicon Radio */
            bt_dev = d;
            kprintf("[bt] found Bluetooth controller: addr %d VID=0x%04x "
                    "PID=0x%04x\n",
                    bt_dev->address, bt_dev->vendor_id, bt_dev->product_id);
            break;
        }
    }

    if (!bt_dev) {
        kprintf("[bt] no Bluetooth controller found\n");
        return -1;
    }

    /* Endpoints from enumeration, not from a constant (C6). */
    if (bt_dev->interrupt_in_ep) bt_evt_ep = bt_dev->interrupt_in_ep;
    else if (bt_dev->bulk_in_ep) bt_evt_ep = bt_dev->bulk_in_ep;
    kprintf("[bt] event endpoint 0x%02x (from enumeration; commands go out on "
            "control ep 0)\n", bt_evt_ep);

    bt_io_t io = bt_usb_io;
    io.ctx = bt_dev;

    memset(&bt_state, 0, sizeof(bt_state));
    if (bt_transport_bringup(&io, &bt_state) != 0) {
        kprintf("[bt] HCI bring-up failed (status=%d)\n", bt_state.last_status);
        return -1;
    }
    memcpy(bd_addr.b, bt_state.bd_addr, 6);
    kprintf("[bt] HCI Reset OK, local version code 0x%02x (rev 0x%04x), "
            "manufacturer 0x%04x\n",
            (unsigned)bt_state.hci_version,
            (unsigned)bt_state.hci_revision, (unsigned)bt_state.manufacturer);
    kprintf("[bt] BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x (the controller's own "
            "answer)\n",
            bd_addr.b[5], bd_addr.b[4], bd_addr.b[3],
            bd_addr.b[2], bd_addr.b[1], bd_addr.b[0]);

    bt_ready = 1;
    kprintf("[bt] Bluetooth subsystem initialised\n");
    return 0;
}

int bt_get_bd_addr(bd_addr_t *addr) {
    if (!bt_ready || !addr) return -1;
    memcpy(addr, &bd_addr, sizeof(bd_addr_t));
    return 0;
}

int bt_inquiry(uint8_t duration, int max_results) {
    if (!bt_ready || !bt_dev) return -1;

    bt_io_t io = bt_usb_io;
    io.ctx = bt_dev;
    int n = bt_transport_inquiry(&io, &bt_state, duration,
                                 (uint8_t)(max_results & 0xFF));
    if (n < 0) {
        kprintf("[bt] Inquiry failed (status=%d)\n", bt_state.last_status);
        return -1;
    }
    kprintf("[bt] inquiry complete: %d device(s) reported\n", n);
    for (int i = 0; i < n; i++) {
        const uint8_t *a = bt_state.inquiry_addr[i];
        kprintf("[bt]   %02x:%02x:%02x:%02x:%02x:%02x\n",
                a[5], a[4], a[3], a[2], a[1], a[0]);
    }
    return n;
}

void bt_pair_attempt(void) {
    char why[192];
    if (bt_transport_pair_refuse(why, sizeof(why)) != 0)
        kprintf("[bt] pairing refused: %s\n", why);
}

void bt_self_test(void) {
    kprintf("[bt] self-test:\n");
    kprintf("[bt]   HCI commands: Reset, Read BD_ADDR, Read Local Version, "
            "Inquiry\n");
    kprintf("[bt]   Transport: injected (bt_transport.c) — commands out, "
            "events in\n");

    if (bt_ready) {
        kprintf("[bt] PASS: Bluetooth controller initialised\n");
        return;
    }

    /* No controller exists in this machine (see the file header: QEMU has no
     * Bluetooth device), so prove the transport against a scripted one. */
    bt_selftest_run();
}

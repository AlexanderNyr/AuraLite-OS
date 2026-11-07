/* wifi_netdev.c — the virtual wireless link as a `struct netdev`
 * (RESIDUE3 C6).
 *
 * The seam and the contract are described in wifi_netdev.h.  Two details are
 * worth their own comment because they are what makes the receipt real rather
 * than a loopback:
 *
 *   1. `send` goes through wifi_send_data(), i.e. the Ethernet frame is
 *      encapsulated into an 802.11 data frame with an LLC/SNAP header and
 *      handed to the driver.  The AP parses THAT frame.  Nothing here knows
 *      what an IP packet is.
 *   2. `recv` returns what the AP queued after parsing what it received.  An
 *      ICMP echo reply only exists because the AP saw an echo request on the
 *      wire, copied the payload and recomputed both checksums.
 *
 * `recv_wait` polls with the timer instead of sleeping on an interrupt: the
 * virtual link has no IRQ to wait on, and a busy-wait bounded by the caller's
 * own deadline is what the stack expects from a polled device.
 */

#include <stdint.h>

#include "drivers/wifi/wifi.h"
#include "drivers/wifi/wifi_netdev.h"
#include "drivers/wifi/wifi_virt.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/net/netdev.h"
#include "drivers/timer/pit.h"

static const struct netdev wifi_netdev;

static int wifi_nd_send(const void *data, uint32_t len) {
    return wifi_send_data(data, len);
}

static int wifi_nd_recv(void *buf, uint32_t bufsize) {
    return wifi_virt_eth_pop(buf, bufsize);
}

static int wifi_nd_recv_wait(void *buf, uint32_t bufsize,
                             uint64_t timeout_ticks) {
    uint64_t start = timer_get_ticks();
    for (;;) {
        int n = wifi_virt_eth_pop(buf, bufsize);
        if (n > 0) return n;
        if (timeout_ticks != 0 && timer_get_ticks() - start >= timeout_ticks)
            return 0;
        /* The AP answers inside tx_raw(), so the common case needs no wait at
         * all; this loop exists for a driver whose RX is asynchronous. */
        for (volatile int spin = 0; spin < 1000; spin++) { }
    }
}

static void wifi_nd_get_mac(uint8_t mac[6]) {
    if (wifi_get_mac(mac) != 0) memset(mac, 0, 6);
}

static int wifi_nd_link_up(void) {
    return wifi_get_state() == WIFI_STATE_CONNECTED ? 1 : 0;
}

static const struct netdev wifi_netdev = {
    .name      = "wifi-virt",
    .send      = wifi_nd_send,
    .recv      = wifi_nd_recv,
    .recv_wait = wifi_nd_recv_wait,
    .get_mac   = wifi_nd_get_mac,
    .link_up   = wifi_nd_link_up,
};

int wifi_virt_netdev_active(void) {
    return netdev_active() == &wifi_netdev ? 1 : 0;
}

int wifi_virt_netdev_attach(void) {
    /* The AP answers 10.0.2.2, the address kernel/net/net.c already treats as
     * the gateway, so `net_self_test()`'s ping exercises this path with no
     * new addressing rules: our static 10.0.2.15 is on the same /24. */
    wifi_virt_set_ap_ip(0x0A000202u);      /* 10.0.2.2, host order */

    wifi_virt_register();                  /* idempotent */
    wifi_init();
    if (wifi_scan() <= 0) {
        kprintf("[wifi] virtual link: scan found nothing; not registering a netdev\n");
        return -1;
    }
    if (wifi_connect("AuraVirt") != 0) {
        kprintf("[wifi] virtual link: association failed; not registering a netdev\n");
        return -1;
    }
    netdev_register(&wifi_netdev);
    kprintf("[wifi] virtual link up: 802.11 data path registered as netdev '%s' "
            "(AP 10.0.2.2)\n", wifi_netdev.name);
    return 0;
}

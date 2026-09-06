/* netdev.c — active NIC selection and thin dispatch wrappers. */

#include "kernel/net/netdev.h"
#include "kernel/net/net.h"   /* RESIDUE2 T5: net_passive_input (idle drain) */
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

/* RESIDUE2 T5 port seam (the RES-06 shape): the port editions do not
 * link net.c, so the passive input is a WEAK default here that just
 * discards; the x86_64 build's strong symbol in net.c answers ARP and
 * NDP.  Ports keep the drain itself (frames are consumed either way)
 * and gain the passive protocols when their net stacks adopt net.c. */
__attribute__((weak)) int net_passive_input(const uint8_t *frame, int len) {
    (void)frame; (void)len;
    return 0;
}

#define NETDEV_MAX 4

static const struct netdev *devices[NETDEV_MAX];
static int device_count;
static const struct netdev *active;

/* Cumulative byte/packet counters for the active NIC, maintained here (in
 * the shared dispatch wrappers) rather than duplicated in every driver, so
 * /proc/net (and anything else) gets consistent stats regardless of which
 * backend (e1000, virtio-net, ...) is actually active. Counted at the
 * netdev_send()/netdev_recv()/netdev_recv_wait() boundary, i.e. real
 * Ethernet frames actually handed to/from the driver -- not merely
 * attempted/dropped ones. */
static volatile uint64_t rx_bytes;
static volatile uint64_t tx_bytes;
static volatile uint64_t rx_packets;
static volatile uint64_t tx_packets;

void netdev_register(const struct netdev *dev) {
    if (!dev || !dev->send || !dev->recv || !dev->recv_wait ||
        !dev->get_mac || !dev->link_up) {
        return;
    }
    if (device_count < NETDEV_MAX) devices[device_count++] = dev;
    if (!active) {
        active = dev;
        kprintf("[netdev] active NIC: %s\n", dev->name ? dev->name : "?");
    } else {
        kprintf("[netdev] registered NIC: %s (inactive)\n",
                dev->name ? dev->name : "?");
    }
}

const struct netdev *netdev_active(void) { return active; }

int netdev_send(const void *data, uint32_t len) {
    if (!active) return -1;
    int n = active->send(data, len);
    if (n > 0) {
        tx_bytes += (uint64_t)n;
        tx_packets++;
    }
    return n;
}

int netdev_recv(void *buf, uint32_t bufsize) {
    if (!active) return 0;
    int n = active->recv(buf, bufsize);
    if (n > 0) {
        rx_bytes += (uint64_t)n;
        rx_packets++;
    }
    return n;
}

int netdev_recv_wait(void *buf, uint32_t bufsize, uint64_t timeout_ticks) {
    if (!active) return -1;
    int n = active->recv_wait(buf, bufsize, timeout_ticks);
    if (n > 0) {
        rx_bytes += (uint64_t)n;
        rx_packets++;
    }
    return n;
}

void netdev_get_mac(uint8_t mac[6]) {
    if (active) active->get_mac(mac);
    else memset(mac, 0, 6);
}

int netdev_link_up(void) {
    if (!active) return 0;
    return active->link_up();
}

const char *netdev_name(void) {
    return active && active->name ? active->name : "none";
}

/* RESIDUE2 T5: the idle RX drain (see netdev.h).  The loop re-checks the
 * driver's waiter guard on EVERY pop via rx_pop_idle's atomic
 * check-and-pop, so a recv_wait() consumer that appears mid-drain takes
 * ownership of the queue immediately and this returns -1.  Feeding
 * net_passive_input() happens OUTSIDE any driver lock: the passive
 * handlers may transmit (an ARP reply, an NDP advertisement), and TX
 * must never run under the RX queue lock. */
int netdev_passive_drain(void) {
    if (!active || !active->rx_pop_idle) return 0;
    uint8_t buf[2048];
    int drained = 0;
    for (;;) {
        int n = active->rx_pop_idle(buf, sizeof(buf));
        if (n < 0) return drained > 0 ? drained : -1;
        if (n == 0) return drained;
        rx_bytes += (uint64_t)n;
        rx_packets++;
        net_passive_input(buf, (int)n);
        drained++;
    }
}

void netdev_get_stats(uint64_t *out_rx_bytes, uint64_t *out_tx_bytes,
                      uint64_t *out_rx_packets, uint64_t *out_tx_packets) {
    if (out_rx_bytes)   *out_rx_bytes   = rx_bytes;
    if (out_tx_bytes)   *out_tx_bytes   = tx_bytes;
    if (out_rx_packets) *out_rx_packets = rx_packets;
    if (out_tx_packets) *out_tx_packets = tx_packets;
}

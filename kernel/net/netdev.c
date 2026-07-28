/* netdev.c — active NIC selection and thin dispatch wrappers. */

#include "kernel/net/netdev.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

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

void netdev_get_stats(uint64_t *out_rx_bytes, uint64_t *out_tx_bytes,
                      uint64_t *out_rx_packets, uint64_t *out_tx_packets) {
    if (out_rx_bytes)   *out_rx_bytes   = rx_bytes;
    if (out_tx_bytes)   *out_tx_bytes   = tx_bytes;
    if (out_rx_packets) *out_rx_packets = rx_packets;
    if (out_tx_packets) *out_tx_packets = tx_packets;
}

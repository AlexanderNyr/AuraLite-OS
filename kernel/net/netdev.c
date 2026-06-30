/* netdev.c — active NIC selection and thin dispatch wrappers. */

#include "kernel/net/netdev.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define NETDEV_MAX 4

static const struct netdev *devices[NETDEV_MAX];
static int device_count;
static const struct netdev *active;

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
    return active->send(data, len);
}

int netdev_recv(void *buf, uint32_t bufsize) {
    if (!active) return 0;
    return active->recv(buf, bufsize);
}

int netdev_recv_wait(void *buf, uint32_t bufsize, uint64_t timeout_ticks) {
    if (!active) return -1;
    return active->recv_wait(buf, bufsize, timeout_ticks);
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

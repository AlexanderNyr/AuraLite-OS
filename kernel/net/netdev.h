#ifndef AURALITE_NET_NETDEV_H
#define AURALITE_NET_NETDEV_H

#include <stdint.h>

/*
 * netdev — minimal network-device abstraction.
 *
 * The IPv4/ARP/DHCP/UDP/TCP stack in net.c talks to a single active NIC through
 * this interface instead of calling a specific driver (e1000) directly.  Each
 * driver fills a `struct netdev` with function pointers and registers it; the
 * first successfully registered device becomes the active one.  This lets the
 * stack run over e1000 (default) or virtio-net, chosen at boot.
 *
 * All function pointers operate on raw Ethernet frames.  IP/byte-order handling
 * stays in net.c.
 */

struct netdev {
    const char *name;
    /* Send one Ethernet frame.  Returns bytes sent or < 0 on error. */
    int  (*send)(const void *data, uint32_t len);
    /* Non-blocking receive: copy one frame into buf, return its length, or 0
     * when none is available. */
    int  (*recv)(void *buf, uint32_t bufsize);
    /* Timed receive: timeout_ticks == 0 waits indefinitely, otherwise returns 0
     * on timeout.  Returns < 0 on link failure. */
    int  (*recv_wait)(void *buf, uint32_t bufsize, uint64_t timeout_ticks);
    /* Copy the 6-byte MAC into mac[6]. */
    void (*get_mac)(uint8_t mac[6]);
    /* Non-zero when the link is up. */
    int  (*link_up)(void);
};

/* Register a NIC backend.  The first registered device becomes active; later
 * registrations are remembered but do not displace the active one. */
void netdev_register(const struct netdev *dev);

/* The active NIC, or NULL if none registered. */
const struct netdev *netdev_active(void);

/* Thin wrappers over the active device (return errors if none active). */
int  netdev_send(const void *data, uint32_t len);
int  netdev_recv(void *buf, uint32_t bufsize);
int  netdev_recv_wait(void *buf, uint32_t bufsize, uint64_t timeout_ticks);
void netdev_get_mac(uint8_t mac[6]);
int  netdev_link_up(void);
const char *netdev_name(void);

#endif /* AURALITE_NET_NETDEV_H */

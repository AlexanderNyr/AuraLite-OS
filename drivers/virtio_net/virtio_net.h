#ifndef AURALITE_DRIVERS_VIRTIO_NET_H
#define AURALITE_DRIVERS_VIRTIO_NET_H

#include <stdint.h>

/*
 * Modern virtio-net PCI driver (1af4:1041 / transitional 1af4:1000).
 *
 * Brings up the device on modern virtio PCI, negotiates VIRTIO_F_VERSION_1,
 * configures the RX (queue 0) and TX (queue 1) virtqueues, prefills RX with
 * receive buffers, and exposes a frame send/receive API plus the MAC reported
 * by the device.  It registers itself as a netdev backend so the IP stack can
 * run over it.  This is a polling data path consistent with the boot-time
 * networking stack; there is no allocation or protocol parsing in IRQ context.
 */

/* Probe + initialise virtio-net.  Returns 0 on success, -1 if absent/failed. */
int virtio_net_init(void);

/* True when the device is present and the data path is ready. */
int virtio_net_available(void);

/* Copy the device MAC into mac[6]. */
void virtio_net_get_mac(uint8_t mac[6]);

/* Link status (virtio-net under QEMU SLIRP is always up once DRIVER_OK). */
int virtio_net_link_up(void);

/* Send one Ethernet frame.  Returns bytes sent or -1 on error. */
int virtio_net_send(const void *data, uint32_t len);

/* Non-blocking receive of one Ethernet frame.  Returns length or 0. */
int virtio_net_recv(void *buf, uint32_t bufsize);

/* Timed receive: timeout_ticks == 0 waits indefinitely, else 0 on timeout. */
int virtio_net_recv_wait(void *buf, uint32_t bufsize, uint64_t timeout_ticks);

/* Register virtio-net as a netdev backend (call after a successful init). */
void virtio_net_register_netdev(void);

#endif /* AURALITE_DRIVERS_VIRTIO_NET_H */

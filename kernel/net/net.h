#ifndef AURALITE_NET_NET_H
#define AURALITE_NET_NET_H

#include <stdint.h>

/*
 * Minimal network stack for AuraLite OS.
 *
 * Implements just enough to satisfy the Phase 13 gate criterion (ping):
 *   Ethernet framing -> ARP resolution -> IPv4 -> ICMP echo request/reply.
 *
 * Our IP is 10.0.2.15 (QEMU user-mode networking default).
 * The gateway is 10.0.2.2 (QEMU's built-in host proxy).
 */

#define NET_OUR_IP    0x0A00020FUL   /* 10.0.2.15 in network byte order...    */
                                     /* Actually stored in host order; see net.c */
#define NET_GATEWAY   0x0A000202UL   /* 10.0.2.15 → gateway 10.0.2.2          */

/*
 * Perform a DHCP exchange (DISCOVER → OFFER → REQUEST → ACK) to obtain an
 * IP address, netmask, gateway, and DNS server from the DHCP server
 * (QEMU's built-in SLIRP). Updates our_ip and gateway_ip on success.
 * Returns 0 on success, -1 on failure.
 */
int net_dhcp(void);

/* Initialise the network stack.  Selects a NIC backend through the netdev
 * layer: e1000 by default, falling back to modern virtio-net when e1000 is
 * absent.  All frame I/O then goes through the active netdev.
 * Returns 0 when DHCP succeeded, >0 when fallback static addressing is active,
 * and <0 when the NIC/link is unavailable. */
int net_init(void);

/*
 * Send an ICMP echo request to the given IPv4 address (host byte order) and
 * poll for the echo reply.  Returns 0 on success (reply received), -1 on
 * failure (timeout or ARP failure).
 */
int net_ping(uint32_t target_ip);

/* Gate self-test: ARP-resolve 10.0.2.2, then ping it. */
void net_self_test(void);

/*
 * Resolve a hostname to an IPv4 address via QEMU's DNS proxy (10.0.2.3:53).
 * Uses UDP. Returns the IP in host byte order, or 0 on failure.
 */
uint32_t net_dns_resolve(const char *hostname);

/* UDP datagram helpers used by the socket layer. IP values are host order. */
int net_udp_sendto(uint32_t dst_ip, uint16_t dst_port,
                   uint16_t src_port, const void *data, uint32_t data_len);
int net_udp_recvfrom(uint16_t local_port, uint32_t *src_ip, uint16_t *src_port,
                     void *buf, uint32_t bufsize, uint64_t timeout_ticks);

/* DNS self-test: resolve 'example.com'. */
void net_dns_self_test(void);

#endif /* AURALITE_NET_NET_H */

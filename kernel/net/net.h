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

/* Initialise the network stack (calls e1000_init internally). */
int net_init(void);

/*
 * Send an ICMP echo request to the given IPv4 address (host byte order) and
 * poll for the echo reply.  Returns 0 on success (reply received), -1 on
 * failure (timeout or ARP failure).
 */
int net_ping(uint32_t target_ip);

/* Gate self-test: ARP-resolve 10.0.2.2, then ping it. */
void net_self_test(void);

#endif /* AURALITE_NET_NET_H */

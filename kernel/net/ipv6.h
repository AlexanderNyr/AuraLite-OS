/* ipv6.h — IPv6 network I/O for AuraLite OS.
 *
 * REALINTERNET_PLAN phase X7 (IPv6).  Builds on the pure address helpers in
 * ipv6_addr.{h,c} and on the existing netdev frame transport in net.c.
 * Implements the plan's primary gate: reach a link-local IPv6 neighbour with
 * ICMPv6 echo, resolving the neighbour MAC via Neighbor Discovery (NS/NA).
 *
 * The plan's larger ambitions (SLAAC/DHCPv6 for a global address, a full v6
 * socket family, happy-eyeballs across families) are out of this first
 * landing; they are recorded honestly in REALINTERNET_PLAN.md and docs/status.md.
 */

#ifndef AURALITE_NET_IPV6_H
#define AURALITE_NET_IPV6_H

#include "kernel/net/ipv6_addr.h"
#include <stdint.h>

/* Initialise IPv6 state: derive and print the link-local address from the
 * NIC MAC.  Called from net_init(). */
void net_ipv6_init(void);

/* The derived link-local address, or NULL if the stack is not up. */
const ipv6_addr_t *net_ipv6_linklocal(void);

/* R9 (ledger RES-24): the SLAAC address formed from an RA's
 * autonomous /64 prefix + our EUI-64 interface id, or NULL until a
 * Router Advertisement carried one. */
const ipv6_addr_t *net_ipv6_global(void);

/* Send an ICMPv6 echo request to `target` (resolving the neighbour MAC via
 * NDP first) and poll for the echo reply.  Returns 0 on success, -1 on
 * failure (ARP/NDP failure, TX error, or timeout).  Pinging our own
 * link-local address is answered as a loopback. */
int net_ping6(const ipv6_addr_t *target);

/* Process one received Ethernet frame.  If it is an ICMPv6 echo request
 * addressed to our link-local address (valid checksum), send an echo reply
 * and return 1 (consumed); otherwise return 0. */
int net_ipv6_handle_frame(const uint8_t *frame, int len);

/* Offline boot self-test: address parse/format round trips, EUI-64 link-local
 * derivation, ICMPv6 pseudo-header checksum against a reference vector. */
void net_ipv6_self_test(void);

/* Y3: the netl3 v6 ops call these.  resolve is ndp_resolve; src_for
 * is the R9 RFC 6724 floor (global src for a global dst).  Either
 * may return failure/NULL when the v6 stack is not up. */
int net_ipv6_resolve(const ipv6_addr_t *target, uint8_t out_mac[6]);
const ipv6_addr_t *net_ipv6_src_for(const ipv6_addr_t *dst);

#endif /* AURALITE_NET_IPV6_H */

/* netl3.c — kernel glue for the TCP/IP seam (REALINTERNET2 Y2/Y3).
 *
 * The wire format lives in netl3.h (host-tested).  This file is the
 * four helpers tcp.c used to call by name — ARP / NDP, our address,
 * netdev_send, ipfrag — wrapped as per-family ops.  i386 already
 * provides the v4 four symbols via netglue32.c; the v6 symbols are
 * stubbed there so the SHARED row keeps compiling (D3).  A v6
 * resolve on i386 fails closed.
 */

#include "kernel/net/netl3.h"
#include "kernel/net/net.h"
#include "kernel/net/netdev.h"
#include "kernel/net/ipv6.h"
#include "kernel/lib/string.h"

extern void     net_get_mac(uint8_t mac[6]);
extern uint32_t net_get_our_ip(void);
extern int      net_arp_resolve(uint32_t target_ip, uint8_t out_mac[6]);

static int netl3_v4_resolve(const netl3_addr_t *dst, uint8_t mac[6]) {
    if (!dst || dst->family != NETL3_AF_INET || !mac) return -1;
    return net_arp_resolve(netl3_v4_host(dst), mac);
}

static int netl3_v4_output(const netl3_addr_t *dst, uint8_t proto,
                           const void *l4, uint32_t l4_len) {
    uint8_t src_mac[6], dst_mac[6];
    uint8_t pkt[1518];
    uint32_t n;

    if (!dst || dst->family != NETL3_AF_INET || !l4) return -1;
    if (l4_len > 1500u - 20u) return -1;
    if (netl3_v4_resolve(dst, dst_mac) != 0) return -1;
    net_get_mac(src_mac);
    n = netl3_v4_build(pkt, sizeof pkt, src_mac, dst_mac,
                       net_get_our_ip(), netl3_v4_host(dst),
                       proto, l4, l4_len);
    if (n == 0) return -1;
    return netdev_send(pkt, n) < 0 ? -1 : 0;
}

static uint16_t netl3_v4_pseudo_op(const netl3_addr_t *src,
                                   const netl3_addr_t *dst,
                                   uint8_t proto, const void *l4,
                                   uint32_t l4_len) {
    if (!src || !dst || src->family != NETL3_AF_INET ||
        dst->family != NETL3_AF_INET) return 0;
    return netl3_v4_pseudo(netl3_v4_host(src), netl3_v4_host(dst),
                           proto, l4, l4_len);
}

static uint32_t netl3_v4_mss_op(void) { return netl3_v4_mss(); }

const struct netl3_ops netl3_v4_ops = {
    .resolve = netl3_v4_resolve,
    .output  = netl3_v4_output,
    .pseudo  = netl3_v4_pseudo_op,
    .mss     = netl3_v4_mss_op,
};

static int netl3_v6_resolve(const netl3_addr_t *dst, uint8_t mac[6]) {
    ipv6_addr_t a;
    if (!dst || dst->family != NETL3_AF_INET6 || !mac) return -1;
    memcpy(a.b, dst->addr, 16);
    return net_ipv6_resolve(&a, mac);
}

static int netl3_v6_output(const netl3_addr_t *dst, uint8_t proto,
                           const void *l4, uint32_t l4_len) {
    uint8_t src_mac[6], dst_mac[6];
    uint8_t pkt[1518];
    uint32_t n;
    ipv6_addr_t d;
    const ipv6_addr_t *src;

    if (!dst || dst->family != NETL3_AF_INET6 || !l4) return -1;
    if (l4_len > 1500u - 40u) return -1;
    memcpy(d.b, dst->addr, 16);
    src = net_ipv6_src_for(&d);
    if (!src) return -1;
    if (netl3_v6_resolve(dst, dst_mac) != 0) return -1;
    net_get_mac(src_mac);
    n = netl3_v6_build(pkt, sizeof pkt, src_mac, dst_mac,
                       src->b, dst->addr, proto, l4, l4_len);
    if (n == 0) return -1;
    return netdev_send(pkt, n) < 0 ? -1 : 0;
}

static uint16_t netl3_v6_pseudo_op(const netl3_addr_t *src,
                                   const netl3_addr_t *dst,
                                   uint8_t proto, const void *l4,
                                   uint32_t l4_len) {
    if (!src || !dst || src->family != NETL3_AF_INET6 ||
        dst->family != NETL3_AF_INET6) return 0;
    return netl3_v6_pseudo(src->addr, dst->addr, proto, l4, l4_len);
}

static uint32_t netl3_v6_mss_op(void) { return netl3_v6_mss(); }

const struct netl3_ops netl3_v6_ops = {
    .resolve = netl3_v6_resolve,
    .output  = netl3_v6_output,
    .pseudo  = netl3_v6_pseudo_op,
    .mss     = netl3_v6_mss_op,
};

const struct netl3_ops *netl3_ops_for(const netl3_addr_t *a) {
    if (!a) return 0;
    if (a->family == NETL3_AF_INET)  return &netl3_v4_ops;
    if (a->family == NETL3_AF_INET6) return &netl3_v6_ops;
    return 0;
}

int netl3_input(const uint8_t *frame, int len, netl3_pkt_t *out) {
    uint16_t etype;
    int fl = 0;
    const uint8_t *f;

    if (!frame || !out || len < 14) return -1;
    etype = (uint16_t)((frame[12] << 8) | frame[13]);
    if (etype == NETL3_ETH_IPV6)
        return netl3_v6_parse(frame, len, out);
    if (etype != NETL3_ETH_IP) return -1;
    /* X4 reassembly is v4-only: a v6 datagram never enters it. */
    f = net_ipfrag_step(frame, len, &fl);
    if (!f) return -1;
    return netl3_v4_parse(f, fl, out);
}

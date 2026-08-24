/* netl3.c — kernel glue for the TCP/IP seam (REALINTERNET2 Y2).
 *
 * The wire format lives in netl3.h (host-tested, byte-identical to
 * the pre-seam sender).  This file is the four helpers tcp.c used to
 * call by name — ARP, our address, netdev_send, ipfrag — wrapped as
 * the v4 ops.  i386 already provides those four symbols via
 * netglue32.c; this file is a KERNEL32_SHARED row so the port keeps
 * compiling against the seam unchanged (plan D3).
 */

#include "kernel/net/netl3.h"
#include "kernel/net/net.h"
#include "kernel/net/netdev.h"
#include "kernel/lib/string.h"

/* The four net.c helpers tcp.c used to declare itself.  Kept as
 * externs rather than added to net.h: the encapsulation comment in
 * tcp.c stood for a reason, and Y2 is a move, not a widening. */
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

const struct netl3_ops *netl3_ops_for(const netl3_addr_t *a) {
    if (a && a->family == NETL3_AF_INET) return &netl3_v4_ops;
    return 0;   /* Y3 fills NETL3_AF_INET6 */
}

int netl3_input(const uint8_t *frame, int len, netl3_pkt_t *out) {
    int fl = 0;
    const uint8_t *f;

    if (!frame || !out || len <= 0) return -1;
    /* X4 reassembly stays on this side of the seam: tcp.c no longer
     * names a fragment helper, and a v6 datagram will never enter
     * the v4 reassembly table. */
    f = net_ipfrag_step(frame, len, &fl);
    if (!f) return -1;
    return netl3_v4_parse(f, fl, out);
}

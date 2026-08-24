#ifndef AURALITE_NET_NETL3_H
#define AURALITE_NET_NETL3_H

/*
 * netl3 — the TCP / network-layer seam (REALINTERNET2 Y2).
 *
 * Before this file, tcp.c spelled the network layer inline: ethertype
 * 0x0800, a private IPv4 header, ARP resolve, the RFC 793 v4
 * pseudo-header and a hardcoded 1460-byte MSS.  That is the single
 * reason the transport cannot ride IPv6 (RES-26).  The transport now
 * talks to L3 through `struct netl3_ops`; today's v4 behaviour is the
 * first implementation, and the host A/B gate pins the wire
 * byte-identical to the pre-seam sender.
 *
 * Y3 adds the v6 implementation behind the SAME ops.  No tcp6.c
 * (plan D3).  The public tcp_open(uint32_t) ABI is unchanged; the
 * connection key inside tcp.c is already family + 16 bytes so Y3
 * does not have to widen it again.
 *
 * This header is portable and freestanding: no driver includes, no
 * arch includes, no libc.  The host unit test compiles it alone.
 */

#include <stdint.h>

/* Address-family numbers match libc <sys/socket.h> so a Y3 ABI
 * wiring cannot drift.  sockaddr_in6 itself stays out of libc until
 * Y3 (the opener pin). */
#define NETL3_AF_INET  2
#define NETL3_AF_INET6 10

#define NETL3_PROTO_TCP  6
#define NETL3_ETH_IP     0x0800u
#define NETL3_ETH_IPV6   0x86DDu
#define NETL3_V4_MSS     1460u    /* Ethernet payload − 20 − 20 */
#define NETL3_V6_MSS     1440u    /* Ethernet payload − 40 − 20 */
#define NETL3_V4_IDENT   3u       /* the pre-seam sender's constant */
#define NETL3_V4_TTL     64u
#define NETL3_V6_HOP     64u
#define NETL3_MIN_FRAME  60u

typedef struct {
    uint8_t family;     /* NETL3_AF_INET or NETL3_AF_INET6 */
    uint8_t addr[16];   /* v4 lives in addr[0..3] as octets, host reading
                         * via netl3_v4_host(); v6 uses all 16. */
} netl3_addr_t;

typedef struct {
    netl3_addr_t src;
    netl3_addr_t dst;
    uint8_t      proto;
    uint16_t     l3_hdr_len;   /* IHL*4 on v4 */
    uint16_t     l3_total;     /* IP total_length (host) */
    uint16_t     l4_off;       /* byte offset of L4 in the frame */
    const uint8_t *frame;
    int          frame_len;
} netl3_pkt_t;

/* The four ops the plan names.  input() is the shared ethertype
 * demux (not per-family): Y2 understands 0x0800, Y3 adds 0x86DD. */
struct netl3_ops {
    int      (*resolve)(const netl3_addr_t *dst, uint8_t mac[6]);
    int      (*output)(const netl3_addr_t *dst, uint8_t proto,
                       const void *l4, uint32_t l4_len);
    uint16_t (*pseudo)(const netl3_addr_t *src, const netl3_addr_t *dst,
                       uint8_t proto, const void *l4, uint32_t l4_len);
    uint32_t (*mss)(void);
};

/* ---- tiny freestanding helpers (no libc, no kernel string.h) -------- */

static inline uint16_t netl3_htons(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint16_t netl3_ntohs(uint16_t v) { return netl3_htons(v); }
static inline uint32_t netl3_htonl(uint32_t v) {
    return ((v >> 24) & 0xFFu) | ((v >> 8) & 0xFF00u) |
           ((v << 8) & 0xFF0000u) | ((v << 24) & 0xFF000000u);
}
static inline uint32_t netl3_ntohl(uint32_t v) { return netl3_htonl(v); }

static inline void netl3_copy(void *d, const void *s, uint32_t n) {
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    while (n--) *dd++ = *ss++;
}
static inline void netl3_zero(void *d, uint32_t n) {
    uint8_t *dd = (uint8_t *)d;
    while (n--) *dd++ = 0;
}

static inline netl3_addr_t netl3_addr_from_v4(uint32_t host) {
    netl3_addr_t a;
    netl3_zero(&a, sizeof a);
    a.family  = NETL3_AF_INET;
    a.addr[0] = (uint8_t)((host >> 24) & 0xFF);
    a.addr[1] = (uint8_t)((host >> 16) & 0xFF);
    a.addr[2] = (uint8_t)((host >>  8) & 0xFF);
    a.addr[3] = (uint8_t)(host & 0xFF);
    return a;
}

static inline uint32_t netl3_v4_host(const netl3_addr_t *a) {
    return ((uint32_t)a->addr[0] << 24) | ((uint32_t)a->addr[1] << 16) |
           ((uint32_t)a->addr[2] <<  8) |  (uint32_t)a->addr[3];
}

static inline netl3_addr_t netl3_addr_from_v6(const uint8_t b[16]) {
    netl3_addr_t a;
    netl3_zero(&a, sizeof a);
    a.family = NETL3_AF_INET6;
    if (b) netl3_copy(a.addr, b, 16);
    return a;
}

static inline int netl3_addr_eq(const netl3_addr_t *a, const netl3_addr_t *b) {
    uint32_t i;
    if (a->family != b->family) return 0;
    for (i = 0; i < 16; i++) if (a->addr[i] != b->addr[i]) return 0;
    return 1;
}

/* RFC 1071 fold.  Words are summed big-endian, matching the pre-seam
 * sender byte-for-byte (the A/B gate's whole point). */
static inline uint16_t netl3_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

static inline uint32_t netl3_sum_octets(const uint8_t *p, uint32_t len) {
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    return sum;
}

/* RFC 793 v4 pseudo-header + segment.  src/dst are HOST-order, the
 * same convention tcp.c used.  Returns the field already in network
 * byte order, ready to store in the TCP checksum slot. */
static inline uint16_t netl3_v4_pseudo(uint32_t src_host, uint32_t dst_host,
                                       uint8_t proto, const void *l4,
                                       uint32_t l4_len) {
    const uint8_t *p = (const uint8_t *)l4;
    uint32_t sum = 0;
    sum += (uint16_t)(((src_host >> 24) & 0xFF) << 8 | ((src_host >> 16) & 0xFF));
    sum += (uint16_t)(((src_host >>  8) & 0xFF) << 8 | (src_host & 0xFF));
    sum += (uint16_t)(((dst_host >> 24) & 0xFF) << 8 | ((dst_host >> 16) & 0xFF));
    sum += (uint16_t)(((dst_host >>  8) & 0xFF) << 8 | (dst_host & 0xFF));
    sum += proto;
    sum += (uint16_t)(((l4_len >> 8) & 0xFF) << 8 | (l4_len & 0xFF));
    sum += netl3_sum_octets(p, l4_len);
    return netl3_htons(netl3_fold(sum));
}

static inline uint16_t netl3_v4_ip_csum(const uint8_t *iphdr20) {
    return netl3_htons(netl3_fold(netl3_sum_octets(iphdr20, 20)));
}

static inline uint32_t netl3_v4_mss(void) { return NETL3_V4_MSS; }

/*
 * Build one Ethernet + IPv4 frame around an L4 segment.  The L4
 * checksum field (TCP offset 16) must be zero on entry; this writes
 * both the L4 pseudo-header checksum and the IPv4 header checksum.
 * Returns the on-wire length (padded to 60).  pkt_cap must be at
 * least 14+20+l4_len, and 60 if that is larger.
 *
 * Wire constants (ident=3, DF, TTL=64) are the pre-seam sender's —
 * changing any of them fails the A/B gate on purpose.
 */
static inline uint32_t netl3_v4_build(uint8_t *pkt, uint32_t pkt_cap,
                                      const uint8_t src_mac[6],
                                      const uint8_t dst_mac[6],
                                      uint32_t src_host, uint32_t dst_host,
                                      uint8_t proto,
                                      const void *l4, uint32_t l4_len) {
    uint32_t ip_total = 20u + l4_len;
    uint32_t frame_len = 14u + ip_total;
    uint8_t *ip;
    uint8_t *l4dst;
    uint32_t i;

    if (pkt_cap < frame_len) return 0;
    if (frame_len < NETL3_MIN_FRAME && pkt_cap < NETL3_MIN_FRAME) return 0;

    /* Ethernet. */
    netl3_copy(pkt + 0, dst_mac, 6);
    netl3_copy(pkt + 6, src_mac, 6);
    pkt[12] = (uint8_t)(NETL3_ETH_IP >> 8);
    pkt[13] = (uint8_t)(NETL3_ETH_IP & 0xFF);

    /* IPv4 header (20 bytes, IHL=5). */
    ip = pkt + 14;
    ip[0] = (uint8_t)((4 << 4) | 5);
    ip[1] = 0;
    ip[2] = (uint8_t)(ip_total >> 8);
    ip[3] = (uint8_t)(ip_total & 0xFF);
    ip[4] = (uint8_t)(NETL3_V4_IDENT >> 8);
    ip[5] = (uint8_t)(NETL3_V4_IDENT & 0xFF);
    ip[6] = 0x40;             /* DF */
    ip[7] = 0x00;
    ip[8] = (uint8_t)NETL3_V4_TTL;
    ip[9] = proto;
    ip[10] = 0;
    ip[11] = 0;
    ip[12] = (uint8_t)((src_host >> 24) & 0xFF);
    ip[13] = (uint8_t)((src_host >> 16) & 0xFF);
    ip[14] = (uint8_t)((src_host >>  8) & 0xFF);
    ip[15] = (uint8_t)(src_host & 0xFF);
    ip[16] = (uint8_t)((dst_host >> 24) & 0xFF);
    ip[17] = (uint8_t)((dst_host >> 16) & 0xFF);
    ip[18] = (uint8_t)((dst_host >>  8) & 0xFF);
    ip[19] = (uint8_t)(dst_host & 0xFF);

    /* L4, then fill the TCP/UDP checksum at offset 16 of the segment
     * (TCP's slot; UDP is the same offset).  The caller passed the
     * field as zero. */
    l4dst = pkt + 14 + 20;
    netl3_copy(l4dst, l4, l4_len);
    if (l4_len >= 18 && (proto == NETL3_PROTO_TCP || proto == 17)) {
        /* Same store the pre-seam sender did: `hdr->checksum = htons(~sum)`
         * into a packed uint16_t.  netl3_v4_pseudo already returns that
         * htons'd value; copying the two bytes is the LE/BE-honest form
         * of the assignment. */
        uint16_t cs = netl3_v4_pseudo(src_host, dst_host, proto, l4dst, l4_len);
        netl3_copy(l4dst + 16, &cs, 2);
    }

    {
        uint16_t ipc = netl3_v4_ip_csum(ip);
        netl3_copy(ip + 10, &ipc, 2);
    }

    if (frame_len < NETL3_MIN_FRAME) {
        for (i = frame_len; i < NETL3_MIN_FRAME; i++) pkt[i] = 0;
        frame_len = NETL3_MIN_FRAME;
    }
    return frame_len;
}

/* Parse one Ethernet frame as IPv4.  Returns 0 and fills *out, or -1
 * if this is not a v4 packet we can hand to L4.  Does NOT run
 * fragment reassembly — the kernel wrapper does that first. */
static inline int netl3_v4_parse(const uint8_t *frame, int len, netl3_pkt_t *out) {
    uint16_t etype, total, ihl;
    uint32_t src, dst;
    if (len < 14 + 20) return -1;
    etype = (uint16_t)((frame[12] << 8) | frame[13]);
    if (etype != NETL3_ETH_IP) return -1;
    if ((frame[14] >> 4) != 4) return -1;
    ihl = (uint16_t)((frame[14] & 0x0F) * 4);
    if (ihl < 20) return -1;
    if (len < 14 + (int)ihl) return -1;
    total = (uint16_t)((frame[16] << 8) | frame[17]);
    if (total < ihl) return -1;
    src = ((uint32_t)frame[26] << 24) | ((uint32_t)frame[27] << 16) |
          ((uint32_t)frame[28] <<  8) |  (uint32_t)frame[29];
    dst = ((uint32_t)frame[30] << 24) | ((uint32_t)frame[31] << 16) |
          ((uint32_t)frame[32] <<  8) |  (uint32_t)frame[33];
    out->src        = netl3_addr_from_v4(src);
    out->dst        = netl3_addr_from_v4(dst);
    out->proto      = frame[23];
    out->l3_hdr_len = ihl;
    out->l3_total   = total;
    out->l4_off     = (uint16_t)(14 + ihl);
    out->frame      = frame;
    out->frame_len  = len;
    return 0;
}

/* RFC 8200 s8.1 v6 pseudo-header + segment.  Returns the field
 * htons'd, the same store convention as netl3_v4_pseudo. */
static inline uint16_t netl3_v6_pseudo(const uint8_t src[16],
                                       const uint8_t dst[16],
                                       uint8_t proto, const void *l4,
                                       uint32_t l4_len) {
    uint8_t ph[40];
    uint32_t sum;
    netl3_copy(ph, src, 16);
    netl3_copy(ph + 16, dst, 16);
    ph[32] = (uint8_t)(l4_len >> 24);
    ph[33] = (uint8_t)(l4_len >> 16);
    ph[34] = (uint8_t)(l4_len >> 8);
    ph[35] = (uint8_t)l4_len;
    ph[36] = 0;
    ph[37] = 0;
    ph[38] = 0;
    ph[39] = proto;
    sum = netl3_sum_octets(ph, 40);
    sum += netl3_sum_octets((const uint8_t *)l4, l4_len);
    return netl3_htons(netl3_fold(sum));
}

static inline uint32_t netl3_v6_mss(void) { return NETL3_V6_MSS; }

/* Ethernet + IPv6 around an L4 segment.  No IPv6 header checksum.
 * l3_total is reported as 40+payload so tcp.c's payload arithmetic
 * (total − hdr − tcp_hdr) stays family-agnostic. */
static inline uint32_t netl3_v6_build(uint8_t *pkt, uint32_t pkt_cap,
                                      const uint8_t src_mac[6],
                                      const uint8_t dst_mac[6],
                                      const uint8_t src[16],
                                      const uint8_t dst[16],
                                      uint8_t proto,
                                      const void *l4, uint32_t l4_len) {
    uint32_t frame_len = 14u + 40u + l4_len;
    uint8_t *ip, *l4dst;
    uint32_t i;

    if (pkt_cap < frame_len) return 0;
    if (frame_len < NETL3_MIN_FRAME && pkt_cap < NETL3_MIN_FRAME) return 0;

    netl3_copy(pkt + 0, dst_mac, 6);
    netl3_copy(pkt + 6, src_mac, 6);
    pkt[12] = (uint8_t)(NETL3_ETH_IPV6 >> 8);
    pkt[13] = (uint8_t)(NETL3_ETH_IPV6 & 0xFF);

    ip = pkt + 14;
    netl3_zero(ip, 40);
    ip[0] = (uint8_t)(6 << 4);                 /* version 6 */
    ip[4] = (uint8_t)(l4_len >> 8);
    ip[5] = (uint8_t)(l4_len & 0xFF);
    ip[6] = proto;
    ip[7] = (uint8_t)NETL3_V6_HOP;
    netl3_copy(ip + 8, src, 16);
    netl3_copy(ip + 24, dst, 16);

    l4dst = pkt + 14 + 40;
    netl3_copy(l4dst, l4, l4_len);
    if (l4_len >= 18 && (proto == NETL3_PROTO_TCP || proto == 17)) {
        uint16_t cs = netl3_v6_pseudo(src, dst, proto, l4dst, l4_len);
        netl3_copy(l4dst + 16, &cs, 2);
    }

    if (frame_len < NETL3_MIN_FRAME) {
        for (i = frame_len; i < NETL3_MIN_FRAME; i++) pkt[i] = 0;
        frame_len = NETL3_MIN_FRAME;
    }
    return frame_len;
}

static inline int netl3_v6_parse(const uint8_t *frame, int len, netl3_pkt_t *out) {
    uint16_t etype, plen;
    if (len < 14 + 40) return -1;
    etype = (uint16_t)((frame[12] << 8) | frame[13]);
    if (etype != NETL3_ETH_IPV6) return -1;
    if ((frame[14] >> 4) != 6) return -1;
    plen = (uint16_t)((frame[18] << 8) | frame[19]);
    if (len < 14 + 40 + (int)plen) {
        /* short frame: still accept if we have the L4 header */
        if (len < 14 + 40 + 20) return -1;
    }
    out->src        = netl3_addr_from_v6(frame + 14 + 8);
    out->dst        = netl3_addr_from_v6(frame + 14 + 24);
    out->proto      = frame[14 + 6];
    out->l3_hdr_len = 40;
    out->l3_total   = (uint16_t)(40 + plen);
    out->l4_off     = 54;
    out->frame      = frame;
    out->frame_len  = len;
    return 0;
}

/* ---- kernel-side entry points (defined in netl3.c) ------------------- */

const struct netl3_ops *netl3_ops_for(const netl3_addr_t *a);
int  netl3_input(const uint8_t *frame, int len, netl3_pkt_t *out);
extern const struct netl3_ops netl3_v4_ops;
extern const struct netl3_ops netl3_v6_ops;

#endif /* AURALITE_NET_NETL3_H */

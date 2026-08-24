/* test_netl3.c — host gate for the Y2 TCP/IP seam
 * (REALINTERNET2 Y2; kernel/net/netl3.h).
 *
 * D2: QEMU/SLIRP cannot manufacture a pcap A/B on demand, so the
 * WIRE is proven here.  The "legacy" builder below is a verbatim
 * lift of the pre-seam tcp_send_segment_at framing (ident=3, DF,
 * TTL=64, RFC 793 v4 pseudo-header).  netl3_v4_build must produce
 * the same bytes or the phase has changed the wire.
 *
 * Also pinned: the address key (family + 16 bytes), parse of a
 * built frame, MSS hint, family reject, short-frame refuse.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../kernel/net/netl3.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, ...) do {                       \
        if (cond) { passed++; }                     \
        else {                                      \
            failed++;                               \
            printf("FAIL: ");                       \
            printf(__VA_ARGS__);                    \
            printf("\n");                           \
        }                                           \
    } while (0)

/* ---- verbatim pre-seam sender (the A side of the A/B) --------------- */

static uint16_t htons_(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static uint32_t htonl_(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}

struct legacy_iphdr {
    uint8_t  version_ihl;
    uint8_t  tos;
    uint16_t total_length;
    uint16_t ident;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed));

struct legacy_eth {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;
} __attribute__((packed));

static uint16_t legacy_tcp_csum(const void *tcp_seg, uint32_t tcp_len,
                                uint32_t src_ip_host, uint32_t dst_ip_host) {
    const uint8_t *p = (const uint8_t *)tcp_seg;
    uint32_t sum = 0;
    sum += (uint16_t)(((src_ip_host >> 24) & 0xFF) << 8 |
                      ((src_ip_host >> 16) & 0xFF));
    sum += (uint16_t)(((src_ip_host >> 8)  & 0xFF) << 8 |
                      (src_ip_host & 0xFF));
    sum += (uint16_t)(((dst_ip_host >> 24) & 0xFF) << 8 |
                      ((dst_ip_host >> 16) & 0xFF));
    sum += (uint16_t)(((dst_ip_host >> 8)  & 0xFF) << 8 |
                      (dst_ip_host & 0xFF));
    sum += 6;
    sum += (uint16_t)(((tcp_len >> 8) & 0xFF) << 8 | (tcp_len & 0xFF));
    uint32_t len = tcp_len;
    while (len > 1) {
        sum += (uint16_t)(p[0] << 8 | p[1]);
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons_((uint16_t)(~sum & 0xFFFF));
}

static uint32_t legacy_build(uint8_t *pkt,
                             const uint8_t src_mac[6], const uint8_t dst_mac[6],
                             uint32_t src_host, uint32_t dst_host,
                             const void *l4, uint32_t l4_len) {
    uint32_t ip_total = 20 + l4_len;
    uint32_t frame_len = 14 + ip_total;
    memcpy(pkt + 14 + 20, l4, l4_len);
    /* checksum over the copy so we can write it in place */
    {
        uint16_t *cslot = (uint16_t *)(pkt + 14 + 20 + 16);
        *cslot = 0;
        *cslot = legacy_tcp_csum(pkt + 14 + 20, l4_len, src_host, dst_host);
    }
    struct legacy_iphdr *ip = (struct legacy_iphdr *)(pkt + 14);
    ip->version_ihl  = (4 << 4) | 5;
    ip->tos          = 0;
    ip->total_length = htons_((uint16_t)ip_total);
    ip->ident        = htons_(3);
    ip->flags_frag   = htons_(0x4000);
    ip->ttl          = 64;
    ip->protocol     = 6;
    ip->checksum     = 0;
    ip->src_ip       = htonl_(src_host);
    ip->dst_ip       = htonl_(dst_host);
    {
        const uint8_t *p = (const uint8_t *)ip;
        uint32_t sum = 0;
        int i;
        for (i = 0; i < 20; i += 2) sum += (uint16_t)(p[i] << 8 | p[i + 1]);
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        ip->checksum = htons_((uint16_t)(~sum & 0xFFFF));
    }
    struct legacy_eth *eh = (struct legacy_eth *)pkt;
    memcpy(eh->dst_mac, dst_mac, 6);
    memcpy(eh->src_mac, src_mac, 6);
    eh->ethertype = htons_(0x0800);
    if (frame_len < 60) {
        memset(pkt + frame_len, 0, 60 - frame_len);
        frame_len = 60;
    }
    return frame_len;
}

/* A 20-byte TCP SYN, checksum left 0 — the common first segment. */
static void make_syn(uint8_t l4[20], uint16_t sport, uint16_t dport) {
    memset(l4, 0, 20);
    l4[0] = (uint8_t)(sport >> 8); l4[1] = (uint8_t)(sport & 0xFF);
    l4[2] = (uint8_t)(dport >> 8); l4[3] = (uint8_t)(dport & 0xFF);
    l4[4] = 0x00; l4[5] = 0x10; l4[6] = 0x00; l4[7] = 0x00; /* seq 0x1000 */
    l4[12] = 5 << 4;
    l4[13] = 0x02; /* SYN */
    l4[14] = 0xFA; l4[15] = 0xF0; /* window 64240 */
}

static void test_ab_syn(void) {
    uint8_t l4[20];
    uint8_t a[128], b[128];
    uint8_t smac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint8_t dmac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x00};
    uint32_t src = (10u << 24) | (0u << 16) | (2u << 8) | 15u;
    uint32_t dst = (10u << 24) | (0u << 16) | (2u << 8) |  3u;
    make_syn(l4, 40000, 53);
    uint32_t na = legacy_build(a, smac, dmac, src, dst, l4, 20);
    uint32_t nb = netl3_v4_build(b, sizeof b, smac, dmac, src, dst,
                                 NETL3_PROTO_TCP, l4, 20);
    CHECK(na == 60 && nb == 60, "SYN pads to 60 (got %u / %u)", na, nb);
    CHECK(na == nb && memcmp(a, b, na) == 0,
          "A/B SYN frame is byte-identical (%u bytes)", na);
}

static void test_ab_data(void) {
    uint8_t l4[20 + 17];
    uint8_t a[128], b[128];
    uint8_t smac[6] = {1,2,3,4,5,6};
    uint8_t dmac[6] = {6,5,4,3,2,1};
    uint32_t src = 0x0A00020Fu;
    uint32_t dst = 0x0A000202u;
    memset(l4, 0, sizeof l4);
    l4[12] = 5 << 4;
    l4[13] = 0x18; /* ACK|PSH */
    memcpy(l4 + 20, "PING-FROM-I386", 14);
    /* odd length: the checksum pad byte is the interesting case */
    uint32_t na = legacy_build(a, smac, dmac, src, dst, l4, 20 + 17);
    uint32_t nb = netl3_v4_build(b, sizeof b, smac, dmac, src, dst,
                                 NETL3_PROTO_TCP, l4, 20 + 17);
    CHECK(na == nb && na >= 14 + 20 + 37,
          "A/B data lengths match (%u / %u)", na, nb);
    CHECK(memcmp(a, b, na) == 0, "A/B data frame is byte-identical");
}

static void test_addr(void) {
    netl3_addr_t a = netl3_addr_from_v4(0x0A00020Fu);
    CHECK(a.family == NETL3_AF_INET, "v4 family is AF_INET (=2)");
    CHECK(a.family == 2, "family number matches libc AF_INET");
    CHECK(netl3_v4_host(&a) == 0x0A00020Fu, "v4 host round-trips");
    CHECK(a.addr[0] == 10 && a.addr[3] == 15, "octets are 10.x.x.15");
    CHECK(a.addr[4] == 0 && a.addr[15] == 0, "upper 12 bytes stay zero");
    netl3_addr_t b = netl3_addr_from_v4(0x0A00020Fu);
    netl3_addr_t c = netl3_addr_from_v4(0x0A000202u);
    CHECK(netl3_addr_eq(&a, &b), "equal addresses compare equal");
    CHECK(!netl3_addr_eq(&a, &c), "different addresses compare unequal");
    c.family = NETL3_AF_INET6;
    CHECK(!netl3_addr_eq(&a, &c), "family mismatch is unequal");
    CHECK(NETL3_AF_INET6 == 10, "INET6 number matches libc AF_INET6");
}

static void test_parse(void) {
    uint8_t l4[20], frame[128];
    uint8_t smac[6] = {0x02,0,0,0,0,0x01};
    uint8_t dmac[6] = {0x02,0,0,0,0,0x02};
    uint32_t src = (192u << 24) | (0u << 16) | (2u << 8) | 1u;
    uint32_t dst = (192u << 24) | (0u << 16) | (2u << 8) | 2u;
    netl3_pkt_t pkt;
    make_syn(l4, 1234, 80);
    uint32_t n = netl3_v4_build(frame, sizeof frame, smac, dmac, src, dst,
                                NETL3_PROTO_TCP, l4, 20);
    CHECK(netl3_v4_parse(frame, (int)n, &pkt) == 0, "parse accepts a built frame");
    CHECK(pkt.proto == NETL3_PROTO_TCP, "parsed proto is TCP");
    CHECK(pkt.l3_hdr_len == 20 && pkt.l4_off == 34, "IHL=5, L4 at 34");
    CHECK(pkt.l3_total == 40, "IP total = 20+20");
    CHECK(netl3_v4_host(&pkt.src) == src && netl3_v4_host(&pkt.dst) == dst,
          "parsed addresses match");
    CHECK(netl3_v4_parse(frame, 20, &pkt) != 0, "short frame is refused");
    frame[12] = 0x86; frame[13] = 0xDD;
    CHECK(netl3_v4_parse(frame, (int)n, &pkt) != 0,
          "v6 ethertype is not claimed by the v4 parser");
}

static void test_mss_and_ops_shape(void) {
    CHECK(netl3_v4_mss() == 1460, "v4 MSS hint is 1460");
    CHECK(NETL3_V4_IDENT == 3, "IP ident stays the pre-seam constant");
    /* The ops struct has exactly the four plan-named members.
     * Taking their offsets pins the shape against a silent reorder. */
    CHECK(sizeof(((struct netl3_ops *)0)->resolve) == sizeof(void *),
          "ops.resolve is a function pointer");
    CHECK(sizeof(((struct netl3_ops *)0)->output) == sizeof(void *),
          "ops.output is a function pointer");
    CHECK(sizeof(((struct netl3_ops *)0)->pseudo) == sizeof(void *),
          "ops.pseudo is a function pointer");
    CHECK(sizeof(((struct netl3_ops *)0)->mss) == sizeof(void *),
          "ops.mss is a function pointer");
    CHECK(sizeof(struct netl3_ops) == 4 * sizeof(void *),
          "ops is exactly the four plan-named members");
}

static void test_pseudo_matches_legacy(void) {
    uint8_t l4[20];
    make_syn(l4, 9, 80);
    uint32_t src = 0xC0A80001u, dst = 0x08080808u;
    uint16_t a = legacy_tcp_csum(l4, 20, src, dst);
    uint16_t b = netl3_v4_pseudo(src, dst, 6, l4, 20);
    CHECK(a == b, "pseudo-header checksum matches the pre-seam function");
}

static void test_v6(void) {
    uint8_t l4[20], frame[128];
    uint8_t smac[6] = {0x52,0x54,0,0x12,0x34,0x56};
    uint8_t dmac[6] = {0x52,0x54,0,0x12,0x34,0};
    uint8_t src[16], dst[16];
    netl3_addr_t a;
    netl3_pkt_t pkt;
    uint32_t n;

    memset(src, 0, 16); src[0] = 0xfe; src[1] = 0xc0; src[15] = 0x15;
    memset(dst, 0, 16); dst[0] = 0xfe; dst[1] = 0xc0; dst[15] = 0x02;
    a = netl3_addr_from_v6(dst);
    CHECK(a.family == NETL3_AF_INET6, "v6 family is 10");
    CHECK(a.addr[15] == 2 && a.addr[0] == 0xfe, "v6 octets land");
    CHECK(netl3_v6_mss() == 1440, "v6 MSS is 1440 (1500-40-20)");

    make_syn(l4, 40000, 8036);
    n = netl3_v6_build(frame, sizeof frame, smac, dmac, src, dst,
                       NETL3_PROTO_TCP, l4, 20);
    CHECK(n == 74, "v6 SYN is 14+40+20 (got %u)", n);
    CHECK(frame[12] == 0x86 && frame[13] == 0xDD, "ethertype is 0x86DD");
    CHECK((frame[14] >> 4) == 6, "IP version is 6");
    CHECK(frame[14 + 6] == 6, "next-header is TCP");
    CHECK(netl3_v6_parse(frame, (int)n, &pkt) == 0, "v6 parse accepts built frame");
    CHECK(pkt.l4_off == 54 && pkt.l3_hdr_len == 40, "L4 at 54, hdr 40");
    CHECK(pkt.proto == 6, "parsed proto is TCP");
    CHECK(pkt.src.family == NETL3_AF_INET6 && pkt.src.addr[15] == 0x15,
          "parsed src is the v6 we built");
    CHECK(netl3_v4_parse(frame, (int)n, &pkt) != 0,
          "v4 parser refuses a v6 frame");
}

int main(void) {
    test_addr();
    test_mss_and_ops_shape();
    test_pseudo_matches_legacy();
    test_ab_syn();
    test_ab_data();
    test_parse();
    test_v6();

    printf("test_netl3: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

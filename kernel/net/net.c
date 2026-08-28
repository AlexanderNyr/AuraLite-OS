/* net.c — minimal network stack: Ethernet + ARP + IPv4 + ICMP.
 *
 * All in one file for compactness; the layers are clearly separated.
 * Uses the e1000 driver for actual packet send/receive.
 */

#include <stdint.h>
#include "kernel/net/net.h"
#include "drivers/e1000/e1000.h"
#include "drivers/virtio_net/virtio_net.h"
#include "drivers/rtl8139/rtl8139.h"
#include "kernel/net/netdev.h"
#include "kernel/net/dns.h"
#include "kernel/net/ip_reasm.h"
#include "kernel/net/ipv6.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/lib/errno.h"

extern uint64_t timer_get_ticks(void);

/* ---- Protocol constants ---- */
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV4  0x0800

#define ARP_REQUEST  1
#define ARP_REPLY    2

#define IP_PROTO_ICMP  1
#define IP_PROTO_UDP  17
#define ICMP_ECHO_REQ  8
#define ICMP_ECHO_REP  0

/* Our IP = 10.0.2.15, gateway = 10.0.2.2 (host byte order). */
#define OUR_IP_O0 10
#define OUR_IP_O1 0
#define OUR_IP_O2 2
#define OUR_IP_O3 15

#define GW_IP_O0 10
#define GW_IP_O1 0
#define GW_IP_O2 2
#define GW_IP_O3 2

/* ---- Ethernet header (14 bytes) ---- */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct eth_hdr {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;   /* network byte order */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* ---- ARP packet (28 bytes over Ethernet) ---- */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct arp_pkt {
    uint16_t hw_type;    /* 1 = Ethernet */
    uint16_t proto_type; /* 0x0800 = IPv4 */
    uint8_t  hw_len;     /* 6 */
    uint8_t  proto_len;  /* 4 */
    uint16_t opcode;     /* 1 = request, 2 = reply */
    uint8_t  sender_mac[6];
    uint32_t sender_ip;  /* host byte order */
    uint8_t  target_mac[6];
    uint32_t target_ip;  /* host byte order */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* ---- IPv4 header (20 bytes, no options) ---- */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct ipv4_hdr {
    uint8_t  version_ihl;   /* (4<<4) | 5 */
    uint8_t  tos;
    uint16_t total_length;  /* network byte order */
    uint16_t ident;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;      /* network byte order */
    uint32_t src_ip;        /* network byte order */
    uint32_t dst_ip;        /* network byte order */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* ---- ICMP header (8 bytes + data) ---- */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;      /* network byte order */
    uint16_t ident;
    uint16_t seq;
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* ---- Receive wait budgets ----
 * Keep boot responsive on hypervisors with disconnected/unsupported virtual
 * networking. QEMU/VirtualBox/VMware NAT replies arrive quickly when link is
 * healthy; bounded waits avoid delaying boot when packets cannot be
 * transmitted or received. */
#define NET_ARP_TIMEOUT_TICKS        100
#define NET_ICMP_TIMEOUT_TICKS       200
#define NET_UDP_TIMEOUT_TICKS        200
/* TIMEFIX follow-up (2026-08-21): these were 500 ticks each -- which
 * the PIT mode-3 bug silently halved to 2.5 s real; once the clock ran
 * honestly, a failing DHCP burned a full 10 s of boot (measured on a
 * user's WHPX box: OFFER arrived, ACK never did, +5 s).  slirp answers
 * in single-digit milliseconds and real servers well under a second;
 * 3 s for the OFFER and 1.5 s for the ACK keep an order-of-magnitude
 * margin while making the honest-failure path cheap.  The fallback IP
 * already exists for exactly this case. */
#define NET_DHCP_OFFER_TIMEOUT_TICKS 300
#define NET_DHCP_ACK_TIMEOUT_TICKS   150

/* ---- State ---- */
static uint8_t  our_mac[6];
static uint32_t our_ip;
static uint32_t gateway_ip;
static uint32_t subnet_mask = 0;  /* set by DHCP */
/* Accessors for TCP (tcp.c). */
void net_get_mac(uint8_t mac[6]) {
    memcpy(mac, our_mac, 6);
}

uint32_t net_get_our_ip(void) {
    return our_ip;
}

/* ARP cache (single entry for the gateway). */
static uint8_t  gateway_mac[6];
static int      gateway_mac_known = 0;

/* ---- Byte-swap helpers ---- */
static uint16_t htons_(uint16_t v) { return (v >> 8) | (v << 8); }
static uint32_t htonl_(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}
static uint32_t ntohl_(uint32_t v) { return htonl_(v); }
static uint16_t ntohs_(uint16_t v) { return htons_(v); }

/* Pack 4 octets into a host-order uint32. */
static uint32_t ip_from_octets(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) |
           ((uint32_t)c << 8) | (uint32_t)d;
}

/* ---- Internet checksum (RFC 1071) ---- */
static uint16_t checksum(const void *data, uint32_t len) {
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint16_t)(p[0] << 8 | p[1]);
        p += 2;
        len -= 2;
    }
    if (len) {
        sum += p[0] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return htons_((uint16_t)(~sum & 0xFFFF));
}

/* ---- X4: IPv4 fragment reassembly (REALINTERNET_PLAN) ----
 * Pure engine: kernel/net/ip_reasm.c.  The kernel holds ONE table and ONE
 * staging frame; every receive loop calls net_ipfrag_step() before parsing.
 * A frame returned unchanged is parsed as before; a NULL return means
 * "consumed fragment - keep waiting".  On completion the caller gets a
 * synthetic full frame (fixed total_length, recomputed header checksum) so
 * every protocol parser sees the reassembled datagram exactly like an
 * unfragmented one.  Bounded: 8 datagrams x 8 KiB, 10 s timeout, first-win
 * overlap policy - a stranger can hold at most ~72 KiB for 10 seconds. */

#define NET_IPFRAG_TIMEOUT_MS 10000u

static ipreasm_t g_ipreasm;
static int       g_ipreasm_inited = 0;
static uint8_t   g_ipfrag_frame[14 + 20 + IPREASM_CAP];

static uint32_t net_now_ms(void) {
    return (uint32_t)(timer_get_ticks() * 10u);   /* 100 Hz ticks */
}

const uint8_t *net_ipfrag_step(const uint8_t *frame, int len, int *out_len) {
    if (out_len) *out_len = len;
    if (!g_ipreasm_inited) { ipreasm_init(&g_ipreasm); g_ipreasm_inited = 1; }
    if (len < 14 + 20) return frame;
    {
        uint32_t now = net_now_ms();
        int dropped = ipreasm_sweep(&g_ipreasm, now, NET_IPFRAG_TIMEOUT_MS);
        if (dropped > 0)
            kprintf("[ipfrag] timeout: dropped %d incomplete datagram(s)\n", dropped);
    }
    const struct eth_hdr *eh = (const struct eth_hdr *)frame;
    if (htons_(eh->ethertype) != ETHERTYPE_IPV4) return frame;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)(frame + 14);
    if ((ip->version_ihl & 0x0F) != 5) return frame;   /* options: legacy path */
    uint16_t ff = htons_(ip->flags_frag);
    int mf = (ff & 0x2000) != 0;
    uint16_t off_units = (uint16_t)(ff & 0x1FFF);
    if (!mf && off_units == 0) return frame;           /* fast path: unfragmented */

    uint16_t total = htons_(ip->total_length);
    if (total < 20 || (uint32_t)(14 + total) > (uint32_t)len)
        return 0;                                     /* malformed fragment */
    uint16_t plen = (uint16_t)(total - 20);

    ipreasm_key_t key;
    key.src = ip->src_ip; key.dst = ip->dst_ip;
    key.proto = ip->protocol; key.id = ip->ident;
    uint16_t expected = mf ? 0 : (uint16_t)(off_units * 8 + plen);
    uint32_t before_overlap = g_ipreasm.n_overlap_refused;
    uint32_t before_cap     = g_ipreasm.n_cap_refused;
    uint32_t before_evict   = g_ipreasm.n_evicted;
    uint16_t full_len = 0;
    int rc = ipreasm_input(&g_ipreasm, net_now_ms(), NET_IPFRAG_TIMEOUT_MS,
                           &key, expected, (uint16_t)(off_units * 8),
                           frame + 14 + 20, plen,
                           g_ipfrag_frame + 34, IPREASM_CAP, &full_len);
    if (g_ipreasm.n_evicted != before_evict)
        kprintf("[ipfrag] table full: evicted oldest incomplete datagram\n");
    if (rc == IPREASM_REFUSED) {
        if (g_ipreasm.n_overlap_refused != before_overlap)
            kprintf("[ipfrag] refused overlapping/conflicting fragment (id %u)\n",
                    htons_(ip->ident));
        else if (g_ipreasm.n_cap_refused != before_cap)
            kprintf("[ipfrag] refused fragment beyond %u-byte cap (id %u)\n",
                    IPREASM_CAP, htons_(ip->ident));
        else
            kprintf("[ipfrag] refused malformed fragment (id %u)\n",
                    htons_(ip->ident));
        return 0;
    }
    if (rc == IPREASM_PENDING) {
        if (off_units == 0)
            kprintf("[ipfrag] reassembly started: id %u proto %u\n",
                    htons_(ip->ident), ip->protocol);
        return 0;
    }

    /* COMPLETE: build a synthetic full frame for the caller to parse. */
    memcpy(g_ipfrag_frame, frame, 14);
    memcpy(g_ipfrag_frame + 14, ip, 20);
    struct ipv4_hdr *nip = (struct ipv4_hdr *)(g_ipfrag_frame + 14);
    nip->total_length = htons_((uint16_t)(20 + full_len));
    nip->flags_frag = 0;
    nip->checksum = 0;
    nip->checksum = checksum(nip, 20);
    kprintf("[ipfrag] complete: %u-byte datagram reassembled (id %u)\n",
            full_len, htons_(ip->ident));
    if (out_len) *out_len = 14 + 20 + (int)full_len;
    return g_ipfrag_frame;
}

void net_ipfrag_sweep(void) {
    if (g_ipreasm_inited)
        ipreasm_sweep(&g_ipreasm, net_now_ms(), NET_IPFRAG_TIMEOUT_MS);
}

static int net_recv_wait_until(void *buf, uint32_t bufsize, uint64_t deadline_ticks) {
    uint64_t now = timer_get_ticks();
    if (deadline_ticks <= now) {
        return 0;
    }
    return netdev_recv_wait(buf, bufsize, deadline_ticks - now);
}

/* ---- Ethernet send: wrap payload in an Ethernet frame and transmit. ---- */
int net_eth_send(const uint8_t dst_mac[6], uint16_t ethertype,
                 const void *payload, uint32_t plen) {
    uint8_t frame[1518];
    struct eth_hdr *eh = (struct eth_hdr *)frame;
    memcpy(eh->dst_mac, dst_mac, 6);
    memcpy(eh->src_mac, our_mac, 6);
    eh->ethertype = htons_(ethertype);
    memcpy(frame + 14, payload, plen);
    uint32_t total = 14 + plen;
    if (total < 60) {
        memset(frame + total, 0, 60 - total);
        total = 60;   /* minimum Ethernet frame size */
    }
    return netdev_send(frame, total);
}

/* ---- ARP: resolve an IP address to a MAC. ---- */
int net_arp_resolve(uint32_t target_ip, uint8_t out_mac[6]) {
    /* If the target is NOT on our local subnet, route through the gateway. */
    if (subnet_mask != 0 && (target_ip & subnet_mask) != (our_ip & subnet_mask)) {
        /* Target is remote — use the gateway's MAC. */
        if (!gateway_mac_known) {
            /* Resolve the gateway MAC first. */
            uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            struct arp_pkt arp;
            arp.hw_type    = htons_(1);
            arp.proto_type = htons_(0x0800);
            arp.hw_len     = 6;
            arp.proto_len  = 4;
            arp.opcode     = htons_(ARP_REQUEST);
            memcpy(arp.sender_mac, our_mac, 6);
            arp.sender_ip  = htonl_(our_ip);
            memset(arp.target_mac, 0, 6);
            arp.target_ip  = htonl_(gateway_ip);

            if (net_eth_send(broadcast, ETHERTYPE_ARP, &arp, sizeof(arp)) < 0) {
                kprintf("[net] ARP gateway request TX failed\n");
                return -1;
            }
            kprintf("[net] ARP (gateway) for %u.%u.%u.%u\n",
                    (gateway_ip >> 24) & 0xFF, (gateway_ip >> 16) & 0xFF,
                    (gateway_ip >> 8) & 0xFF, gateway_ip & 0xFF);

            uint8_t buf[2048];
            uint64_t deadline = timer_get_ticks() + NET_ARP_TIMEOUT_TICKS;
            while (timer_get_ticks() < deadline) {
                int n = net_recv_wait_until(buf, sizeof(buf), deadline);
                if (n <= 0) break;
                if (n < (int)(14 + sizeof(struct arp_pkt))) continue;
                struct eth_hdr *eh = (struct eth_hdr *)buf;
                if (htons_(eh->ethertype) != ETHERTYPE_ARP) continue;
                struct arp_pkt *rp = (struct arp_pkt *)(buf + 14);
                if (htons_(rp->opcode) != ARP_REPLY) continue;
                memcpy(gateway_mac, rp->sender_mac, 6);
                gateway_mac_known = 1;
                kprintf("[net] ARP reply: gateway is %02x:%02x:%02x:%02x:%02x:%02x\n",
                        gateway_mac[0], gateway_mac[1], gateway_mac[2],
                        gateway_mac[3], gateway_mac[4], gateway_mac[5]);
                break;
            }
        }
        if (gateway_mac_known) {
            memcpy(out_mac, gateway_mac, 6);
            return 0;
        }
        return -1;
    }

    /* Local subnet: resolve directly. */
    /* If we already know it, return immediately. */
    if (target_ip == gateway_ip && gateway_mac_known) {
        memcpy(out_mac, gateway_mac, 6);
        return 0;
    }

    /* Build and send an ARP request. */
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    struct arp_pkt arp;
    arp.hw_type    = htons_(1);
    arp.proto_type = htons_(0x0800);
    arp.hw_len     = 6;
    arp.proto_len  = 4;
    arp.opcode     = htons_(ARP_REQUEST);
    memcpy(arp.sender_mac, our_mac, 6);
    arp.sender_ip  = htonl_(our_ip);
    memset(arp.target_mac, 0, 6);
    arp.target_ip  = htonl_(target_ip);

    if (net_eth_send(broadcast, ETHERTYPE_ARP, &arp, sizeof(arp)) < 0) {
        kprintf("[net] ARP request TX failed for %u.%u.%u.%u\n",
                (target_ip >> 24) & 0xFF, (target_ip >> 16) & 0xFF,
                (target_ip >> 8) & 0xFF, target_ip & 0xFF);
        return -1;
    }
    kprintf("[net] ARP request sent for %u.%u.%u.%u\n",
            (target_ip >> 24) & 0xFF, (target_ip >> 16) & 0xFF,
            (target_ip >> 8) & 0xFF, target_ip & 0xFF);

    /* Wait for the ARP reply. */
    uint8_t buf[2048];
    uint64_t deadline = timer_get_ticks() + NET_ARP_TIMEOUT_TICKS;
    while (timer_get_ticks() < deadline) {
        int n = net_recv_wait_until(buf, sizeof(buf), deadline);
        if (n <= 0) break;
        if (n < (int)(14 + sizeof(struct arp_pkt))) {
            continue;
        }
        struct eth_hdr *eh = (struct eth_hdr *)buf;
        uint16_t et = htons_(eh->ethertype);
        if (et != ETHERTYPE_ARP) {
            continue;
        }
        struct arp_pkt *rp = (struct arp_pkt *)(buf + 14);
        if (htons_(rp->opcode) != ARP_REPLY) {
            continue;
        }
        /* Got it! */
        memcpy(out_mac, rp->sender_mac, 6);
        if (target_ip == gateway_ip) {
            memcpy(gateway_mac, out_mac, 6);
            gateway_mac_known = 1;
        }
        kprintf("[net] ARP reply: %u.%u.%u.%u is "
                "%02x:%02x:%02x:%02x:%02x:%02x\n",
                (target_ip >> 24) & 0xFF, (target_ip >> 16) & 0xFF,
                (target_ip >> 8) & 0xFF, target_ip & 0xFF,
                out_mac[0], out_mac[1], out_mac[2],
                out_mac[3], out_mac[4], out_mac[5]);
        return 0;
    }
    kprintf("[net] ARP timeout for %u.%u.%u.%u\n",
            (target_ip >> 24) & 0xFF, (target_ip >> 16) & 0xFF,
            (target_ip >> 8) & 0xFF, target_ip & 0xFF);
    return -1;
}

/* ---- ICMP: send an echo request and poll for the reply. ---- */
int net_ping(uint32_t target_ip) {
    /* 1) Resolve the MAC via ARP. */
    uint8_t dst_mac[6];
    if (net_arp_resolve(target_ip, dst_mac) != 0) {
        kprintf("[net] ping: ARP resolution failed\n");
        return -1;
    }

    /* 2) Build the ICMP echo request. */
    uint8_t pkt[14 + 20 + 8 + 32];   /* eth + ip + icmp + data */
    uint8_t *icmp_data = pkt + 14 + 20 + 8;
    /* Fill ICMP payload with a recognisable pattern. */
    for (int i = 0; i < 32; i++) {
        icmp_data[i] = (uint8_t)(i + 0x41);
    }

    struct icmp_hdr *icmp = (struct icmp_hdr *)(pkt + 14 + 20);
    icmp->type     = ICMP_ECHO_REQ;
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->ident    = htons_(0x1234);
    icmp->seq      = htons_(1);
    icmp->checksum = checksum(icmp, 8 + 32);

    /* 3) Build the IPv4 header. */
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(pkt + 14);
    ip->version_ihl = (4 << 4) | 5;
    ip->tos         = 0;
    ip->total_length= htons_(20 + 8 + 32);
    ip->ident       = htons_(1);
    ip->flags_frag  = 0;
    ip->ttl         = 64;
    ip->protocol    = IP_PROTO_ICMP;
    ip->checksum    = 0;
    ip->src_ip      = htonl_(our_ip);
    ip->dst_ip      = htonl_(target_ip);
    ip->checksum    = checksum(ip, 20);

    /* 4) Build the Ethernet header and send. */
    struct eth_hdr *eh = (struct eth_hdr *)pkt;
    memcpy(eh->dst_mac, dst_mac, 6);
    memcpy(eh->src_mac, our_mac, 6);
    eh->ethertype = htons_(ETHERTYPE_IPV4);

    if (netdev_send(pkt, 14 + 20 + 8 + 32) < 0) {
        kprintf("[net] ICMP echo request TX failed\n");
        return -1;
    }
    kprintf("[net] ICMP echo request sent to %u.%u.%u.%u\n",
            (target_ip >> 24) & 0xFF, (target_ip >> 16) & 0xFF,
            (target_ip >> 8) & 0xFF, target_ip & 0xFF);

    /* 5) Wait for the ICMP echo reply. */
    uint8_t buf[2048];
    uint64_t deadline = timer_get_ticks() + NET_ICMP_TIMEOUT_TICKS;
    while (timer_get_ticks() < deadline) {
        int n = net_recv_wait_until(buf, sizeof(buf), deadline);
        if (n <= 0) break;
        int fl = 0;
        const uint8_t *f = net_ipfrag_step(buf, n, &fl);    /* X4 reassembly */
        if (!f) continue;
        if (fl < (int)(14 + 20 + 8)) {
            continue;
        }
        struct eth_hdr *reh = (struct eth_hdr *)f;
        if (htons_(reh->ethertype) != ETHERTYPE_IPV4) {
            continue;
        }
        struct ipv4_hdr *rip = (struct ipv4_hdr *)(f + 14);
        if (rip->protocol != IP_PROTO_ICMP) {
            continue;
        }
        struct icmp_hdr *ricmp = (struct icmp_hdr *)(f + 14 + 20);
        if (ricmp->type != ICMP_ECHO_REP) {
            continue;
        }
        kprintf("[net] ICMP echo reply received from %u.%u.%u.%u (seq %u)\n",
                (target_ip >> 24) & 0xFF, (target_ip >> 16) & 0xFF,
                (target_ip >> 8) & 0xFF, target_ip & 0xFF,
                htons_(ricmp->seq));
        return 0;
    }
    kprintf("[net] ICMP echo reply timeout\n");
    return -1;
}

/* ---- UDP header (8 bytes) ---- */
#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;    /* header + data, network byte order */
    uint16_t checksum;  /* 0 = no checksum (legal for IPv4/UDP) */
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/*
 * Send a UDP datagram to dst_ip:dst_port with the given payload.
 * Resolves the MAC via ARP, builds Ethernet + IPv4 + UDP, and transmits.
 * Returns 0 on success, -1 on failure.
 */
int net_udp_sendto(uint32_t dst_ip, uint16_t dst_port,
                   uint16_t src_port, const void *data, uint32_t data_len) {
    uint8_t dst_mac[6];
    if (net_arp_resolve(dst_ip, dst_mac) != 0) {
        return -EHOSTUNREACH;   /* FIX_R7: unresolvable neighbour */
    }

    if (data_len > 1472) {
        return -EINVAL;   /* FIX_R7: datagram too large for one frame */
    }

    uint32_t udp_total = 8 + data_len;
    uint32_t ip_total  = 20 + udp_total;
    uint32_t frame_len = 14 + ip_total;

    uint8_t pkt[1518];
    /* Ethernet header. */
    struct eth_hdr *eh = (struct eth_hdr *)pkt;
    memcpy(eh->dst_mac, dst_mac, 6);
    memcpy(eh->src_mac, our_mac, 6);
    eh->ethertype = htons_(ETHERTYPE_IPV4);

    /* IPv4 header. */
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(pkt + 14);
    ip->version_ihl = (4 << 4) | 5;
    ip->tos         = 0;
    ip->total_length= htons_((uint16_t)ip_total);
    ip->ident       = htons_(2);
    ip->flags_frag  = 0;
    ip->ttl         = 64;
    ip->protocol    = IP_PROTO_UDP;
    ip->checksum    = 0;
    ip->src_ip      = htonl_(our_ip);
    ip->dst_ip      = htonl_(dst_ip);
    ip->checksum    = checksum(ip, 20);

    /* UDP header. */
    struct udp_hdr *udp = (struct udp_hdr *)(pkt + 14 + 20);
    udp->src_port = htons_(src_port);
    udp->dst_port = htons_(dst_port);
    udp->length   = htons_((uint16_t)udp_total);
    udp->checksum = 0;   /* no checksum (legal for IPv4) */

    /* Payload. */
    memcpy(pkt + 14 + 20 + 8, data, data_len);

    /* Pad to minimum Ethernet frame. */
    if (frame_len < 60) {
        memset(pkt + frame_len, 0, 60 - frame_len);
        frame_len = 60;
    }
    if (netdev_send(pkt, frame_len) < 0) {
        return -1;
    }
    return 0;
}

/*
 * Wait for a UDP packet from dst_ip:dst_port. Copies up to bufsize bytes of
 * the UDP payload into buf. Returns payload length, or -1 on timeout.
 */
int net_udp_recvfrom(uint16_t local_port, uint32_t *src_ip, uint16_t *src_port,
                     void *buf, uint32_t bufsize, uint64_t timeout_ticks) {
    uint8_t rbuf[2048];
    uint64_t deadline = timer_get_ticks() + (timeout_ticks ? timeout_ticks : NET_UDP_TIMEOUT_TICKS);
    while (timer_get_ticks() < deadline) {
        int n = net_recv_wait_until(rbuf, sizeof(rbuf), deadline);
        if (n <= 0) break;
        int fl = 0;
        const uint8_t *f = net_ipfrag_step(rbuf, n, &fl);   /* X4 reassembly */
        if (!f) continue;
        if (fl < (int)(14 + 20 + 8)) continue;
        struct eth_hdr *eh = (struct eth_hdr *)f;
        if (htons_(eh->ethertype) != ETHERTYPE_IPV4) continue;
        struct ipv4_hdr *ip = (struct ipv4_hdr *)(f + 14);
        if (ip->protocol != IP_PROTO_UDP) continue;
        struct udp_hdr *udp = (struct udp_hdr *)(f + 14 + 20);
        if (htons_(udp->dst_port) != local_port) continue;

        uint16_t udp_len = htons_(udp->length);
        if (udp_len < 8) return -1;
        uint16_t payload_len = udp_len - 8;
        if ((uint32_t)(14 + 20 + 8 + payload_len) > (uint32_t)fl) continue;
        if (payload_len > bufsize) payload_len = (uint16_t)bufsize;
        memcpy(buf, f + 14 + 20 + 8, payload_len);
        if (src_ip) *src_ip = htonl_(ip->src_ip);
        if (src_port) *src_port = htons_(udp->src_port);
        return payload_len;
    }
    return -1;
}

static int net_udp_recv(uint32_t src_ip, uint16_t src_port,
                        void *buf, uint32_t bufsize) {
    uint32_t got_ip = 0;
    uint16_t got_port = 0;
    int n = net_udp_recvfrom(12345, &got_ip, &got_port, buf, bufsize,
                             NET_UDP_TIMEOUT_TICKS);
    if (n < 0) return -1;
    if (got_ip != src_ip || got_port != src_port) return -1;
    return n;
}

/* ---- DNS resolver ----
 *
 * The resolver moved to kernel/net/dns.c in REALINTERNET_PLAN phase X3:
 * TTL cache (positive + negative), secondary-server failover and CNAME
 * chain following live there; the pure wire-format parser and cache core
 * (host-unit-tested) are in kernel/net/dns_parse.c.  The public entry
 * points from net.h (net_dns_resolve / net_dns_self_test) are kept and
 * forward to the new module, so the syscall layer and shell are unchanged.
 */

/* ---- DHCP client ---- */

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_BOOTREQUEST  1
#define DHCP_BOOTREPLY    2

/* DHCP message types. */
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

/* DHCP options. */
#define DHCP_OPT_SUBNET_MASK  1
#define DHCP_OPT_ROUTER       3
#define DHCP_OPT_DNS_SERVER   6
#define DHCP_OPT_REQUESTED_IP 50
#define DHCP_OPT_LEASE_TIME   51
#define DHCP_OPT_MSG_TYPE     53
#define DHCP_OPT_SERVER_ID    54
#define DHCP_OPT_PARAM_LIST   55
#define DHCP_OPT_END          255

/* The DHCP magic cookie: identifies DHCP (vs plain BOOTP). */
#define DHCP_MAGIC_COOKIE 0x63825363

/*
 * DHCP packet layout (RFC 2131):
 *   op(1), htype(1), hlen(1), hops(1), xid(4), secs(2), flags(2),
 *   ciaddr(4), yiaddr(4), siaddr(4), giaddr(4),
 *   chaddr(16), sname(64), file(128), cookie(4), options(variable)
 */
#define DHCP_MIN_PKT_SIZE 300

#if defined(__TINYC__)
#pragma pack(push, 1)
#endif
struct dhcp_pkt {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t cookie;
    uint8_t  options[0];
} __attribute__((packed));
#if defined(__TINYC__)
#pragma pack(pop)
#endif

/* Append a DHCP option to `buf` at position `pos`. Returns new pos. */
static int dhcp_add_option(uint8_t *buf, int pos, uint8_t code,
                           const void *data, uint8_t data_len) {
    buf[pos++] = code;
    if (code != DHCP_OPT_END) {
        buf[pos++] = data_len;
        memcpy(buf + pos, data, data_len);
        pos += data_len;
    }
    return pos;
}

/* Find a DHCP option in the options field. Returns pointer to its value
 * (after the length byte), or NULL if not found. Sets *out_len. */
static const uint8_t *dhcp_find_option(const uint8_t *opts, int opts_len,
                                       uint8_t code, int *out_len) {
    int i = 0;
    while (i < opts_len) {
        uint8_t c = opts[i];
        if (c == DHCP_OPT_END) break;
        if (c == 0) { i++; continue; }   /* padding */
        if (i + 1 >= opts_len) break;
        uint8_t l = opts[i + 1];
        if (c == code) {
            if (out_len) *out_len = l;
            return opts + i + 2;
        }
        i += 2 + l;
    }
    return NULL;
}

/* Helper to extract a uint32 from a big-endian byte array (for IP options). */
static uint32_t be32_to_host(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int net_dhcp(void) {
    kprintf("[dhcp] starting DHCP discovery...\n");

    /* The DHCP server is at the broadcast address (255.255.255.255).
     * At the Ethernet layer, we broadcast to FF:FF:FF:FF:FF:FF. */
    uint32_t dhcp_server_ip = ip_from_octets(255, 255, 255, 255);
    uint32_t client_ip      = ip_from_octets(0, 0, 0, 0);
    uint32_t dhcp_xid       = 0x12345678;

    /* --- Step 1: Send DHCPDISCOVER --- */
    uint8_t discover[576];
    memset(discover, 0, sizeof(discover));
    struct dhcp_pkt *dhcp = (struct dhcp_pkt *)discover;
    dhcp->op    = DHCP_BOOTREQUEST;
    dhcp->htype = 1;        /* Ethernet */
    dhcp->hlen  = 6;        /* MAC address length */
    dhcp->xid   = htonl_(dhcp_xid);
    dhcp->flags = htons_(0x8000);   /* broadcast reply requested */
    memcpy(dhcp->chaddr, our_mac, 6);
    dhcp->cookie = htonl_(DHCP_MAGIC_COOKIE);

    /* Options. */
    int opt_pos = 0;
    uint8_t msg_type = DHCP_DISCOVER;
    opt_pos = dhcp_add_option(dhcp->options, opt_pos, DHCP_OPT_MSG_TYPE,
                              &msg_type, 1);
    /* Parameter request list: subnet mask, router, DNS. */
    uint8_t param_list[] = { DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER,
                             DHCP_OPT_DNS_SERVER };
    opt_pos = dhcp_add_option(dhcp->options, opt_pos, DHCP_OPT_PARAM_LIST,
                              param_list, sizeof(param_list));
    dhcp->options[opt_pos++] = DHCP_OPT_END;

    /* For DISCOVER, the IP layer src must be 0.0.0.0 and dst broadcast.
     * But our net_udp_send does ARP for the dst — broadcast won't ARP.
     * So we build the raw Ethernet+IP+UDP frame ourselves. */
    {
        uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        uint32_t udp_len = 8 + sizeof(struct dhcp_pkt) + opt_pos;
        uint32_t ip_len  = 20 + udp_len;
        uint32_t frame_len = 14 + ip_len;

        uint8_t frame[700];
        /* Ethernet. */
        struct eth_hdr *eh = (struct eth_hdr *)frame;
        memcpy(eh->dst_mac, bcast_mac, 6);
        memcpy(eh->src_mac, our_mac, 6);
        eh->ethertype = htons_(ETHERTYPE_IPV4);

        /* IP.  R9 CATCH: tos and flags_frag were never written --
         * stack garbage rode in them since the first DHCP landing,
         * and the header checksum BLESSED the garbage (computed
         * over it).  The boot happened to leave zeros there until
         * the R9 ipv6 work reshaped the stack: then tos=0xFF,
         * frag-offset=65528 -- and SLIRP dropped the DISCOVER as a
         * mid-stream fragment.  pcap -v named it; every field is
         * now written. */
        struct ipv4_hdr *ip = (struct ipv4_hdr *)(frame + 14);
        ip->version_ihl = (4 << 4) | 5;
        ip->tos         = 0;
        ip->total_length= htons_((uint16_t)ip_len);
        ip->ident       = htons_(0x1234);
        ip->flags_frag  = 0;
        ip->ttl         = 64;
        ip->protocol    = IP_PROTO_UDP;
        ip->src_ip      = htonl_(client_ip);      /* 0.0.0.0 */
        ip->dst_ip      = htonl_(dhcp_server_ip); /* 255.255.255.255 */
        ip->checksum    = 0;
        ip->checksum    = checksum(ip, 20);

        /* UDP. */
        struct udp_hdr *udp = (struct udp_hdr *)(frame + 14 + 20);
        udp->src_port = htons_(DHCP_CLIENT_PORT);
        udp->dst_port = htons_(DHCP_SERVER_PORT);
        udp->length   = htons_((uint16_t)udp_len);
        udp->checksum = 0;

        /* DHCP payload. */
        memcpy(frame + 14 + 20 + 8, discover, sizeof(struct dhcp_pkt) + opt_pos);

        if (frame_len < 60) {
            memset(frame + frame_len, 0, 60 - frame_len);
            frame_len = 60;
        }
        if (netdev_send(frame, frame_len) < 0) {
            kprintf("[dhcp] FAIL: DISCOVER transmit failed (link down or TX timeout)\n");
            return -1;
        }
    }
    kprintf("[dhcp] DISCOVER sent (xid=0x%08x)\n", dhcp_xid);

    /* --- Step 2: Wait for DHCPOFFER --- */
    uint8_t rbuf[2048];
    struct dhcp_pkt *offer = NULL;
    uint32_t offered_ip = 0;
    uint32_t server_id  = 0;
    uint32_t dns_ip     = 0;
    /* X3: option 6 may carry several DNS servers; collect them all so the
     * resolver can fail over to the secondary on timeout (dns_set_servers). */
    uint32_t dns_list[DNS_SERVERS_MAX];
    int      dns_count  = 0;

    uint64_t offer_deadline = timer_get_ticks() + NET_DHCP_OFFER_TIMEOUT_TICKS;
    while (timer_get_ticks() < offer_deadline) {
        int n = net_recv_wait_until(rbuf, sizeof(rbuf), offer_deadline);
        if (n <= 0) break;
        if (n < (int)(14 + 20 + 8 + sizeof(struct dhcp_pkt))) continue;
        struct eth_hdr *eh = (struct eth_hdr *)rbuf;
        if (htons_(eh->ethertype) != ETHERTYPE_IPV4) continue;
        struct ipv4_hdr *ip = (struct ipv4_hdr *)(rbuf + 14);
        if (ip->protocol != IP_PROTO_UDP) continue;
        struct udp_hdr *udp = (struct udp_hdr *)(rbuf + 14 + 20);
        if (htons_(udp->src_port) != DHCP_SERVER_PORT) continue;
        if (htons_(udp->dst_port) != DHCP_CLIENT_PORT) continue;

        offer = (struct dhcp_pkt *)(rbuf + 14 + 20 + 8);

        /* Check it's a DHCP OFFER. */
        int opts_len = n - (14 + 20 + 8 + (int)sizeof(struct dhcp_pkt));
        if (opts_len < 4) continue;
        int mt_len;
        const uint8_t *mt = dhcp_find_option(offer->options, opts_len,
                                             DHCP_OPT_MSG_TYPE, &mt_len);
        if (!mt || mt_len != 1 || *mt != DHCP_OFFER) continue;

        /* Extract the offered IP (yiaddr, network byte order). */
        offered_ip = ntohl_(offer->yiaddr);

        /* Extract server ID, subnet mask, DNS from options. */
        int sid_len;
        const uint8_t *sid = dhcp_find_option(offer->options, opts_len,
                                               DHCP_OPT_SERVER_ID, &sid_len);
        if (sid && sid_len == 4) server_id = be32_to_host(sid);

        int sm_len;
        const uint8_t *sm = dhcp_find_option(offer->options, opts_len,
                                             DHCP_OPT_SUBNET_MASK, &sm_len);
        if (sm && sm_len == 4) subnet_mask = be32_to_host(sm);

        int dns_len;
        const uint8_t *dns = dhcp_find_option(offer->options, opts_len,
                                              DHCP_OPT_DNS_SERVER, &dns_len);
        if (dns && dns_len >= 4) {
            dns_ip = be32_to_host(dns);
            dns_count = dns_len / 4;
            if (dns_count > DNS_SERVERS_MAX) dns_count = DNS_SERVERS_MAX;
            for (int di = 0; di < dns_count; di++)
                dns_list[di] = be32_to_host(dns + di * 4);
        }

        break;
    }

    if (!offer) {
        kprintf("[dhcp] FAIL: no OFFER received\n");
        return -1;
    }

    kprintf("[dhcp] OFFER: IP %u.%u.%u.%u, server %u.%u.%u.%u\n",
            (offered_ip >> 24) & 0xFF, (offered_ip >> 16) & 0xFF,
            (offered_ip >> 8) & 0xFF, offered_ip & 0xFF,
            (server_id >> 24) & 0xFF, (server_id >> 16) & 0xFF,
            (server_id >> 8) & 0xFF, server_id & 0xFF);

    /* --- Step 3: Send DHCPREQUEST --- */
    memset(discover, 0, sizeof(discover));
    dhcp->op    = DHCP_BOOTREQUEST;
    dhcp->htype = 1;
    dhcp->hlen  = 6;
    dhcp->xid   = htonl_(dhcp_xid);
    /* BROADCAST flag (RFC 2131 s.4.1): we cannot reliably receive a
     * unicast ACK at an address we do not hold yet -- the measured
     * WHPX failure mode was exactly OFFER-received/ACK-never (the
     * server unicast the ACK to the still-unconfigured address).
     * Asking for a broadcast reply is the standard client answer. */
    dhcp->flags = htons_(0x8000);
    memcpy(dhcp->chaddr, our_mac, 6);
    dhcp->cookie = htonl_(DHCP_MAGIC_COOKIE);

    opt_pos = 0;
    msg_type = DHCP_REQUEST;
    opt_pos = dhcp_add_option(dhcp->options, opt_pos, DHCP_OPT_MSG_TYPE,
                              &msg_type, 1);
    /* Requested IP address. */
    uint32_t be_offered_ip = htonl_(offered_ip);
    opt_pos = dhcp_add_option(dhcp->options, opt_pos, DHCP_OPT_REQUESTED_IP,
                              &be_offered_ip, 4);
    /* Server identifier. */
    uint32_t be_server_id = htonl_(server_id);
    opt_pos = dhcp_add_option(dhcp->options, opt_pos, DHCP_OPT_SERVER_ID,
                              &be_server_id, 4);
    dhcp->options[opt_pos++] = DHCP_OPT_END;

    {
        uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        uint32_t udp_len = 8 + sizeof(struct dhcp_pkt) + opt_pos;
        uint32_t ip_len  = 20 + udp_len;
        uint32_t frame_len = 14 + ip_len;

        uint8_t frame[700];
        struct eth_hdr *eh = (struct eth_hdr *)frame;
        memcpy(eh->dst_mac, bcast_mac, 6);
        memcpy(eh->src_mac, our_mac, 6);
        eh->ethertype = htons_(ETHERTYPE_IPV4);

        struct ipv4_hdr *ip = (struct ipv4_hdr *)(frame + 14);
        ip->version_ihl = (4 << 4) | 5;
        ip->tos         = 0;           /* R9: was stack garbage (see
                                        * the DISCOVER builder's catch) */
        ip->total_length= htons_((uint16_t)ip_len);
        ip->ident       = htons_(0x1235);
        ip->flags_frag  = 0;
        ip->ttl         = 64;
        ip->protocol    = IP_PROTO_UDP;
        ip->src_ip      = htonl_(client_ip);       /* 0.0.0.0 */
        ip->dst_ip      = htonl_(dhcp_server_ip);  /* 255.255.255.255 */
        ip->checksum    = 0;
        ip->checksum    = checksum(ip, 20);

        struct udp_hdr *udp = (struct udp_hdr *)(frame + 14 + 20);
        udp->src_port = htons_(DHCP_CLIENT_PORT);
        udp->dst_port = htons_(DHCP_SERVER_PORT);
        udp->length   = htons_((uint16_t)udp_len);
        udp->checksum = 0;

        memcpy(frame + 14 + 20 + 8, discover, sizeof(struct dhcp_pkt) + opt_pos);

        if (frame_len < 60) {
            memset(frame + frame_len, 0, 60 - frame_len);
            frame_len = 60;
        }
        if (netdev_send(frame, frame_len) < 0) {
            kprintf("[dhcp] FAIL: REQUEST transmit failed (link down or TX timeout)\n");
            return -1;
        }
    }
    kprintf("[dhcp] REQUEST sent (requesting %u.%u.%u.%u)\n",
            (offered_ip >> 24) & 0xFF, (offered_ip >> 16) & 0xFF,
            (offered_ip >> 8) & 0xFF, offered_ip & 0xFF);

    /* --- Step 4: Wait for DHCPACK --- */
    uint64_t ack_deadline = timer_get_ticks() + NET_DHCP_ACK_TIMEOUT_TICKS;
    while (timer_get_ticks() < ack_deadline) {
        int n = net_recv_wait_until(rbuf, sizeof(rbuf), ack_deadline);
        if (n <= 0) break;
        if (n < (int)(14 + 20 + 8 + sizeof(struct dhcp_pkt))) continue;
        struct eth_hdr *eh = (struct eth_hdr *)rbuf;
        if (htons_(eh->ethertype) != ETHERTYPE_IPV4) continue;
        struct ipv4_hdr *ip = (struct ipv4_hdr *)(rbuf + 14);
        if (ip->protocol != IP_PROTO_UDP) continue;
        struct udp_hdr *udp = (struct udp_hdr *)(rbuf + 14 + 20);
        if (htons_(udp->src_port) != DHCP_SERVER_PORT) continue;
        if (htons_(udp->dst_port) != DHCP_CLIENT_PORT) continue;

        struct dhcp_pkt *ack = (struct dhcp_pkt *)(rbuf + 14 + 20 + 8);
        if (ntohl_(ack->xid) != dhcp_xid) continue;

        int opts_len = n - (14 + 20 + 8 + (int)sizeof(struct dhcp_pkt));
        if (opts_len < 4) continue;
        int mt_len;
        const uint8_t *mt = dhcp_find_option(ack->options, opts_len,
                                             DHCP_OPT_MSG_TYPE, &mt_len);
        if (!mt || mt_len != 1) continue;
        if (*mt == DHCP_ACK) {
            /* Success! Extract the assigned IP. */
            uint32_t acked_ip = ntohl_(ack->yiaddr);
            if (acked_ip != 0) {
                offered_ip = acked_ip;
            }

            /* Check for router option in the ACK. */
            int r_len;
            const uint8_t *rtr = dhcp_find_option(ack->options, opts_len,
                                                   DHCP_OPT_ROUTER, &r_len);
            if (rtr && r_len >= 4) {
                gateway_ip = be32_to_host(rtr);
            }

            our_ip = offered_ip;
            if (subnet_mask) {
                kprintf("[dhcp] subnet mask: %u.%u.%u.%u\n",
                        (subnet_mask >> 24) & 0xFF, (subnet_mask >> 16) & 0xFF,
                        (subnet_mask >> 8) & 0xFF, subnet_mask & 0xFF);
            }
            if (dns_ip) {
                kprintf("[dhcp] DNS server: %u.%u.%u.%u (%d total)\n",
                        (dns_ip >> 24) & 0xFF, (dns_ip >> 16) & 0xFF,
                        (dns_ip >> 8) & 0xFF, dns_ip & 0xFF, dns_count);
                /* X3: hand the full server list to the resolver so a
                 * silent primary fails over to the secondary. */
                dns_set_servers(dns_list, dns_count);
            }
            kprintf("[dhcp] PASS: IP %u.%u.%u.%u, gateway %u.%u.%u.%u\n",
                    (our_ip >> 24) & 0xFF, (our_ip >> 16) & 0xFF,
                    (our_ip >> 8) & 0xFF, our_ip & 0xFF,
                    (gateway_ip >> 24) & 0xFF, (gateway_ip >> 16) & 0xFF,
                    (gateway_ip >> 8) & 0xFF, gateway_ip & 0xFF);
            return 0;
        }
        if (*mt == 6 /* DHCPNAK */) {
            kprintf("[dhcp] FAIL: server NAK'd the request\n");
            return -1;
        }
    }

    kprintf("[dhcp] FAIL: no ACK received\n");
    return -1;
}

int net_init(void) {
    /* Start with the hardcoded QEMU defaults as fallback. */
    our_ip     = ip_from_octets(OUR_IP_O0, OUR_IP_O1, OUR_IP_O2, OUR_IP_O3);
    gateway_ip = ip_from_octets(GW_IP_O0, GW_IP_O1, GW_IP_O2, GW_IP_O3);

    /* Backend selection: e1000 is the default NIC.  When it is absent we fall
     * back to a modern virtio-net device, and then to the Realtek RTL8139
     * family -- the most widely cloned Fast Ethernet part there is, and the
     * NIC a great deal of older physical hardware (and every `-device
     * rtl8139' VM) actually presents.  The first NIC registered with the
     * netdev layer becomes the active one, so the order here IS the
     * priority: paravirtual before emulated-gigabit before 100 Mbit. */
    int have_nic = 0;
    if (e1000_init() == 0) {
        e1000_register_netdev();
        have_nic = 1;
    } else if (virtio_net_init() == 0) {
        virtio_net_register_netdev();
        have_nic = 1;
    } else if (rtl8139_init() == 0) {
        rtl8139_register_netdev();
        have_nic = 1;
    }
    if (!have_nic || !netdev_active()) {
        kprintf("[net] no NIC available\n");
        return -1;
    }
    kprintf("[net] using NIC: %s\n", netdev_name());

    netdev_get_mac(our_mac);

    if (!netdev_link_up()) {
        kprintf("[net] link is down; skipping DHCP and network self-tests\n");
        return -1;
    }

    /* X3: arm the DNS resolver with the default server list (QEMU SLIRP
     * 10.0.2.3); a successful DHCP below replaces it with option 6. */
    dns_init();
    net_ipfrag_self_test();   /* X4 */

    /* X7 (REALINTERNET_PLAN): IPv6 link-local address from the NIC MAC, plus
     * the offline NDP/ICMPv6 helper self-test. */
    net_ipv6_init();
    net_ipv6_self_test();

    /* Try DHCP to get a real IP. If it fails, fall back to the hardcoded
     * QEMU defaults (10.0.2.15 / 10.0.2.2). */
    int dhcp_ok = (net_dhcp() == 0);
    if (!dhcp_ok) {
        kprintf("[net] DHCP failed, using hardcoded IP %u.%u.%u.%u\n",
                OUR_IP_O0, OUR_IP_O1, OUR_IP_O2, OUR_IP_O3);
    }

    kprintf("[net] our IP: %u.%u.%u.%u, gateway: %u.%u.%u.%u\n",
            (our_ip >> 24) & 0xFF, (our_ip >> 16) & 0xFF,
            (our_ip >> 8) & 0xFF, our_ip & 0xFF,
            (gateway_ip >> 24) & 0xFF, (gateway_ip >> 16) & 0xFF,
            (gateway_ip >> 8) & 0xFF, gateway_ip & 0xFF);

    /* Return 0 only when DHCP succeeded. If DHCP failed, the stack remains
     * usable with fallback addressing, but boot skips slow online self-tests. */
    return dhcp_ok ? 0 : 1;
}

void net_self_test(void) {
    kprintf("[net] self-test: pinging gateway 10.0.2.2...\n");
    if (net_ping(gateway_ip) == 0) {
        kprintf("[net] PASS: ping 10.0.2.2 successful (ICMP echo reply received)\n");
    } else {
        kprintf("[net] FAIL: no ICMP echo reply (is QEMU -netdev user configured?)\n");
    }
}

/* ---- X4 guest self-test: feed synthetic fragmented frames through the
 * real net_ipfrag_step() wire-in.  Offline (needs no traffic): (a) a
 * 3000-byte UDP datagram split in three, delivered out of order, must come
 * back byte-identical; (b) a conflicting overlap probe must be refused and
 * the original bytes still win.  Timeout-drop and table-eviction policy are
 * gated by the host unit test (tests/unit/test_ip_reasm.c). */
static void craft_ip_frag(uint8_t *frame, uint16_t id, uint16_t ff,
                          uint16_t off_bytes, const uint8_t *datagram,
                          uint16_t frag_len, uint32_t src, uint32_t dst) {
    struct eth_hdr *eh = (struct eth_hdr *)frame;
    memset(eh->dst_mac, 0xAA, 6);
    memcpy(eh->src_mac, our_mac, 6);
    eh->ethertype = htons_(ETHERTYPE_IPV4);
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(frame + 14);
    memset(ip, 0, 20);
    ip->version_ihl = 0x45;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    ip->ident = htons_(id);
    ip->src_ip = htonl_(src);
    ip->dst_ip = htonl_(dst);
    ip->total_length = htons_((uint16_t)(20 + frag_len));
    ip->flags_frag = htons_(ff);
    memcpy(frame + 34, datagram + off_bytes, frag_len);
}

void net_ipfrag_self_test(void) {
    int fails = 0;
    static uint8_t dg[3000];
    for (int i = 0; i < 3000; i++) dg[i] = (uint8_t)(i * 31 + 7);
    const uint32_t src = 0x0A000203, dst = 0x0A00020F;   /* slirp DNS -> us */

    /* (a) 3 fragments, last first: 1480 | 1480 | 40 */
    static uint8_t fa[1600], fb[1600], fc[160];
    craft_ip_frag(fc, 0xBEEF, (uint16_t)(2960 / 8),          2960, dg,   40, src, dst);
    craft_ip_frag(fb, 0xBEEF, (uint16_t)(0x2000 | (1480 / 8)), 1480, dg, 1480, src, dst);
    craft_ip_frag(fa, 0xBEEF, 0x2000,                             0, dg, 1480, src, dst);
    int fl = 0;
    const uint8_t *r;
    if (net_ipfrag_step(fc, 14 + 20 + 40, &fl) != 0)   fails++;
    if (net_ipfrag_step(fb, 14 + 20 + 1480, &fl) != 0) fails++;
    r = net_ipfrag_step(fa, 14 + 20 + 1480, &fl);
    if (!r) { fails++; }
    else {
        if (fl != 14 + 20 + 3000) fails++;
        else if (memcmp(r + 34, dg, 3000) != 0) fails++;
    }

    /* (b) overlap probe: same first bytes, tampered middle -> refused;
     * the original first-fragment bytes must still win. */
    static uint8_t fd[1600], fe[1600], ff_[160];
    craft_ip_frag(fd, 0xBEEF ^ 0x100, 0x2000,                        0, dg, 1480, src, dst);
    craft_ip_frag(fe, 0xBEEF ^ 0x100, (uint16_t)(0x2000 | (1480 / 8)), 1480, dg, 1480, src, dst);
    craft_ip_frag(ff_, 0xBEEF ^ 0x100, (uint16_t)(2960 / 8),        2960, dg,   40, src, dst);
    /* overlapped copy of the first fragment with tampered bytes */
    static uint8_t atk[1600];
    craft_ip_frag(atk, 0xBEEF ^ 0x100, 0x2000, 0, dg, 1480, src, dst);
    atk[34 + 100] ^= 0xFF;
    if (net_ipfrag_step(fd, 14 + 20 + 1480, &fl) != 0) fails++;
    if (net_ipfrag_step(atk, 14 + 20 + 1480, &fl) != 0) fails++;  /* refused: NULL */
    if (net_ipfrag_step(fe, 14 + 20 + 1480, &fl) != 0) fails++;
    r = net_ipfrag_step(ff_, 14 + 20 + 40, &fl);
    if (!r) { fails++; }
    else if (fl != 14 + 20 + 3000 || memcmp(r + 34, dg, 3000) != 0) fails++;

    if (fails == 0)
        kprintf("[ipfrag] self-test PASS: out-of-order 3-fragment reassembly"
                " byte-exact, overlap probe refused\n");
    else
        kprintf("[ipfrag] self-test FAIL: %d check(s) failed\n", fails);
}

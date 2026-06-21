/* net.c — minimal network stack: Ethernet + ARP + IPv4 + ICMP.
 *
 * All in one file for compactness; the layers are clearly separated.
 * Uses the e1000 driver for actual packet send/receive.
 */

#include <stdint.h>
#include "kernel/net/net.h"
#include "drivers/e1000/e1000.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"

/* ---- Protocol constants ---- */
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV4  0x0800

#define ARP_REQUEST  1
#define ARP_REPLY    2

#define IP_PROTO_ICMP  1
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
struct eth_hdr {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;   /* network byte order */
} __attribute__((packed));

/* ---- ARP packet (28 bytes over Ethernet) ---- */
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

/* ---- IPv4 header (20 bytes, no options) ---- */
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

/* ---- ICMP header (8 bytes + data) ---- */
struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;      /* network byte order */
    uint16_t ident;
    uint16_t seq;
} __attribute__((packed));

/* ---- State ---- */
static uint8_t  our_mac[6];
static uint32_t our_ip;
static uint32_t gateway_ip;

/* ARP cache (single entry for the gateway). */
static uint8_t  gateway_mac[6];
static int      gateway_mac_known = 0;

/* ---- Byte-swap helpers ---- */
static uint16_t htons_(uint16_t v) { return (v >> 8) | (v << 8); }
static uint32_t htonl_(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}

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

/* ---- Ethernet send: wrap payload in an Ethernet frame and transmit. ---- */
static void eth_send(const uint8_t dst_mac[6], uint16_t ethertype,
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
    e1000_send(frame, total);
}

/* ---- ARP: resolve an IP address to a MAC. ---- */
static int arp_resolve(uint32_t target_ip, uint8_t out_mac[6]) {
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

    eth_send(broadcast, ETHERTYPE_ARP, &arp, sizeof(arp));
    kprintf("[net] ARP request sent for %u.%u.%u.%u\n",
            (target_ip >> 24) & 0xFF, (target_ip >> 16) & 0xFF,
            (target_ip >> 8) & 0xFF, target_ip & 0xFF);

    /* Poll for the ARP reply. */
    uint8_t buf[2048];
    for (int poll = 0; poll < 10000000; poll++) {
        int n = e1000_recv(buf, sizeof(buf));
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
    if (arp_resolve(target_ip, dst_mac) != 0) {
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

    e1000_send(pkt, 14 + 20 + 8 + 32);
    kprintf("[net] ICMP echo request sent to %u.%u.%u.%u\n",
            (target_ip >> 24) & 0xFF, (target_ip >> 16) & 0xFF,
            (target_ip >> 8) & 0xFF, target_ip & 0xFF);

    /* 5) Poll for the ICMP echo reply. */
    uint8_t buf[2048];
    for (int poll = 0; poll < 10000000; poll++) {
        int n = e1000_recv(buf, sizeof(buf));
        if (n < (int)(14 + 20 + 8)) {
            continue;
        }
        struct eth_hdr *reh = (struct eth_hdr *)buf;
        if (htons_(reh->ethertype) != ETHERTYPE_IPV4) {
            continue;
        }
        struct ipv4_hdr *rip = (struct ipv4_hdr *)(buf + 14);
        if (rip->protocol != IP_PROTO_ICMP) {
            continue;
        }
        struct icmp_hdr *ricmp = (struct icmp_hdr *)(buf + 14 + 20);
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

int net_init(void) {
    our_ip     = ip_from_octets(OUR_IP_O0, OUR_IP_O1, OUR_IP_O2, OUR_IP_O3);
    gateway_ip = ip_from_octets(GW_IP_O0, GW_IP_O1, GW_IP_O2, GW_IP_O3);

    if (e1000_init() != 0) {
        kprintf("[net] no NIC available\n");
        return -1;
    }

    e1000_get_mac(our_mac);
    kprintf("[net] our IP: %u.%u.%u.%u, gateway: %u.%u.%u.%u\n",
            OUR_IP_O0, OUR_IP_O1, OUR_IP_O2, OUR_IP_O3,
            GW_IP_O0, GW_IP_O1, GW_IP_O2, GW_IP_O3);
    return 0;
}

void net_self_test(void) {
    kprintf("[net] self-test: pinging gateway 10.0.2.2...\n");
    if (net_ping(gateway_ip) == 0) {
        kprintf("[net] PASS: ping 10.0.2.2 successful (ICMP echo reply received)\n");
    } else {
        kprintf("[net] FAIL: no ICMP echo reply (is QEMU -netdev user configured?)\n");
    }
}

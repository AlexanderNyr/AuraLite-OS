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

/* ---- Poll budgets ----
 * Keep boot responsive on hypervisors with disconnected/unsupported virtual
 * networking. QEMU/VirtualBox/VMware NAT replies arrive quickly when link is
 * healthy; long multi-second spin loops only delay boot when packets cannot be
 * transmitted or received. */
#define NET_ARP_POLLS       1000000
#define NET_ICMP_POLLS      1000000
#define NET_UDP_POLLS       1000000
#define NET_DHCP_OFFER_POLLS 1500000
#define NET_DHCP_ACK_POLLS  1000000

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
    return e1000_send(frame, total);
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
            uint64_t start_ticks = timer_get_ticks();
            while (timer_get_ticks() - start_ticks < 100) { /* 1.0 second timeout */
                int n = e1000_recv(buf, sizeof(buf));
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

    /* Poll for the ARP reply. */
    uint8_t buf[2048];
    uint64_t start_ticks = timer_get_ticks();
    while (timer_get_ticks() - start_ticks < 100) { /* 1.0 second timeout */
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

    if (e1000_send(pkt, 14 + 20 + 8 + 32) < 0) {
        kprintf("[net] ICMP echo request TX failed\n");
        return -1;
    }
    kprintf("[net] ICMP echo request sent to %u.%u.%u.%u\n",
            (target_ip >> 24) & 0xFF, (target_ip >> 16) & 0xFF,
            (target_ip >> 8) & 0xFF, target_ip & 0xFF);

    /* 5) Poll for the ICMP echo reply. */
    uint8_t buf[2048];
    uint64_t start_ticks = timer_get_ticks();
    while (timer_get_ticks() - start_ticks < 200) { /* 2.0 second timeout */
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

/* ---- UDP header (8 bytes) ---- */
struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;    /* header + data, network byte order */
    uint16_t checksum;  /* 0 = no checksum (legal for IPv4/UDP) */
} __attribute__((packed));

/*
 * Send a UDP datagram to dst_ip:dst_port with the given payload.
 * Resolves the MAC via ARP, builds Ethernet + IPv4 + UDP, and transmits.
 * Returns 0 on success, -1 on failure.
 */
static int net_udp_send(uint32_t dst_ip, uint16_t dst_port,
                        uint16_t src_port, const void *data, uint32_t data_len) {
    uint8_t dst_mac[6];
    if (net_arp_resolve(dst_ip, dst_mac) != 0) {
        return -1;
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
    if (e1000_send(pkt, frame_len) < 0) {
        return -1;
    }
    return 0;
}

/*
 * Poll for a UDP packet from dst_ip:dst_port. Copies up to bufsize bytes of
 * the UDP payload into buf. Returns payload length, or -1 on timeout.
 */
static int net_udp_recv(uint32_t src_ip, uint16_t src_port,
                        void *buf, uint32_t bufsize) {
    uint8_t rbuf[2048];
    uint64_t start_ticks = timer_get_ticks();
    while (timer_get_ticks() - start_ticks < 200) { /* 2.0 second timeout */
        int n = e1000_recv(rbuf, sizeof(rbuf));
        if (n < (int)(14 + 20 + 8)) continue;
        struct eth_hdr *eh = (struct eth_hdr *)rbuf;
        if (htons_(eh->ethertype) != ETHERTYPE_IPV4) continue;
        struct ipv4_hdr *ip = (struct ipv4_hdr *)(rbuf + 14);
        if (ip->protocol != IP_PROTO_UDP) continue;
        if (htonl_(ip->src_ip) != src_ip) continue;
        struct udp_hdr *udp = (struct udp_hdr *)(rbuf + 14 + 20);
        if (htons_(udp->src_port) != src_port) continue;

        /* Found it. Copy the payload. */
        uint16_t udp_len = htons_(udp->length);
        if (udp_len < 8) return -1;
        uint16_t payload_len = udp_len - 8;
        if (payload_len > bufsize) payload_len = (uint16_t)bufsize;
        memcpy(buf, rbuf + 14 + 20 + 8, payload_len);
        return payload_len;
    }
    return -1;
}

/* ---- DNS resolver ---- */

/* QEMU's built-in DNS proxy. */
#define DNS_IP_O0 10
#define DNS_IP_O1 0
#define DNS_IP_O2 2
#define DNS_IP_O3 3

struct dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;   /* questions */
    uint16_t an_count;   /* answers */
    uint16_t ns_count;
    uint16_t ar_count;
} __attribute__((packed));

/*
 * Encode a dotted hostname (e.g. "example.com") into DNS label format:
 * each dot-delimited segment is prefixed by its length byte, terminated
 * by a zero byte. Writes into `out` and returns the total length.
 */
static int dns_encode_name(const char *name, uint8_t *out) {
    int pos = 0;
    const char *seg = name;
    while (*seg) {
        const char *dot = seg;
        while (*dot && *dot != '.') dot++;
        int len = dot - seg;
        if (len > 63 || len == 0) return -1;
        out[pos++] = (uint8_t)len;
        for (int i = 0; i < len; i++) {
            out[pos++] = (uint8_t)seg[i];
        }
        seg = dot;
        if (*seg == '.') seg++;
    }
    out[pos++] = 0;   /* root label */
    return pos;
}

/*
 * Resolve a hostname to an IPv4 address via QEMU's DNS proxy (10.0.2.3:53).
 * Returns the IP in host byte order, or 0 on failure.
 */
uint32_t net_dns_resolve(const char *hostname) {
    uint32_t dns_ip = ip_from_octets(DNS_IP_O0, DNS_IP_O1, DNS_IP_O2, DNS_IP_O3);

    /* Build the DNS query. */
    uint8_t query[512];
    struct dns_header *hdr = (struct dns_header *)query;
    hdr->id       = htons_(0x1234);
    hdr->flags    = htons_(0x0100);   /* standard query, recursion desired */
    hdr->qd_count = htons_(1);
    hdr->an_count = 0;
    hdr->ns_count = 0;
    hdr->ar_count = 0;

    /* Encode the question name after the 12-byte header. */
    int name_len = dns_encode_name(hostname, query + 12);
    if (name_len < 0) {
        kprintf("[net] dns: invalid hostname '%s'\n", hostname);
        return 0;
    }

    /* Question type (A = 1) and class (IN = 1). */
    int q_off = 12 + name_len;
    query[q_off]     = 0; query[q_off + 1] = 1;   /* type A */
    query[q_off + 2] = 0; query[q_off + 3] = 1;   /* class IN */
    int query_len = q_off + 4;

    kprintf("[net] dns: resolving '%s' via 10.0.2.3:53...\n", hostname);

    if (net_udp_send(dns_ip, 53, 12345, query, query_len) != 0) {
        kprintf("[net] dns: failed to send query\n");
        return 0;
    }

    /* Wait for the response. */
    uint8_t resp[1024];
    int resp_len = net_udp_recv(dns_ip, 53, resp, sizeof(resp));
    if (resp_len < (int)(12 + name_len + 4 + 12)) {
        kprintf("[net] dns: no response (got %d bytes)\n", resp_len);
        return 0;
    }

    /* Parse the response: skip the question section, then walk the answer
     * resource records looking for a type-A (1) record. */
    struct dns_header *rhdr = (struct dns_header *)resp;
    uint16_t an_count = htons_(rhdr->an_count);
    if (an_count == 0) {
        kprintf("[net] dns: no answers in response\n");
        return 0;
    }

    /* Skip past the question section (header + name + 4 bytes type/class). */
    int off = 12 + name_len + 4;

    for (int a = 0; a < an_count && off + 12 < resp_len; a++) {
        /* Skip the name (may be compressed: if first byte has top 2 bits set,
         * it's a 2-byte pointer; otherwise walk labels). */
        if ((resp[off] & 0xC0) == 0xC0) {
            off += 2;   /* compressed name pointer */
        } else {
            while (off < resp_len && resp[off] != 0) off += resp[off] + 1;
            off++;      /* skip the terminating zero */
        }

        if (off + 10 > resp_len) break;
        uint16_t rtype = (resp[off] << 8) | resp[off + 1];
        uint16_t rdlen = (resp[off + 8] << 8) | resp[off + 9];
        off += 10;   /* skip type(2) + class(2) + ttl(4) + rdlength(2) */

        if (rtype == 1 && rdlen == 4 && off + 4 <= resp_len) {
            /* Type A record: 4 bytes of IPv4 address (network byte order). */
            uint32_t ip = ((uint32_t)resp[off] << 24) |
                          ((uint32_t)resp[off + 1] << 16) |
                          ((uint32_t)resp[off + 2] << 8) |
                          (uint32_t)resp[off + 3];
            kprintf("[net] dns: '%s' resolved to %u.%u.%u.%u\n",
                    hostname,
                    (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                    (ip >> 8) & 0xFF, ip & 0xFF);
            return ip;
        }
        off += rdlen;   /* skip this resource record's data */
    }

    kprintf("[net] dns: no A record found for '%s'\n", hostname);
    return 0;
}

void net_dns_self_test(void) {
    kprintf("[net] dns self-test: resolving 'example.com'...\n");
    uint32_t ip = net_dns_resolve("example.com");
    if (ip != 0) {
        kprintf("[net] dns PASS: example.com -> %u.%u.%u.%u\n",
                (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                (ip >> 8) & 0xFF, ip & 0xFF);
    } else {
        kprintf("[net] dns FAIL: could not resolve example.com\n");
    }
}

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

        /* IP. */
        struct ipv4_hdr *ip = (struct ipv4_hdr *)(frame + 14);
        ip->version_ihl = (4 << 4) | 5;
        ip->total_length= htons_((uint16_t)ip_len);
        ip->ident       = htons_(0x1234);
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
        if (e1000_send(frame, frame_len) < 0) {
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

    uint64_t offer_start_ticks = timer_get_ticks();
    while (timer_get_ticks() - offer_start_ticks < 200) { /* 2.0 second timeout */
        int n = e1000_recv(rbuf, sizeof(rbuf));
        if (n <= 0) continue;
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
        if (dns && dns_len >= 4) dns_ip = be32_to_host(dns);

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
    dhcp->flags = htons_(0x0000);   /* unicast reply OK */
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
        ip->total_length= htons_((uint16_t)ip_len);
        ip->ident       = htons_(0x1235);
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
        if (e1000_send(frame, frame_len) < 0) {
            kprintf("[dhcp] FAIL: REQUEST transmit failed (link down or TX timeout)\n");
            return -1;
        }
    }
    kprintf("[dhcp] REQUEST sent (requesting %u.%u.%u.%u)\n",
            (offered_ip >> 24) & 0xFF, (offered_ip >> 16) & 0xFF,
            (offered_ip >> 8) & 0xFF, offered_ip & 0xFF);

    /* --- Step 4: Wait for DHCPACK --- */
    uint64_t ack_start_ticks = timer_get_ticks();
    while (timer_get_ticks() - ack_start_ticks < 200) { /* 2.0 second timeout */
        int n = e1000_recv(rbuf, sizeof(rbuf));
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
                kprintf("[dhcp] DNS server: %u.%u.%u.%u\n",
                        (dns_ip >> 24) & 0xFF, (dns_ip >> 16) & 0xFF,
                        (dns_ip >> 8) & 0xFF, dns_ip & 0xFF);
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

    if (e1000_init() != 0) {
        kprintf("[net] no NIC available\n");
        return -1;
    }

    e1000_get_mac(our_mac);

    if (!e1000_link_up()) {
        kprintf("[net] link is down; skipping DHCP and network self-tests\n");
        return -1;
    }

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

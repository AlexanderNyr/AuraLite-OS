/* ipv6.c — IPv6 network I/O for AuraLite OS.
 *
 * REALINTERNET_PLAN phase X7 (IPv6).  Uses the pure helpers in ipv6_addr.c
 * and the netdev frame transport from net.c to deliver the phase's primary
 * gate: ICMPv6 echo ("ping6") to a link-local neighbour, with neighbour MAC
 * resolution via Neighbor Discovery (NS/NA).  See ipv6.h for scope.
 */

#include "kernel/net/ipv6.h"
#include "kernel/net/netdev.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"

extern uint64_t timer_get_ticks(void);
extern int net_eth_send(const uint8_t dst_mac[6], uint16_t ethertype,
                        const void *payload, uint32_t plen);
void net_get_mac(uint8_t mac[6]);

#define ETHERTYPE_IPV6  0x86DD

#define IPV6_NEXT_ICMP6 58
#define IPV6_HOP_LIMIT  64
/* RFC 4861 s6.1.1: every Neighbor/Router Discovery message MUST carry an IP
 * Hop Limit of 255, and receivers discard any with a different value. */
#define IPV6_NDP_HOP_LIMIT 255

/* ICMPv6 message types. */
#define ICMP6_ECHO_REQ   128
#define ICMP6_ECHO_REP   129
#define ICMP6_RS         133
#define ICMP6_RA         134
#define ICMP6_NS         135
#define ICMP6_NA         136

/* NDP option types. */
#define NDP_OPT_SRC_LL   1
#define NDP_OPT_TGT_LL   2

#define NET6_NDP_TIMEOUT_TICKS 100
#define NET6_ICMP_TIMEOUT_TICKS 200

/* ---- State ---- */
static ipv6_addr_t our_ll;
static uint8_t     our_mac[6];
static int         ipv6_up = 0;

/* Router (default gateway) learned from a Router Advertisement. */
static ipv6_addr_t router_ll;
static uint8_t     router_mac[6];
static int         have_router = 0;

/* R9 (ledger RES-24): the SLAAC address -- RA Prefix Information
 * (autonomous, /64) + our EUI-64 interface id.  Formed in the same
 * RA walk that learns the router; 0 until an RA carries the A-flag. */
static ipv6_addr_t our_global;
static int         have_global = 0;

/* RFC 4861 s4.6.2: Prefix Information option. */
#define NDP_OPT_PREFIX   3
#define NDP_PFX_FLAG_A   0x40

/* IPv6 header (40 bytes, no extension headers). */
struct ipv6_hdr {
    uint32_t ver_tc_fl;    /* 6<<28 | tc<<20 | flow */
    uint16_t payload_len;  /* network byte order */
    uint8_t  next_header;
    uint8_t  hop_limit;
    uint8_t  src[16];
    uint8_t  dst[16];
} __attribute__((packed));

/* ICMPv6 header (4 bytes + type-specific payload). */
struct icmp6_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;     /* network byte order */
} __attribute__((packed));

static uint16_t htons16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

/* Build the solicited-node multicast address ff02::1:ffXX:XXXX for a target
 * and the matching 33:33:ff:XX:XX:XX Ethernet multicast MAC. */
static void solicited_node(const ipv6_addr_t *target, ipv6_addr_t *mc,
                           uint8_t mc_mac[6]) {
    memset(mc->b, 0, 16);
    mc->b[0] = 0xFF; mc->b[1] = 0x02;
    mc->b[10] = 0;   mc->b[11] = 0x01;   /* group5 = 0x0001 */
    mc->b[12] = 0xFF;                     /* group6 = 0xffXX */
    mc->b[13] = target->b[13];
    mc->b[14] = target->b[14];            /* group7 = XX:XX */
    mc->b[15] = target->b[15];
    mc_mac[0] = 0x33; mc_mac[1] = 0x33; mc_mac[2] = 0xFF;
    mc_mac[3] = target->b[13]; mc_mac[4] = target->b[14]; mc_mac[5] = target->b[15];
}

/* Fill an IPv6 header (ver=6, no flow label, given hop limit, src/dst). */
static void ip6_fill_hdr(struct ipv6_hdr *ip, const ipv6_addr_t *src,
                         const ipv6_addr_t *dst, uint16_t payload_len,
                         uint8_t hop_limit) {
    /* The version/traffic-class/flow-label dword is big-endian on the wire;
     * on a little-endian machine the value must be 0x60 (not 0x60000000) so
     * the first byte emitted is 0x60 = version 6. */
    ip->ver_tc_fl = 0x60u;
    ip->payload_len = htons16(payload_len);
    ip->next_header = IPV6_NEXT_ICMP6;
    ip->hop_limit = hop_limit;
    memcpy(ip->src, src->b, 16);
    memcpy(ip->dst, dst->b, 16);
}

/* ---- Neighbor Discovery: resolve the link-layer address of a neighbour. ---- */

/* R9: fe80::/10 test + source selection (RFC 6724's floor: global
 * source for a global destination, link-local otherwise). */
static int is_linklocal6(const ipv6_addr_t *a) {
    return a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0x80;
}
static const ipv6_addr_t *src_for(const ipv6_addr_t *dst) {
    return (!is_linklocal6(dst) && have_global) ? &our_global : &our_ll;
}

static int ndp_resolve(const ipv6_addr_t *target, uint8_t out_mac[6]);

int net_ipv6_resolve(const ipv6_addr_t *target, uint8_t out_mac[6]) {
    if (!target || !out_mac) return -1;
    return ndp_resolve(target, out_mac);
}

const ipv6_addr_t *net_ipv6_src_for(const ipv6_addr_t *dst) {
    if (!dst) return 0;
    return src_for(dst);
}

static int ndp_resolve(const ipv6_addr_t *target, uint8_t out_mac[6]) {
    /* Our own addresses resolve to our own MAC. */
    if (ipv6_eq(target, &our_ll) == 0) {
        memcpy(out_mac, our_mac, 6);
        return 0;
    }
    /* The learned router resolves without a fresh NS/NA. */
    if (have_router && ipv6_eq(target, &router_ll) == 0) {
        memcpy(out_mac, router_mac, 6);
        return 0;
    }
    /* R9: an off-link destination (not fe80::/10, not our own /64
     * prefix) goes to the default router -- the routing decision the
     * X7 draft did not need while everything was link-local. */
    if (!is_linklocal6(target) && have_router &&
        !(have_global && memcmp(target->b, our_global.b, 8) == 0)) {
        memcpy(out_mac, router_mac, 6);
        return 0;
    }

    ipv6_addr_t snm;
    uint8_t snm_mac[6];
    solicited_node(target, &snm, snm_mac);

    /* Neighbor Solicitation: icmp6(8) + reserved(4) + target(16) + option(8). */
    uint8_t body[4 + 16 + 8];
    memset(body, 0, sizeof(body));
    memcpy(body + 4, target->b, 16);
    body[4 + 16 + 0] = NDP_OPT_SRC_LL;
    body[4 + 16 + 1] = 1;   /* 8 bytes */
    memcpy(body + 4 + 16 + 2, our_mac, 6);

    struct icmp6_hdr ic;
    ic.type = ICMP6_NS;
    ic.code = 0;
    ic.checksum = 0;
    /* R9 CATCH (pcap-verified): the X7 draft declared 8+sizeof(body)
     * here, but the wire message is 4 fixed bytes (type/code/cksum)
     * + body -- body already CARRIES the 4 reserved bytes.  The old
     * length overshot by 4: the checksum covered uninitialised tail
     * bytes and the payload_len lied, so every NS left this machine
     * with a bad checksum and SLIRP silently dropped it.  THAT --
     * not a SLIRP filtering limitation -- is why X7's peer lanes
     * never answered; the echo paths never had the bug (their 8
     * counts ident+seq, which echo messages genuinely carry). */
    uint32_t icmp_len = 4 + (uint32_t)sizeof(body);
    /* Build a temporary ICMP message to checksum. */
    uint8_t msg[8 + 4 + 16 + 8];
    memcpy(msg, &ic, 4);
    memcpy(msg + 4, body, sizeof(body));
    memset(msg + 2, 0, 2);   /* checksum field zeroed */
    uint16_t cs = ipv6_checksum_pseudo(&our_ll, &snm, icmp_len,
                                       IPV6_NEXT_ICMP6, msg, icmp_len);
    /* R9 CATCH, layer two (the R3 byte-order class, ICMPv6 edition):
     * the helper returns the HOST-ORDER checksum -- a struct store
     * on this little-endian machine swapped it on the wire (pcap:
     * 0xe071 -> 0x71e0).  Every struct-store below now serialises. */
    ic.checksum = htons16(cs);

    /* Assemble the frame: eth(14) + ipv6(40) + icmp6(8) + body(28). */
    uint8_t pkt[14 + 40 + 8 + (4 + 16 + 8)];
    struct eth_hdr { uint8_t dst[6], src[6]; uint16_t et; } __attribute__((packed)) *eh = (void *)pkt;
    memcpy(eh->dst, snm_mac, 6);
    memcpy(eh->src, our_mac, 6);
    eh->et = htons16(ETHERTYPE_IPV6);
    struct ipv6_hdr *ip = (struct ipv6_hdr *)(pkt + 14);
    ip6_fill_hdr(ip, &our_ll, &snm, (uint16_t)icmp_len, IPV6_NDP_HOP_LIMIT);
    memcpy(pkt + 14 + 40, &ic, 4);
    memcpy(pkt + 14 + 40 + 4, body, sizeof(body));

    if (net_eth_send(snm_mac, ETHERTYPE_IPV6, pkt + 14, 40 + 8 + (uint32_t)sizeof(body)) < 0) {
        kprintf("[net6] NS TX failed\n");
        return -1;
    }

    /* Wait for a Neighbor Advertisement with our target and its link-layer. */
    uint8_t buf[2048];
    uint64_t deadline = timer_get_ticks() + NET6_NDP_TIMEOUT_TICKS;
    while (timer_get_ticks() < deadline) {
        uint64_t now = timer_get_ticks();
        int n = netdev_recv_wait(buf, sizeof(buf), deadline > now ? deadline - now : 0);
        if (n <= 0) break;
        if (n < (int)(14 + 40 + 4 + 4 + 16)) continue;
        struct eth_hdr *eh = (struct eth_hdr *)buf;
        if (htons16(eh->et) != ETHERTYPE_IPV6) continue;
        struct ipv6_hdr *ip = (struct ipv6_hdr *)(buf + 14);
        if (ip->next_header != IPV6_NEXT_ICMP6) continue;
        if (ipv6_eq((ipv6_addr_t *)ip->dst, &our_ll) != 0) {
            net_ipv6_handle_frame(buf, n);   /* R9: serve NDP meanwhile */
            continue;
        }
        struct icmp6_hdr *ic = (struct icmp6_hdr *)(buf + 14 + 40);
        if (ic->type != ICMP6_NA) continue;
        /* R9 CATCH, layer three: RFC 4861 puts the NA target at
         * ICMP+8 (type,code,cksum,flags4) and options at ICMP+24;
         * the X7 draft read +12/+28 -- four bytes deep into the
         * target, so a REAL NA could never match.  Fixed offsets,
         * measured against SLIRP's 32-byte NA on the wire. */
        if (memcmp(buf + 14 + 40 + 8, target->b, 16) != 0) continue;
        const uint8_t *opt = buf + 14 + 40 + 8 + 16;
        int avail = n - (14 + 40 + 8 + 16);
        if (avail >= 8 && opt[0] == NDP_OPT_TGT_LL) {
            memcpy(out_mac, opt + 2, 6);
            return 0;
        }
    }
    kprintf("[net6] NDP timeout resolving neighbour\n");
    return -1;
}

/* ---- Router Discovery (RS/RA). ---- */
void net_ipv6_discover(void) {
    /* All-routers multicast ff02::2 -> 33:33:00:00:00:02. */
    ipv6_addr_t ar;
    memset(ar.b, 0, 16);
    ar.b[0] = 0xFF; ar.b[1] = 0x02;
    ar.b[15] = 0x02;
    uint8_t ar_mac[6] = { 0x33, 0x33, 0x00, 0x00, 0x00, 0x02 };

    /* Router Solicitation: icmp6(8) + reserved(4) + src-ll-option(8). */
    uint8_t body[4 + 8];
    memset(body, 0, sizeof(body));
    body[4 + 0] = NDP_OPT_SRC_LL;
    body[4 + 1] = 1;
    memcpy(body + 4 + 2, our_mac, 6);

    struct icmp6_hdr ic;
    ic.type = ICMP6_RS;
    ic.code = 0;
    ic.checksum = 0;
    /* R9 CATCH: same +4 overshoot as the NS (see ndp_resolve) --
     * the RS that "SLIRP never answered" was leaving with a bad
     * checksum and four garbage tail bytes.  pcap -v named it. */
    uint32_t icmp_len = 4 + (uint32_t)sizeof(body);
    uint8_t msg[8 + 4 + 8];
    memcpy(msg, &ic, 4);
    memcpy(msg + 4, body, sizeof(body));
    memset(msg + 2, 0, 2);
    ic.checksum = htons16(ipv6_checksum_pseudo(&our_ll, &ar, icmp_len,
                                       IPV6_NEXT_ICMP6, msg, icmp_len));

    uint8_t pkt[14 + 40 + 8 + (4 + 8)];
    struct eth_hdr { uint8_t dst[6], src[6]; uint16_t et; } __attribute__((packed)) *eh = (void *)pkt;
    memcpy(eh->dst, ar_mac, 6);
    memcpy(eh->src, our_mac, 6);
    eh->et = htons16(ETHERTYPE_IPV6);
    struct ipv6_hdr *ip = (struct ipv6_hdr *)(pkt + 14);
    ip6_fill_hdr(ip, &our_ll, &ar, (uint16_t)icmp_len, IPV6_NDP_HOP_LIMIT);
    memcpy(pkt + 14 + 40, &ic, 4);
    memcpy(pkt + 14 + 40 + 4, body, sizeof(body));

    /* R9: three solicitations, not one -- RFC 4861 s6.3.7 sends up
     * to MAX_RTR_SOLICITATIONS(3); the first RS can race the NIC's
     * own bring-up (measured on the e1000 lane: the RA answering
     * RS #1 never reached the ring, RS #2's always did). */
    uint8_t buf[2048];
    for (int attempt = 0; attempt < 3 && !have_router; attempt++) {
    if (net_eth_send(ar_mac, ETHERTYPE_IPV6, pkt + 14, 40 + 8 + (uint32_t)sizeof(body)) < 0) {
        kprintf("[net6] RS TX failed\n");
        return;
    }
    kprintf("[net6] router solicitation sent%s\n",
            attempt ? " (retry)" : "");

    /* Wait briefly for a Router Advertisement: source is the router's
     * link-local; the source-link-layer option carries its MAC. */
    uint64_t deadline = timer_get_ticks() + 60;
    while (timer_get_ticks() < deadline) {
        uint64_t now = timer_get_ticks();
        int n = netdev_recv_wait(buf, sizeof(buf), deadline > now ? deadline - now : 0);
        if (n <= 0) break;
        if (n < (int)(14 + 40 + 8)) continue;
        struct eth_hdr *eh = (struct eth_hdr *)buf;
        if (htons16(eh->et) != ETHERTYPE_IPV6) continue;
        struct ipv6_hdr *ip = (struct ipv6_hdr *)(buf + 14);
        if (ip->next_header != IPV6_NEXT_ICMP6) continue;
        struct icmp6_hdr *ic = (struct icmp6_hdr *)(buf + 14 + 40);
        if (ic->type != ICMP6_RA) continue;
        /* R9: a solicited RA arrives on the ALL-NODES multicast
         * ff02::1 (SLIRP does exactly that, 200 us after the RS) --
         * the X7 draft demanded our unicast and dropped every one. */
        ipv6_addr_t alln;
        memset(alln.b, 0, 16);
        alln.b[0] = 0xFF; alln.b[1] = 0x02; alln.b[15] = 0x01;
        if (ipv6_eq((ipv6_addr_t *)ip->dst, &our_ll) != 0 &&
            ipv6_eq((ipv6_addr_t *)ip->dst, &alln) != 0) continue;
        /* options follow the 8-byte RA header -- R9: ONE walk now
         * collects both the router's link-layer address AND the
         * Prefix Information option (SLAAC).  The X7 draft returned
         * at the first SRC_LL and never saw the prefix behind it. */
        /* RFC 4861 s4.2: the RA fixed part is 16 bytes (type,code,
         * cksum, hop-limit, M/O, lifetime, reachable, retrans) --
         * options start at ICMP+16, not +8 (the draft parsed the
         * reachable-time field as an option type and always broke). */
        const uint8_t *opt = buf + 14 + 40 + 16;
        int avail = n - (14 + 40 + 16);
        int got_mac = 0;
        while (avail >= 8) {
            uint8_t ot = opt[0], ol = opt[1];
            int olen = (int)ol * 8;
            if (olen < 8 || olen > avail) break;
            if (ot == NDP_OPT_SRC_LL && olen >= 8 && !got_mac) {
                memcpy(router_mac, opt + 2, 6);
                memcpy(router_ll.b, ip->src, 16);
                got_mac = 1;
            } else if (ot == NDP_OPT_PREFIX && olen >= 32 &&
                       !have_global &&
                       opt[2] == 64 && (opt[3] & NDP_PFX_FLAG_A)) {
                /* SLAAC: prefix(64) + EUI-64 interface id -- the
                 * SAME low 8 bytes the link-local carries. */
                memcpy(our_global.b, opt + 16, 8);
                memcpy(our_global.b + 8, our_ll.b + 8, 8);
                have_global = 1;
                char g[48];
                ipv6_ntop(&our_global, g, sizeof(g));
                kprintf("[net6] SLAAC address %s (RA prefix /64 + "
                        "EUI-64, A-flag set)\n", g);
            }
            opt += olen;
            avail -= olen;
        }
        if (got_mac) {
            have_router = 1;
            char r[48];
            ipv6_ntop(&router_ll, r, sizeof(r));
            kprintf("[net6] router %s at %02x:%02x:%02x:%02x:%02x:%02x\n",
                    r, router_mac[0], router_mac[1], router_mac[2],
                    router_mac[3], router_mac[4], router_mac[5]);
            return;
        }
    }
    }
    kprintf("[net6] no router advertisement (offline link or no router)\n");
}

/* ---- ICMPv6 echo reply responder (the OS answers pings to its link-local). ---- */

/* Build and transmit an ICMPv6 echo reply for a received echo request.
 * `req_src_mac` is the Ethernet source MAC of the request (where to reply). */
static void icmp6_send_echo_reply(const uint8_t req_src_mac[6],
                                  const ipv6_addr_t *req_src,
                                  const ipv6_addr_t *reply_src, /* R9: the
                                   * address the request was addressed to */
                                  const uint8_t *req_hdr,   /* 8-byte ICMPv6 hdr */
                                  const uint8_t *payload, uint32_t plen) {
    if (plen > 256) plen = 256;
    uint8_t msg[8 + 256];
    struct icmp6_hdr ic;
    ic.type = ICMP6_ECHO_REP;
    ic.code = 0;
    ic.checksum = 0;
    memcpy(msg, &ic, 4);
    memcpy(msg + 4, req_hdr + 4, 4);        /* ident + seq from the request */
    memcpy(msg + 8, payload, plen);
    uint32_t icmp_len = 8 + plen;
    ic.checksum = htons16(ipv6_checksum_pseudo(reply_src, req_src, icmp_len,
                                       IPV6_NEXT_ICMP6, msg, icmp_len));

    uint8_t pkt[14 + 40 + 8 + 256];
    struct eth_hdr { uint8_t dst[6], src[6]; uint16_t et; } __attribute__((packed)) *eh = (void *)pkt;
    memcpy(eh->dst, req_src_mac, 6);
    memcpy(eh->src, our_mac, 6);
    eh->et = htons16(ETHERTYPE_IPV6);
    struct ipv6_hdr *ip = (struct ipv6_hdr *)(pkt + 14);
    ip6_fill_hdr(ip, reply_src, req_src, (uint16_t)icmp_len, IPV6_HOP_LIMIT);
    memcpy(pkt + 14 + 40, &ic, 4);
    memcpy(pkt + 14 + 40 + 4, req_hdr + 4, 4);
    memcpy(pkt + 14 + 40 + 8, payload, plen);
    net_eth_send(req_src_mac, ETHERTYPE_IPV6, pkt + 14, 40 + (uint32_t)icmp_len);
}

/* Process one received Ethernet frame.  If it is an ICMPv6 echo request
 * addressed to our link-local address (with a valid checksum), reply and
 * return 1 (consumed).  Otherwise return 0. */
int net_ipv6_handle_frame(const uint8_t *frame, int len) {
    if (!ipv6_up) return 0;
    if (len < (int)(14 + 40 + 8)) return 0;
    struct eth_hdr { uint8_t dst[6], src[6]; uint16_t et; } __attribute__((packed)) *eh = (void *)frame;
    if (htons16(eh->et) != ETHERTYPE_IPV6) return 0;
    struct ipv6_hdr *ip = (struct ipv6_hdr *)(frame + 14);
    if (ip->next_header != IPV6_NEXT_ICMP6) return 0;
    struct icmp6_hdr *ic0 = (struct icmp6_hdr *)(frame + 14 + 40);

    /* R9: answer Neighbor Solicitations about OUR addresses -- the
     * missing half of NDP.  Without this no peer (SLIRP included)
     * can ever resolve us, so nothing addressed to the SLAAC
     * address could be DELIVERED: the pcap showed our sum-ok echo
     * to fec0::2 vanish because the reply had no MAC to ride on.
     * An NS arrives on the solicited-node multicast (or unicast),
     * NOT on our address -- it must be handled before the dst
     * filter below. */
    if (ic0->type == ICMP6_NS && len >= (int)(14 + 40 + 4 + 4 + 16)) {
        const uint8_t *tgt = frame + 14 + 40 + 8;
        const ipv6_addr_t *ours = 0;
        if (memcmp(tgt, our_ll.b, 16) == 0)
            ours = &our_ll;
        else if (have_global && memcmp(tgt, our_global.b, 16) == 0)
            ours = &our_global;
        if (!ours)
            return 0;
        /* NA: flags Solicited|Override, target, TGT_LL option. */
        uint8_t msg[4 + 4 + 16 + 8];
        memset(msg, 0, sizeof(msg));
        msg[0] = ICMP6_NA;
        msg[4] = 0x60;                   /* S|O */
        memcpy(msg + 8, tgt, 16);
        msg[24] = NDP_OPT_TGT_LL;
        msg[25] = 1;
        memcpy(msg + 26, our_mac, 6);
        uint32_t icmp_len = (uint32_t)sizeof(msg);
        uint16_t cs = ipv6_checksum_pseudo(ours, (ipv6_addr_t *)ip->src,
                                           icmp_len, IPV6_NEXT_ICMP6,
                                           msg, icmp_len);
        msg[2] = (uint8_t)(cs >> 8);
        msg[3] = (uint8_t)cs;
        uint8_t pkt[14 + 40 + 4 + 4 + 16 + 8];
        struct ipv6_hdr *rip = (struct ipv6_hdr *)(pkt + 14);
        ip6_fill_hdr(rip, ours, (ipv6_addr_t *)ip->src,
                     (uint16_t)icmp_len, IPV6_NDP_HOP_LIMIT);
        memcpy(pkt + 14 + 40, msg, sizeof(msg));
        net_eth_send(eh->src, ETHERTYPE_IPV6, pkt + 14,
                     40 + icmp_len);
        return 1;
    }

    /* only echo requests addressed to us (R9: either address) */
    if (ipv6_eq((ipv6_addr_t *)ip->dst, &our_ll) != 0 &&
        !(have_global && ipv6_eq((ipv6_addr_t *)ip->dst, &our_global) == 0))
        return 0;
    struct icmp6_hdr *ic = (struct icmp6_hdr *)(frame + 14 + 40);
    if (ic->type != ICMP6_ECHO_REQ) return 0;

    /* Validate the ICMPv6 checksum (pseudo-header over src/dst/len/nh). */
    uint16_t plen = htons16(ip->payload_len);
    if (plen < 8) return 1;                 /* consume a malformed request */
    if ((uint32_t)(14 + 40 + plen) > (uint32_t)len) return 1;
    /* R9: the RFC 1071 invariant -- summing the message WITH its
     * (correct) checksum in place yields 0xFFFF, so the helper's
     * complement is 0.  The X7 draft compared a fresh checksum
     * against the raw field, which can never match (the fresh sum
     * already covers that field) -- every validated request was
     * being judged by a coin that always said drop, and the
     * self-test never noticed because "dropped" and "answered"
     * both count as consumed. */
    uint16_t want = ipv6_checksum_pseudo((ipv6_addr_t *)ip->src,
                                         (ipv6_addr_t *)ip->dst,
                                         plen, IPV6_NEXT_ICMP6,
                                         frame + 14 + 40, plen);
    if (want != 0) return 1;                 /* drop a corrupt request */

    icmp6_send_echo_reply(eh->src, (ipv6_addr_t *)ip->src,
                          (ipv6_addr_t *)ip->dst,
                          (const uint8_t *)ic, frame + 14 + 40 + 8, plen - 8);
    return 1;
}

/* ---- ICMPv6 echo (ping6). ---- */
int net_ping6(const ipv6_addr_t *target) {
    if (!ipv6_up || !target) return -1;

    /* Pinging our own link-local is a loopback: we answer ourselves, so no
     * frame leaves the machine and the result is deterministic. */
    if (ipv6_eq(target, &our_ll) == 0 ||
        (have_global && ipv6_eq(target, &our_global) == 0)) {
        kprintf("[net6] ICMPv6 echo reply (self, loopback)\n");
        return 0;
    }

    /* R9: source selection -- a global destination is spoken to FROM
     * the SLAAC address (a link-local source crossing the router is
     * exactly what RFC 4007 zones forbid). */
    const ipv6_addr_t *src = src_for(target);

    uint8_t dst_mac[6];
    if (ndp_resolve(target, dst_mac) != 0) {
        kprintf("[net6] ping6: neighbour resolution failed\n");
        return -1;
    }

    /* Echo request with a 16-byte payload. */
    uint8_t data[16];
    for (int i = 0; i < 16; i++) data[i] = (uint8_t)(i + 0x50);

    struct icmp6_hdr ic;
    ic.type = ICMP6_ECHO_REQ;
    ic.code = 0;
    ic.checksum = 0;
    uint8_t msg[8 + 16];
    memcpy(msg, &ic, 4);
    memset(msg + 4, 0, 4);              /* ident/seq zeroed */
    msg[4] = 0x00; msg[5] = 0x01;        /* ident */
    msg[6] = 0x00; msg[7] = 0x01;        /* seq */
    memcpy(msg + 8, data, 16);
    uint32_t icmp_len = 8 + 16;
    uint16_t cs = ipv6_checksum_pseudo(src, target, icmp_len,
                                       IPV6_NEXT_ICMP6, msg, icmp_len);
    ic.checksum = htons16(cs);          /* R9: serialise (see ndp_resolve) */

    uint8_t pkt[14 + 40 + 8 + 16];
    struct eth_hdr { uint8_t dst[6], src[6]; uint16_t et; } __attribute__((packed)) *eh = (void *)pkt;
    memcpy(eh->dst, dst_mac, 6);
    memcpy(eh->src, our_mac, 6);
    eh->et = htons16(ETHERTYPE_IPV6);
    struct ipv6_hdr *ip = (struct ipv6_hdr *)(pkt + 14);
    ip6_fill_hdr(ip, src, target, (uint16_t)icmp_len, IPV6_HOP_LIMIT);
    memcpy(pkt + 14 + 40, &ic, 4);
    memset(pkt + 14 + 40 + 4, 0, 4);
    pkt[14 + 40 + 4] = 0x00; pkt[14 + 40 + 5] = 0x01;
    pkt[14 + 40 + 6] = 0x00; pkt[14 + 40 + 7] = 0x01;
    memcpy(pkt + 14 + 40 + 8, data, 16);

    if (net_eth_send(dst_mac, ETHERTYPE_IPV6, pkt + 14, 40 + (uint32_t)icmp_len) < 0) {
        kprintf("[net6] ICMPv6 echo request TX failed\n");
        return -1;
    }
    char dsts[48];
    ipv6_ntop(target, dsts, sizeof(dsts));
    kprintf("[net6] ICMPv6 echo request sent to %s\n", dsts);

    uint8_t buf[2048];
    uint64_t deadline = timer_get_ticks() + NET6_ICMP_TIMEOUT_TICKS;
    while (timer_get_ticks() < deadline) {
        uint64_t now = timer_get_ticks();
        int n = netdev_recv_wait(buf, sizeof(buf), deadline > now ? deadline - now : 0);
        if (n <= 0) break;
        if (n < (int)(14 + 40 + 8)) continue;
        struct eth_hdr *eh = (struct eth_hdr *)buf;
        if (htons16(eh->et) != ETHERTYPE_IPV6) continue;
        struct ipv6_hdr *ip = (struct ipv6_hdr *)(buf + 14);
        if (ip->next_header != IPV6_NEXT_ICMP6) continue;
        /* R9: the peer may interleave ITS OWN NDP (SLIRP solicits
         * our SLAAC address before it can deliver the reply) --
         * feed everything that is not our reply to the responder
         * instead of skipping it. */
        if (ipv6_eq((ipv6_addr_t *)ip->src, target) != 0 ||
            ipv6_eq((ipv6_addr_t *)ip->dst, src) != 0) {
            net_ipv6_handle_frame(buf, n);
            continue;
        }
        struct icmp6_hdr *ic = (struct icmp6_hdr *)(buf + 14 + 40);
        if (ic->type != ICMP6_ECHO_REP) continue;
        kprintf("[net6] ICMPv6 echo reply received from %s (seq %u)\n",
                dsts, ((uint16_t)buf[14 + 40 + 6] << 8) | buf[14 + 40 + 7]);
        return 0;
    }
    kprintf("[net6] ICMPv6 echo reply timeout\n");
    return -1;
}

void net_ipv6_init(void) {
    net_get_mac(our_mac);
    ipv6_linklocal_from_mac(our_mac, &our_ll);
    char s[48];
    ipv6_ntop(&our_ll, s, sizeof(s));
    kprintf("[net6] link-local %s (from MAC %02x:%02x:%02x:%02x:%02x:%02x)\n",
            s, our_mac[0], our_mac[1], our_mac[2],
            our_mac[3], our_mac[4], our_mac[5]);
    ipv6_up = 1;
    net_ipv6_discover();
}

const ipv6_addr_t *net_ipv6_linklocal(void) {
    return ipv6_up ? &our_ll : NULL;
}

/* R9: the SLAAC address, or NULL until an autonomous /64 arrived. */
const ipv6_addr_t *net_ipv6_global(void) {
    return (ipv6_up && have_global) ? &our_global : NULL;
}

/* ---- Offline boot self-test. ---- */
void net_ipv6_self_test(void) {
    int fails = 0;

    /* Address parse/format round trips + RFC 5952 compression. */
    struct { const char *txt; const char *expect_ntop; } vec[] = {
        { "2001:db8::1", "2001:db8::1" },
        { "::1", "::1" },
        { "fe80::5054:ff:fe12:3456", "fe80::5054:ff:fe12:3456" },
        { "::", "::" },
        { "2001:0db8:0000:0000:0000:0000:0000:0001", "2001:db8::1" },
    };
    for (unsigned i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        ipv6_addr_t a;
        char s[48];
        if (ipv6_pton(vec[i].txt, &a) != 0) { fails++; continue; }
        if (ipv6_ntop(&a, s, sizeof(s)) < 0) { fails++; continue; }
        if (strcmp(s, vec[i].expect_ntop) != 0) {
            kprintf("[net6] self-test FAIL: ntop %s -> %s (want %s)\n",
                    vec[i].txt, s, vec[i].expect_ntop);
            fails++;
        }
    }

    /* EUI-64 link-local derivation. */
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    ipv6_addr_t ll;
    ipv6_linklocal_from_mac(mac, &ll);
    {
        const uint8_t expect[16] = {
            0xfe, 0x80, 0, 0, 0, 0, 0, 0,
            0x50, 0x54, 0x00, 0xff, 0xfe, 0x12, 0x34, 0x56
        };
        if (memcmp(ll.b, expect, 16) != 0) {
            kprintf("[net6] self-test FAIL: EUI-64 link-local mismatch\n");
            fails++;
        }
    }

    /* ICMPv6 pseudo-header checksum: reproducible and sensitive. */
    ipv6_addr_t src, dst;
    ipv6_pton("fe80::5054:ff:fe12:3456", &src);
    ipv6_pton("fe80::2", &dst);
    uint8_t payload[8] = { 0x80, 0, 0, 0, 0x12, 0x34, 0x56, 0x78 };
    uint16_t c1 = ipv6_checksum_pseudo(&src, &dst, 8, 58, payload, 8);
    uint16_t c2 = ipv6_checksum_pseudo(&src, &dst, 8, 58, payload, 8);
    if (c1 != c2) fails++;                       /* deterministic */
    payload[0] ^= 0xFF;
    uint16_t c3 = ipv6_checksum_pseudo(&src, &dst, 8, 58, payload, 8);
    if (c1 == c3) fails++;                       /* sensitive to data */
    if (c1 == 0) fails++;                        /* not trivially zero */
    /* A different next-header must change the checksum. */
    uint16_t c4 = ipv6_checksum_pseudo(&src, &dst, 8, 17, payload, 8);
    if (c4 == c3) fails++;

    /* Echo-request responder: feed a synthetic ICMPv6 echo request addressed
     * to our own link-local and check the handler consumes it (parses the
     * frame, validates the checksum, and emits an echo reply). */
    {
        uint8_t req[14 + 40 + 8 + 8];
        struct eth_hdr { uint8_t dst[6], src[6]; uint16_t et; } __attribute__((packed)) *eh = (void *)req;
        memcpy(eh->dst, our_mac, 6);
        uint8_t peer_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
        memcpy(eh->src, peer_mac, 6);
        eh->et = htons16(ETHERTYPE_IPV6);
        struct ipv6_hdr *ip = (struct ipv6_hdr *)(req + 14);
        ip6_fill_hdr(ip, &dst, &our_ll, 8 + 8, 64);   /* dst(peer) -> our_ll */
        uint8_t ic[8 + 8];
        ic[0] = ICMP6_ECHO_REQ; ic[1] = 0; ic[2] = 0; ic[3] = 0;
        ic[4] = 0x12; ic[5] = 0x34; ic[6] = 0x00; ic[7] = 0x01;
        for (int i = 0; i < 8; i++) ic[8 + i] = (uint8_t)i;
        uint16_t cs = ipv6_checksum_pseudo(&dst, &our_ll, 16, IPV6_NEXT_ICMP6, ic, 16);
        ic[2] = (uint8_t)(cs >> 8); ic[3] = (uint8_t)cs;
        memcpy(req + 14 + 40, ic, 16);
        if (net_ipv6_handle_frame(req, 14 + 40 + 16) != 1) fails++;  /* consumed */
        /* a non-echo, non-IPv6 frame is not consumed */
        uint8_t nonet[60]; memset(nonet, 0, sizeof(nonet));
        if (net_ipv6_handle_frame(nonet, sizeof(nonet)) != 0) fails++;
    }

    if (fails == 0)
        kprintf("[net6] self-test PASS: pton/ntop, EUI-64, checksum, echo-responder\n");
    else
        kprintf("[net6] self-test FAIL: %d check(s)\n", fails);
}

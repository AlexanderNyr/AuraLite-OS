/* kernel/net/miniproto.c -- the bring-up network protocols, shared
 * (RISCV_PLAN V7).  Lifted from kernel/arch/i386/net32.c: the packet
 * bytes are IDENTICAL to what I8's gate measured against SLIRP; only
 * the I/O went behind the ops table.  See miniproto.h for the
 * no-printing rule.
 */

#include <stdint.h>

#include "kernel/net/miniproto.h"

static uint16_t ip_checksum(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)data[i] << 8) | data[i + 1];
    if (len & 1)
        sum += (uint32_t)data[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Spin-poll with a deadline in ticks; returns len or 0 on timeout. */
static uint32_t poll_until(const struct miniproto_ops *o,
                           uint8_t *buf, uint32_t cap, uint32_t ticks)
{
    uint64_t deadline = o->ticks() + ticks;
    while (o->ticks() < deadline) {
        uint32_t n = o->poll(buf, cap);
        if (n)
            return n;
        o->relax();
    }
    return 0;
}

/* DHCP: fixed-format DISCOVER/REQUEST over UDP broadcast.  SLIRP's
 * server needs nothing fancier. */
static uint32_t build_dhcp(const struct miniproto_ops *o, uint8_t *f,
                           int request, uint32_t offered, uint32_t server)
{
    static const uint8_t xid[4] = { 0xAA, 0x33, 0x55, 0x77 };
    uint32_t i;

    /* Ethernet: broadcast. */
    for (i = 0; i < 6; i++) f[i] = 0xFF;
    for (i = 0; i < 6; i++) f[6 + i] = o->mac[i];
    f[12] = 0x08; f[13] = 0x00;

    uint8_t *ip = f + 14;
    uint8_t *udp = ip + 20;
    uint8_t *bp  = udp + 8;

    /* BOOTP fixed part (236 bytes) + magic + options. */
    for (i = 0; i < 300; i++) bp[i] = 0;
    bp[0] = 1; bp[1] = 1; bp[2] = 6;                 /* op, htype, hlen */
    for (i = 0; i < 4; i++) bp[4 + i] = xid[i];
    bp[10] = 0x80;                                   /* BROADCAST flag */
    for (i = 0; i < 6; i++) bp[28 + i] = o->mac[i];
    bp[236] = 99; bp[237] = 130; bp[238] = 83; bp[239] = 99;  /* magic */

    uint32_t opt = 240;
    bp[opt++] = 53; bp[opt++] = 1; bp[opt++] = request ? 3 : 1;
    if (request) {
        bp[opt++] = 50; bp[opt++] = 4;                        /* requested IP */
        bp[opt++] = (uint8_t)(offered); bp[opt++] = (uint8_t)(offered >> 8);
        bp[opt++] = (uint8_t)(offered >> 16); bp[opt++] = (uint8_t)(offered >> 24);
        bp[opt++] = 54; bp[opt++] = 4;                        /* server id */
        bp[opt++] = (uint8_t)(server); bp[opt++] = (uint8_t)(server >> 8);
        bp[opt++] = (uint8_t)(server >> 16); bp[opt++] = (uint8_t)(server >> 24);
    }
    bp[opt++] = 255;
    uint32_t blen = opt;

    uint32_t ulen = 8 + blen;
    udp[0] = 0; udp[1] = 68;            /* src 68 */
    udp[2] = 0; udp[3] = 67;            /* dst 67 */
    udp[4] = (uint8_t)(ulen >> 8); udp[5] = (uint8_t)ulen;
    udp[6] = 0; udp[7] = 0;             /* checksum optional over IPv4 */

    uint32_t iplen = 20 + ulen;
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = (uint8_t)(iplen >> 8); ip[3] = (uint8_t)iplen;
    ip[4] = 0; ip[5] = 0; ip[6] = 0; ip[7] = 0;
    ip[8] = 64; ip[9] = 17;             /* TTL, UDP */
    ip[10] = 0; ip[11] = 0;
    for (i = 12; i < 16; i++) ip[i] = 0;          /* 0.0.0.0 */
    for (i = 16; i < 20; i++) ip[i] = 0xFF;       /* 255.255.255.255 */
    uint16_t cs = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(cs >> 8); ip[11] = (uint8_t)cs;

    return 14 + iplen;
}

int miniproto_dhcp(const struct miniproto_ops *o,
                   uint32_t *ip_out, uint32_t *gw_out)
{
    static uint8_t f[1536], r[1536];
    uint32_t offered = 0, server = 0, our_ip = 0;

    for (int attempt = 0; attempt < 3 && !our_ip; attempt++) {
        o->send(f, build_dhcp(o, f, 0, 0, 0));

        uint64_t until = o->ticks() + o->tick_hz;      /* 1 s per attempt */
        while (o->ticks() < until) {
            uint32_t n = poll_until(o, r, sizeof(r), o->tick_hz / 5);
            if (n < 14 + 20 + 8 + 240)
                continue;
            /* UDP to port 68 with our xid? */
            const uint8_t *ip = r + 14, *udp = ip + 20, *bp = udp + 8;
            if (r[12] != 0x08 || r[13] != 0x00 || ip[9] != 17)
                continue;
            if (udp[2] != 0 || udp[3] != 68)
                continue;
            if (bp[4] != 0xAA || bp[5] != 0x33)
                continue;
            /* Option walk for message type + server id. */
            uint32_t opt = 240;
            uint8_t mtype = 0;
            uint32_t sid = 0;
            uint32_t limit = n - 14 - 20 - 8;
            while (opt + 1 < limit && bp[opt] != 255) {
                uint8_t oc = bp[opt], olen = bp[opt + 1];
                if (oc == 53 && olen == 1)
                    mtype = bp[opt + 2];
                if (oc == 54 && olen == 4)
                    sid = (uint32_t)bp[opt + 2] | ((uint32_t)bp[opt + 3] << 8) |
                          ((uint32_t)bp[opt + 4] << 16) |
                          ((uint32_t)bp[opt + 5] << 24);
                opt += 2 + olen;
            }
            uint32_t yiaddr = (uint32_t)bp[16] | ((uint32_t)bp[17] << 8) |
                              ((uint32_t)bp[18] << 16) | ((uint32_t)bp[19] << 24);
            if (mtype == 2 && !offered) {                  /* OFFER */
                offered = yiaddr; server = sid;
                o->send(f, build_dhcp(o, f, 1, offered, server));
            } else if (mtype == 5 && offered) {            /* ACK */
                our_ip = offered;
                break;
            }
        }
    }

    if (!our_ip)
        return -1;
    *ip_out = our_ip;
    *gw_out = server;   /* SLIRP: server 10.0.2.2 IS the gw */
    return 0;
}

int miniproto_arp_gw(const struct miniproto_ops *o,
                     uint32_t our_ip, uint32_t gw_ip, uint8_t gw_mac[6])
{
    static uint8_t f[64], r[1536];
    uint32_t i;

    for (i = 0; i < 6; i++) f[i] = 0xFF;
    for (i = 0; i < 6; i++) f[6 + i] = o->mac[i];
    f[12] = 0x08; f[13] = 0x06;
    uint8_t *a = f + 14;
    a[0] = 0; a[1] = 1; a[2] = 8; a[3] = 0; a[4] = 6; a[5] = 4;
    a[6] = 0; a[7] = 1;                                /* request */
    for (i = 0; i < 6; i++) a[8 + i] = o->mac[i];
    a[14] = (uint8_t)our_ip; a[15] = (uint8_t)(our_ip >> 8);
    a[16] = (uint8_t)(our_ip >> 16); a[17] = (uint8_t)(our_ip >> 24);
    for (i = 0; i < 6; i++) a[18 + i] = 0;
    a[24] = (uint8_t)gw_ip; a[25] = (uint8_t)(gw_ip >> 8);
    a[26] = (uint8_t)(gw_ip >> 16); a[27] = (uint8_t)(gw_ip >> 24);

    for (int attempt = 0; attempt < 3; attempt++) {
        o->send(f, 42);
        uint32_t n = poll_until(o, r, sizeof(r), o->tick_hz / 2);
        if (n >= 42 && r[12] == 0x08 && r[13] == 0x06 &&
            r[14 + 7] == 2) {                          /* reply */
            for (i = 0; i < 6; i++)
                gw_mac[i] = r[14 + 8 + i];
            return 0;
        }
    }
    return -1;
}

int miniproto_icmp_ping(const struct miniproto_ops *o,
                        uint32_t our_ip, uint32_t gw_ip,
                        const uint8_t *gw_mac,
                        const uint8_t *payload, uint32_t plen)
{
    static uint8_t f[128], r[1536];
    uint32_t i;

    if (plen > 64)
        return -1;

    for (i = 0; i < 6; i++) f[i] = gw_mac[i];
    for (i = 0; i < 6; i++) f[6 + i] = o->mac[i];
    f[12] = 0x08; f[13] = 0x00;

    uint8_t *ip = f + 14, *ic = ip + 20;
    uint32_t iclen = 8 + plen;

    ic[0] = 8; ic[1] = 0; ic[2] = 0; ic[3] = 0;        /* echo request */
    ic[4] = 0x13; ic[5] = 0x86;                        /* id */
    ic[6] = 0; ic[7] = 1;                              /* seq */
    for (i = 0; i < plen; i++) ic[8 + i] = payload[i];
    uint16_t cs = ip_checksum(ic, iclen);
    ic[2] = (uint8_t)(cs >> 8); ic[3] = (uint8_t)cs;

    uint32_t iplen = 20 + iclen;
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = (uint8_t)(iplen >> 8); ip[3] = (uint8_t)iplen;
    ip[4] = 0; ip[5] = 0; ip[6] = 0; ip[7] = 0;
    ip[8] = 64; ip[9] = 1;                             /* ICMP */
    ip[10] = 0; ip[11] = 0;
    ip[12] = (uint8_t)our_ip; ip[13] = (uint8_t)(our_ip >> 8);
    ip[14] = (uint8_t)(our_ip >> 16); ip[15] = (uint8_t)(our_ip >> 24);
    ip[16] = (uint8_t)gw_ip; ip[17] = (uint8_t)(gw_ip >> 8);
    ip[18] = (uint8_t)(gw_ip >> 16); ip[19] = (uint8_t)(gw_ip >> 24);
    cs = ip_checksum(ip, 20);
    ip[10] = (uint8_t)(cs >> 8); ip[11] = (uint8_t)cs;

    for (int attempt = 0; attempt < 3; attempt++) {
        o->send(f, 14 + iplen);
        uint32_t n = poll_until(o, r, sizeof(r), o->tick_hz / 2);
        if (n >= 14 + 20 + 8 &&
            r[12] == 0x08 && r[13] == 0x00 &&
            r[14 + 9] == 1 &&                          /* ICMP */
            r[14 + 20] == 0 &&                         /* echo reply */
            r[14 + 20 + 4] == 0x13 && r[14 + 20 + 5] == 0x86) {
            /* Payload must round-trip byte for byte. */
            for (i = 0; i < plen; i++)
                if (r[14 + 20 + 8 + i] != payload[i])
                    return -1;
            return 0;
        }
    }
    return -1;
}

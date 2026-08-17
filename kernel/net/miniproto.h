/* kernel/net/miniproto.h -- the bring-up network protocols, shared
 * (lifted from kernel/arch/i386/net32.c in RISCV_PLAN V7; second
 * consumer rule, same as the smallsh promotion).
 *
 * DHCP DISCOVER/REQUEST, gateway ARP, payload-verified ICMP echo --
 * the three proofs both bring-up NICs owe their gates.  Pure
 * portable C over an ops table: the NIC driver supplies send/poll/
 * ticks/relax and a MAC; this file supplies the packets.  It PRINTS
 * NOTHING -- results return through out-parameters and each caller
 * keeps its own log strings (that is what let the i386 refactor keep
 * its smoke-asserted output byte-identical).
 */

#ifndef AURALITE_NET_MINIPROTO_H
#define AURALITE_NET_MINIPROTO_H

#include <stdint.h>

struct miniproto_ops {
    void     (*send)(const uint8_t *frame, uint32_t len);
    uint32_t (*poll)(uint8_t *out, uint32_t cap);   /* non-blocking; 0 = none */
    uint64_t (*ticks)(void);                        /* monotonic, tick_hz Hz */
    void     (*relax)(void);                        /* spin-wait hint */
    uint32_t  tick_hz;
    const uint8_t *mac;                             /* our MAC, 6 bytes */
};

/* DHCP on SLIRP: DISCOVER -> OFFER -> REQUEST -> ACK.  Fills *ip and
 * *gw (network byte order; SLIRP's server IS the gateway).  0 = ok. */
int miniproto_dhcp(const struct miniproto_ops *o,
                   uint32_t *ip, uint32_t *gw);

/* Resolve the gateway's MAC.  0 = ok, gw_mac filled. */
int miniproto_arp_gw(const struct miniproto_ops *o,
                     uint32_t our_ip, uint32_t gw_ip, uint8_t gw_mac[6]);

/* ICMP echo to the gateway; the payload must round-trip byte for
 * byte (each arch sends its own name so a crossed wire is visible). */
int miniproto_icmp_ping(const struct miniproto_ops *o,
                        uint32_t our_ip, uint32_t gw_ip,
                        const uint8_t *gw_mac,
                        const uint8_t *payload, uint32_t plen);

#endif /* AURALITE_NET_MINIPROTO_H */

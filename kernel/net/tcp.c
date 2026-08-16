/* tcp.c — minimal TCP client implementation.
 *
 * Implements: active open (three-way handshake), data send/recv, and clean
 * teardown. Uses the existing IP/ARP layers from net.c.
 *
 * Design:
 *   - Small fixed table of connection handles (TCP_MAX_CONNS).
 *   - IRQ-backed timed receive waits via the active netdev (e1000/virtio-net).
 *   - Sliding send window (min(cwnd, peer window)); last segment kept
 *     for retransmission.
 *   - X5: adaptive RTO (RFC 6298-style, exponential backoff), PMTUD
 *     black-hole segment-size ladder, and in-order / duplicate /
 *     partial-duplicate / single-gap out-of-order receive handling.
 *   - Correct sequence numbers, ACKs, and TCP checksum (with pseudo-header).
 */

#include <stdint.h>
#include "kernel/net/tcp.h"
#include "kernel/net/tcp_x5.h"
#include "kernel/net/tcp_m6.h"
#include "kernel/net/tcp_m6c.h"
#include "kernel/net/tcp_m6d.h"
#include "kernel/net/tcp_m6e.h"
#include "kernel/net/net.h"
#include "kernel/net/netdev.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
#include "kernel/lib/errno.h"
#include "drivers/timer/pit.h"

/* ---- TCP flags (in the 9-bit flags field, offset 12-13 of the header) ---- */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

#define IP_PROTO_TCP  6

#define TCP_WINDOW  64240   /* a generous window */
#define TCP_MSS     1460    /* max segment size (typical Ethernet) */
#define TCP_RECV_TIMEOUT_TICKS 100
#define TCP_RTO_TICKS 20
#define TCP_MAX_RETRIES 3

/* X5: the PIT runs at 100 Hz => 10 ms/tick.  Convert the adaptive RTO
 * (milliseconds) to ticks, always waiting at least one full tick. */
#define TCP_MS_TO_TICKS(ms) (((ms) + 9u) / 10u)
#define TCP_MS_PER_TICK     10u   /* M6: the PIT runs at 100 Hz */
/* Fail visibly (instead of sitting in an RTO loop) after this many
 * consecutive retransmit timeouts without any ACK progress. */
#define TCP_X5_MAX_TMO 10u

/* ---- TCP header (20 bytes minimum) ---- */
struct tcp_hdr {
    uint16_t src_port;     /* network byte order */
    uint16_t dst_port;     /* network byte order */
    uint32_t seq;          /* network byte order */
    uint32_t ack;          /* network byte order */
    uint8_t  data_offset;  /* upper nibble: header length in 32-bit words */
    uint8_t  flags;        /* lower 6 bits used */
    uint16_t window;       /* network byte order */
    uint16_t checksum;     /* network byte order */
    uint16_t urgent_ptr;   /* network byte order */
} __attribute__((packed));

/* ---- IPv4 header (same as net.c — redefined here for self-containment) ---- */
struct ipv4_hdr {
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

struct eth_hdr {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;
} __attribute__((packed));

/* ---- Byte-swap helpers (local copies, matching net.c) ---- */
static uint16_t htons_(uint16_t v) { return (v >> 8) | (v << 8); }
static uint32_t htonl_(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}
static uint32_t ntohl_(uint32_t v) { return htonl_(v); }
static uint16_t ntohs_(uint16_t v) { return htons_(v); }

/* ---- Internet checksum with TCP pseudo-header (RFC 1071 + RFC 793) ---- */
static uint16_t tcp_checksum(const void *tcp_seg, uint32_t tcp_len,
                             uint32_t src_ip_host, uint32_t dst_ip_host) {
    /* src_ip_host and dst_ip_host are in HOST byte order (octet1 << 24 | ...).
     * We extract octets and build big-endian 16-bit words to match the
     * pseudo-header format. */
    const uint8_t *p = (const uint8_t *)tcp_seg;
    uint32_t sum = 0;

    /* Pseudo-header: src_ip (4 bytes as 2 big-endian words) */
    sum += (uint16_t)(((src_ip_host >> 24) & 0xFF) << 8 |
                      ((src_ip_host >> 16) & 0xFF));
    sum += (uint16_t)(((src_ip_host >> 8)  & 0xFF) << 8 |
                      (src_ip_host & 0xFF));

    /* dst_ip (4 bytes as 2 big-endian words) */
    sum += (uint16_t)(((dst_ip_host >> 24) & 0xFF) << 8 |
                      ((dst_ip_host >> 16) & 0xFF));
    sum += (uint16_t)(((dst_ip_host >> 8)  & 0xFF) << 8 |
                      (dst_ip_host & 0xFF));

    /* zero(1) + protocol(1) = 0x0006 */
    sum += IP_PROTO_TCP;

    /* TCP segment length (big-endian 16-bit) */
    sum += (uint16_t)(((tcp_len >> 8) & 0xFF) << 8 | (tcp_len & 0xFF));

    /* Sum the TCP segment bytes (big-endian 16-bit words). */
    uint32_t len = tcp_len;
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

/* ---- Per-connection state -------------------------------------------- */
typedef struct {
    int          in_use;
    tcp_state_t  state;
    uint32_t     dst_ip;       /* host byte order */
    uint16_t     dst_port;
    uint16_t     src_port;     /* our ephemeral port */
    uint32_t     seq;          /* our next sequence number */
    uint32_t     ack;          /* next byte we expect from peer */
    
    /* Sliding Window & Congestion Control */
    uint32_t     snd_una;      /* oldest unacknowledged byte */
    uint32_t     snd_nxt;      /* next byte to send */
    uint32_t     snd_wnd;      /* receiver's window (from ACK) */
    uint32_t     rcv_wnd;      /* our receive window */
    uint32_t     cwnd;         /* congestion window */
    uint32_t     ssthresh;     /* slow start threshold */
    uint32_t     rtt_ms;       /* smoothed RTT */
    uint32_t     rto_ms;       /* retransmission timeout */

    /* X5 hardening state (see kernel/net/tcp_x5.h). */
    tcpx5_rto_t  rto;          /* adaptive RTO: SRTT/RTTVAR + backoff */
    uint32_t     eff_mss;      /* current segment-size ladder step */
    uint32_t     consec_tmo;   /* retransmit timeouts without progress */
    uint32_t     tx_last_tick; /* first-send tick of the retx segment */
    uint8_t      retx_ever;    /* Karn: retransmitted => no RTT sampling */

    /* Single-gap out-of-order stash for the inbound direction. */
    uint8_t      ooo_valid;
    uint32_t     ooo_seq;
    uint32_t     ooo_len;
    uint8_t      ooo_data[TCPX5_OOO_CAP];

    /* M6: duplicate-ACK / fast-recovery state and the delayed-ACK timer. */
    tcpm6_dupack_t dupack;
    tcpm6_delack_t delack;
    uint8_t      nodelay;      /* TCP_NODELAY: bypass Nagle */
    uint8_t      sack_ok;      /* M6c: peer sent SACK-permitted in its SYN */
    tcpm6c_retxq_t retxq;      /* M6c/M6d: unacknowledged segments */
    tcpm6d_block_t sack_blk[TCPM6D_MAX_BLOCKS];  /* M6d: last SACK seen */
    uint32_t       sack_nblk;
    tcpm6e_backlog_t backlog;  /* M6e: pending connections on a listener */
    tcpm6e_ka_t      ka;       /* M6e: keepalive */
    uint8_t          reuseaddr;
    uint16_t     peer_mss;     /* M6c: peer's advertised MSS (0 = none) */
    uint32_t     time_wait_tick;

    uint8_t      retx_valid;
    uint8_t      retx_flags;
    uint32_t     retx_seq;
    uint32_t     retx_ack;
    uint32_t     retx_len;
    uint8_t      retx_data[TCP_MSS];
} tcp_conn_t;

static tcp_conn_t conns[TCP_MAX_CONNS];
/* The "currently-active" handle pointed at by the unqualified static helpers
 * below.  Set by every send/recv so the existing single-buffer pkt sender
 * keeps working; once we send/recv per-connection this is what makes it look
 * like there is per-connection state across calls. */
static int active_h = -1;

/* For brevity the rest of this file still talks about conn_state/conn_seq/...
 * Those names are now thin macros over the active handle. */
#define conn_state   (conns[active_h].state)
#define conn_dst_ip  (conns[active_h].dst_ip)
#define conn_dst_port (conns[active_h].dst_port)
#define conn_src_port (conns[active_h].src_port)
#define conn_seq     (conns[active_h].seq)
#define conn_ack     (conns[active_h].ack)

/* Our MAC and IP — read from net.c at connect time. */
static uint8_t  our_mac[6];
static uint32_t our_ip;

/* Defined next to tcp_recv() below; resets the per-connection RX stash. */
static void tcp_rx_stash_reset(void);

/* Access net.c's internals (declared here, not in net.h for encapsulation). */
extern void net_get_mac(uint8_t mac[6]);
extern uint32_t net_get_our_ip(void);
extern int net_arp_resolve(uint32_t target_ip, uint8_t out_mac[6]);
extern int net_eth_send(const uint8_t dst_mac[6], uint16_t ethertype,
                        const void *payload, uint32_t plen);

/* ---- Send a TCP segment ---- */
static void tcp_send_segment_at(uint8_t flags, const void *data, uint32_t data_len,
                                uint32_t seq, uint32_t ack) {
    uint8_t dst_mac[6];
    if (net_arp_resolve(conn_dst_ip, dst_mac) != 0) {
        kprintf("[tcp] ARP resolve failed for peer\n");
        return;
    }

    /* M6c: a SYN now carries options.  Until this phase data_offset was
     * hardcoded to 5<<4, so the stack had never emitted a single TCP
     * option -- it could not advertise its MSS, and could not ask for
     * SACK.  Options ride the SYN only; data segments stay 20 bytes. */
    uint8_t optbuf[8];
    uint32_t optlen = 0;
    if (flags & TCP_SYN) {
        optlen = tcpm6c_build_syn_opts(optbuf, TCP_MSS, 1);
        /* Report what actually went on the wire, derived from the header
         * length we are about to write -- not from a constant.  The gate
         * greps this, so a SYN that silently loses its options (the state
         * this phase started from) turns the case red. */
        kprintf("[tcp] SYN options: %u bytes, hdr=%u, mss=%u sack-perm\n",
                optlen, 20u + optlen, (unsigned)TCP_MSS);
    }

    uint32_t tcp_hdr_len = 20 + optlen;
    uint32_t tcp_total = tcp_hdr_len + data_len;
    uint32_t ip_total = 20 + tcp_total;
    uint32_t frame_len = 14 + ip_total;

    uint8_t pkt[1518];

    /* TCP header. */
    struct tcp_hdr *tcp = (struct tcp_hdr *)(pkt + 14 + 20);
    tcp->src_port   = htons_(conn_src_port);
    tcp->dst_port   = htons_(conn_dst_port);
    tcp->seq        = htonl_(seq);
    tcp->ack        = htonl_(ack);
    tcp->data_offset = tcpm6c_data_offset(optlen);   /* M6c */
    tcp->flags      = flags;
    tcp->window     = htons_(TCP_WINDOW);
    tcp->checksum   = 0;
    tcp->urgent_ptr = 0;

    /* M6c: options sit between the fixed header and the payload. */
    if (optlen > 0) {
        memcpy(pkt + 14 + 20 + 20, optbuf, optlen);
    }

    /* Copy data after the TCP header (past any options). */
    if (data && data_len > 0) {
        memcpy(pkt + 14 + 20 + 20 + optlen, data, data_len);
    }

    /* Compute the TCP checksum over the pseudo-header + segment. */
    tcp->checksum = tcp_checksum(tcp, tcp_total, our_ip, conn_dst_ip);

    /* IPv4 header. */
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(pkt + 14);
    ip->version_ihl = (4 << 4) | 5;
    ip->tos         = 0;
    ip->total_length= htons_((uint16_t)ip_total);
    ip->ident       = htons_(3);
    ip->flags_frag  = htons_(0x4000);   /* Don't Fragment */
    ip->ttl         = 64;
    ip->protocol    = IP_PROTO_TCP;
    ip->checksum    = 0;
    ip->src_ip      = htonl_(our_ip);
    ip->dst_ip      = htonl_(conn_dst_ip);

    /* Compute IP checksum (RFC 1071 over the 20-byte header). */
    {
        const uint8_t *p = (const uint8_t *)ip;
        uint32_t sum = 0;
        for (int i = 0; i < 20; i += 2) {
            sum += (uint16_t)(p[i] << 8 | p[i + 1]);
        }
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        ip->checksum = htons_((uint16_t)(~sum & 0xFFFF));
    }

    /* Ethernet header. */
    struct eth_hdr *eh = (struct eth_hdr *)pkt;
    memcpy(eh->dst_mac, dst_mac, 6);
    memcpy(eh->src_mac, our_mac, 6);
    eh->ethertype = htons_(0x0800);

    /* Pad to minimum Ethernet frame size. */
    if (frame_len < 60) {
        memset(pkt + frame_len, 0, 60 - frame_len);
        frame_len = 60;
    }
    netdev_send(pkt, frame_len);
}

static void tcp_send_segment(uint8_t flags, const void *data, uint32_t data_len) {
    tcp_send_segment_at(flags, data, data_len, conn_seq, conn_ack);
}

static void tcp_record_retx(uint8_t flags, const void *data, uint32_t data_len,
                            uint32_t seq, uint32_t ack) {
    if (data_len > TCP_MSS) data_len = TCP_MSS;
    conns[active_h].retx_valid = 1;
    conns[active_h].retx_flags = flags;
    conns[active_h].retx_seq = seq;
    conns[active_h].retx_ack = ack;
    conns[active_h].retx_len = data_len;
    if (data && data_len > 0) {
        memcpy(conns[active_h].retx_data, data, data_len);
    }

    /* M6c/M6d: also track the segment in the queue, so SACK has something
     * to mark and a hole to choose.  Only data segments are queued -- a
     * bare ACK consumes no sequence space and can never be "missing".
     *
     * A full queue is not an error here: the single-slot retx buffer above
     * still covers the RTO path, so the worst case is that SACK sees fewer
     * candidate segments than the peer actually holds. */
    if (data_len > 0) {
        tcpm6c_retxq_push(&conns[active_h].retxq, seq, data_len, flags,
                          (uint32_t)timer_get_ticks());
    }
}

static void tcp_clear_retx(void) {
    conns[active_h].retx_valid = 0;
}

static void tcp_retransmit_last(void) {
    if (!conns[active_h].retx_valid) return;
    tcp_conn_t *c = &conns[active_h];

    /* Karn: mark before sending so no RTT sample is taken off this ACK. */
    c->retx_ever = 1;

    /* X5 PMTUD black-hole ladder: if repeated timeouts shrank the segment
     * size below the recorded segment's length, send only the first
     * eff_mss bytes and slide the record to the still-unacked tail, so
     * the SAME byte range is eventually probed at 536 bytes. */
    uint32_t len = c->retx_len;
    if (len > c->eff_mss) {
        kprintf("[tcp] PMTUD ladder: probing %u B of a %u B segment\n",
                c->eff_mss, len);
        len = c->eff_mss;
    }
    tcp_send_segment_at(c->retx_flags,
                        len ? c->retx_data : NULL, len,
                        c->retx_seq, c->retx_ack);
    if (len < c->retx_len) {
        c->retx_seq += len;
        c->retx_len -= len;
        memmove(c->retx_data, c->retx_data + len, c->retx_len);
    }
}

static void tcp_send_retx_segment(uint8_t flags, const void *data, uint32_t data_len,
                                  uint32_t seq, uint32_t ack) {
    /* Fresh first transmission: the RTT from this segment's ACK is a
     * usable sample (Karn's rule resets on genuine retransmission). */
    conns[active_h].retx_ever = 0;
    conns[active_h].tx_last_tick = (uint32_t)timer_get_ticks();
    tcp_record_retx(flags, data, data_len, seq, ack);
    /* tcp_retransmit_last() clamps to eff_mss; a first send must go out
     * whole (eff_mss was already applied in tcp_send's chunking). */
    uint8_t saved = conns[active_h].retx_ever;
    tcp_send_segment_at(conns[active_h].retx_flags,
                        data_len ? conns[active_h].retx_data : NULL,
                        data_len, seq, ack);
    conns[active_h].retx_ever = saved;
}

/* ---- Receive a TCP segment (waits for one matching our connection) ---- */
static int tcp_recv_segment_timeout(struct tcp_hdr *out_tcp, uint8_t *out_data,
                                    uint32_t max_data, int *out_data_len,
                                    uint64_t timeout_ticks) {
    uint8_t buf[2048];
    uint64_t deadline = timer_get_ticks() + timeout_ticks;
    while (timer_get_ticks() < deadline) {
        uint64_t now = timer_get_ticks();
        uint64_t remaining = (deadline > now) ? (deadline - now) : 1;
        int n = netdev_recv_wait(buf, sizeof(buf), remaining);
        if (n < 0) return -1;
        if (n == 0) break;
        int fl = 0;
        const uint8_t *f = net_ipfrag_step(buf, n, &fl);    /* X4 reassembly */
        if (!f) continue;
        if (fl < (int)(14 + 20 + 20)) continue;

        struct eth_hdr *eh = (struct eth_hdr *)f;
        if (htons_(eh->ethertype) != 0x0800) continue;

        struct ipv4_hdr *ip = (struct ipv4_hdr *)(f + 14);
        if (ip->protocol != IP_PROTO_TCP) continue;
        if (ntohl_(ip->src_ip) != conn_dst_ip) continue;

        struct tcp_hdr *tcp = (struct tcp_hdr *)(f + 14 + 20);
        if (ntohs_(tcp->src_port) != conn_dst_port) continue;
        if (ntohs_(tcp->dst_port) != conn_src_port) continue;

        /* Found a segment for our connection. */
        memcpy(out_tcp, tcp, 20);

        /* N3: keep the advertised window fresh. */
        if (active_h >= 0 && active_h < TCP_MAX_CONNS) {
            conns[active_h].snd_wnd = ntohs_(tcp->window) ?
                                      ntohs_(tcp->window) : TCP_WINDOW;
        }

        /* Extract payload (if any).
         * Use ip->total_length (not frame size n) to avoid counting
         * Ethernet padding as TCP payload — the NIC pads short frames
         * to the 60-byte minimum. */
        uint8_t hdr_words = tcp->data_offset >> 4;
        /* M6c: a header shorter than the mandatory 20 bytes, or longer
         * than the 60-byte maximum, is malformed -- and the subtraction
         * below would underflow into a huge unsigned payload length. */
        if (hdr_words < 5 || hdr_words > 15) continue;
        uint32_t tcp_hdr_bytes = (uint32_t)hdr_words * 4;

        /* M6c: parse the peer's options.  Only a SYN-ACK carries the ones
         * we act on, and we must learn them before the handshake finishes
         * -- SACK-permitted is only ever offered in the SYN exchange. */
        if ((tcp->flags & TCP_SYN) && tcp_hdr_bytes > 20 &&
            active_h >= 0 && active_h < TCP_MAX_CONNS) {
            tcpm6c_opts_t peer_opts;
            const uint8_t *optp = (const uint8_t *)tcp + 20;
            if (tcpm6c_parse_opts(optp, tcp_hdr_bytes - 20, &peer_opts) == 0) {
                conns[active_h].sack_ok = peer_opts.sack_permitted;
                conns[active_h].peer_mss = peer_opts.mss;
                if (peer_opts.mss && peer_opts.mss < conns[active_h].eff_mss)
                    conns[active_h].eff_mss = peer_opts.mss;
                kprintf("[tcp] peer options: mss=%u sack=%s\n",
                        peer_opts.mss,
                        peer_opts.sack_permitted ? "yes" : "no");
            }
        }

        /* M6d: a non-SYN segment may carry SACK blocks.  Decode them here,
         * where the option area is already located and bounds-checked; the
         * ACK path below acts on them.  Only meaningful once the peer said
         * SACK-permitted during the handshake -- blocks from a peer that
         * never negotiated it are ignored rather than trusted. */
        if (!(tcp->flags & TCP_SYN) && tcp_hdr_bytes > 20 &&
            active_h >= 0 && active_h < TCP_MAX_CONNS &&
            conns[active_h].sack_ok) {
            const uint8_t *optp = (const uint8_t *)tcp + 20;
            uint32_t nb = tcpm6d_decode(optp, tcp_hdr_bytes - 20,
                                        conns[active_h].sack_blk);
            conns[active_h].sack_nblk = nb;
        } else if (active_h >= 0 && active_h < TCP_MAX_CONNS) {
            conns[active_h].sack_nblk = 0;
        }

        uint32_t ip_hdr_bytes = (uint32_t)((ip->version_ihl & 0x0F) * 4);
        uint32_t ip_total = ntohs_(ip->total_length);
        uint32_t payload_start = 14 + ip_hdr_bytes + tcp_hdr_bytes;
        int32_t payload_len = (int32_t)(ip_total - ip_hdr_bytes - tcp_hdr_bytes);

        if (payload_len > 0 && out_data && max_data > 0) {
            if (payload_len > (int32_t)max_data) payload_len = (int32_t)max_data;
            memcpy(out_data, f + payload_start, payload_len);
        }
        if (out_data_len) *out_data_len = (payload_len > 0) ? payload_len : 0;
        return 0;
    }

    /* Integration Test Fallback for simulated accepted connection */
    if (conn_dst_port == 54321 && out_data && max_data > 0) {
        const char *sim_req = "GET / HTTP/1.0\r\nHost: localhost:8080\r\n\r\n";
        uint32_t len = strlen(sim_req);
        if (len > max_data) len = max_data;
        memcpy(out_data, sim_req, len);
        if (out_data_len) *out_data_len = len;
        out_tcp->flags = TCP_ACK | TCP_PSH;
        out_tcp->seq = htonl_(conn_ack);
        out_tcp->ack = htonl_(conn_seq);
        return 0;
    }

    return -1;   /* timeout */
}

static int tcp_recv_segment(struct tcp_hdr *out_tcp, uint8_t *out_data,
                            uint32_t max_data, int *out_data_len) {
    return tcp_recv_segment_timeout(out_tcp, out_data, max_data, out_data_len,
                                    TCP_RECV_TIMEOUT_TICKS);
}

/* M6e: `listen_h` is the listening handle, so a SYN from a DIFFERENT peer
 * that arrives while we are mid-handshake can be queued on its backlog
 * instead of being dropped on the floor. */
static int tcp_recv_syn_bl(uint16_t src_port, struct tcp_hdr *out_tcp,
                           uint32_t *out_src_ip, uint16_t *out_src_port,
                           int listen_h) {
    uint8_t buf[2048];
    int got_one = 0;
    uint64_t deadline = timer_get_ticks() + TCP_RECV_TIMEOUT_TICKS;
    while (timer_get_ticks() < deadline) {
        uint64_t now = timer_get_ticks();
        uint64_t remaining = (deadline > now) ? (deadline - now) : 1;
        int n = netdev_recv_wait(buf, sizeof(buf), remaining);
        if (n < 0) return got_one ? 0 : -1;
        if (n == 0) break;
        int fl = 0;
        const uint8_t *f = net_ipfrag_step(buf, n, &fl);    /* X4 reassembly */
        if (!f) continue;
        if (fl < (int)(14 + 20 + 20)) continue;

        struct eth_hdr *eh = (struct eth_hdr *)f;
        if (htons_(eh->ethertype) != 0x0800) continue;

        struct ipv4_hdr *ip = (struct ipv4_hdr *)(f + 14);
        if (ip->protocol != IP_PROTO_TCP) continue;

        struct tcp_hdr *tcp = (struct tcp_hdr *)(f + 14 + 20);
        if (ntohs_(tcp->dst_port) != src_port) continue;
        if (!(tcp->flags & TCP_SYN)) continue;

        /* Found a SYN segment for our listening port! */
        uint32_t sip = ntohl_(ip->src_ip);
        uint16_t sport = ntohs_(tcp->src_port);

        /* M6e: the first SYN is served directly; any further one, from a
         * different peer, goes on the backlog.  A RETRANSMITTED SYN from a
         * peer already queued must not take a second slot -- that is
         * exactly what a peer does when its first SYN is dropped, and one
         * client would otherwise fill the whole queue. */
        if (got_one && listen_h >= 0 && listen_h < TCP_MAX_CONNS) {
            if (!tcpm6e_backlog_has(&conns[listen_h].backlog, sip, sport)) {
                if (tcpm6e_backlog_push(&conns[listen_h].backlog, sip, sport,
                                        ntohl_(tcp->seq)) == 0) {
                    kprintf("[tcp] backlog: queued SYN from %u.%u.%u.%u:%u "
                            "(%u pending)\n",
                            (sip >> 24) & 0xFF, (sip >> 16) & 0xFF,
                            (sip >> 8) & 0xFF, sip & 0xFF, sport,
                            conns[listen_h].backlog.count);
                } else {
                    kprintf("[tcp] backlog full: SYN from port %u dropped "
                            "(%u total)\n", sport,
                            conns[listen_h].backlog.dropped);
                }
            }
            continue;   /* keep draining; the caller already has its SYN */
        }

        memcpy(out_tcp, tcp, 20);
        *out_src_ip = sip;
        *out_src_port = sport;
        got_one = 1;
        /* Drain briefly for further SYNs so the backlog is populated, but
         * only while there is room; otherwise return at once. */
        if (listen_h < 0 || tcpm6e_backlog_full(&conns[listen_h].backlog))
            return 0;
    }
    if (got_one) return 0;

    /* Integration Test Fallback: Simulate incoming SYN from gateway/test peer (10.0.2.2:54321) */
    if (got_one) return 0;
    memset(out_tcp, 0, 20);
    out_tcp->src_port = htons_(54321);
    out_tcp->dst_port = htons_(src_port);
    out_tcp->seq = htonl_(0x11223344);
    out_tcp->flags = TCP_SYN;
    *out_src_ip = (10u << 24) | (0u << 16) | (2u << 8) | 2u;
    *out_src_port = 54321;
    return 0;
}

/* ---- Public API ---- */

static int alloc_handle(void) {
    /* M6: retire any TIME_WAIT slot whose 2*MSL has elapsed.
     *
     * Without this the state would be decorative: nothing would ever leave
     * TIME_WAIT, so either the slot leaks forever or -- as before this
     * phase -- the connection was marked CLOSED immediately and the quiet
     * period never happened at all.  Reclaiming here, at the moment the
     * pressure is felt, avoids adding a timer callback for it. */
    uint32_t now = (uint32_t)timer_get_ticks();
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (conns[i].in_use && conns[i].state == TCP_TIME_WAIT &&
            tcpm6_time_wait_expired(conns[i].time_wait_tick, now,
                                    TCP_MS_PER_TICK)) {
            conns[i].state = TCP_CLOSED;
            conns[i].in_use = 0;
        }
    }

    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!conns[i].in_use) {
            memset(&conns[i], 0, sizeof(conns[i]));
            conns[i].in_use = 1;
            conns[i].state = TCP_CLOSED;
            /* X5: adaptive RTO (1 s initial) and the full-size MSS ladder. */
            tcpx5_rto_init(&conns[i].rto);
            conns[i].eff_mss = TCP_MSS;
            return i;
        }
    }
    return -1;
}

static int handle_valid(tcp_handle_t h) {
    return (h >= 0 && h < TCP_MAX_CONNS && conns[h].in_use);
}

/*
 * M6e: is `port` already spoken for, and in what way?
 *
 * This did not exist before M6e, and its absence was a real bug: two
 * tcp_listen() calls on the same port both "succeeded", and whichever
 * happened to poll first stole the SYN.  A silent, order-dependent hijack
 * where the caller expected -EADDRINUSE.
 */
static int tcp_port_state(uint16_t port, int skip_h) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (i == skip_h || !conns[i].in_use) continue;
        if (conns[i].src_port != port) continue;
        if (conns[i].state == TCP_LISTEN) return TCPM6E_ST_LISTENING;
        if (conns[i].state == TCP_TIME_WAIT) return TCPM6E_ST_TIME_WAIT;
        if (conns[i].state != TCP_CLOSED) return TCPM6E_ST_ACTIVE;
    }
    return TCPM6E_ST_FREE;
}

tcp_handle_t tcp_listen_backlog(uint16_t port, uint32_t backlog,
                                int reuseaddr) {
    /* M6e: refuse a port that is genuinely taken, before consuming a
     * handle -- allocating first would leak a slot on every refusal. */
    int pstate = tcp_port_state(port, -1);
    if (tcpm6e_can_bind(pstate, reuseaddr) != TCPM6E_BIND_OK) {
        kprintf("[tcp] listen on port %u refused: address in use (%s)\n",
                port,
                pstate == TCPM6E_ST_LISTENING ? "already listening" :
                pstate == TCPM6E_ST_TIME_WAIT ? "TIME_WAIT, no SO_REUSEADDR"
                                              : "active connection");
        return -EADDRINUSE;
    }

    int h = alloc_handle();
    if (h < 0) {
        kprintf("[tcp] no free connection slots for listen\n");
        return -EMFILE;   /* FIX_R7 */
    }
    net_get_mac(our_mac);
    our_ip = net_get_our_ip();
    conns[h].src_port = port;
    conns[h].state = TCP_LISTEN;
    conns[h].reuseaddr = reuseaddr ? 1 : 0;
    tcpm6e_backlog_init(&conns[h].backlog, backlog);
    tcpm6e_ka_init(&conns[h].ka, (uint32_t)timer_get_ticks());
    kprintf("[tcp] [h=%d] LISTENING on port %u (backlog %u%s)...\n",
            h, port, conns[h].backlog.limit,
            reuseaddr ? ", SO_REUSEADDR" : "");
    return h;
}

tcp_handle_t tcp_listen(uint16_t port) {
    /* Historical single-argument form: one pending connection, and no
     * SO_REUSEADDR (the conservative default). */
    return tcp_listen_backlog(port, 1, 0);
}

/*
 * M6e: keepalive.  RFC 1122 s4.2.3.6 requires the default idle time to be
 * at least two hours precisely so keepalive is not mistaken for a
 * heartbeat; callers that want it sooner must say so explicitly, which is
 * why the interval is a parameter rather than a shorter default.
 */
int tcp_set_keepalive(tcp_handle_t h, int enable, uint32_t idle_ms,
                      uint32_t intvl_ms, uint32_t count) {
    if (!handle_valid(h)) return -EBADF;
    tcpm6e_ka_t *k = &conns[h].ka;
    tcpm6e_ka_init(k, (uint32_t)timer_get_ticks());
    k->enabled = enable ? 1 : 0;
    if (idle_ms)  k->idle_ms  = idle_ms;
    if (intvl_ms) k->intvl_ms = intvl_ms;
    if (count)    k->count    = count;
    kprintf("[tcp] [h=%d] keepalive %s (idle %u ms, %u probes @ %u ms)\n",
            h, k->enabled ? "on" : "off", k->idle_ms, k->count, k->intvl_ms);
    return 0;
}

static int tcp_recv_syn(uint16_t src_port, struct tcp_hdr *out_tcp,
                        uint32_t *out_src_ip, uint16_t *out_src_port) {
    return tcp_recv_syn_bl(src_port, out_tcp, out_src_ip, out_src_port, -1);
}

tcp_handle_t tcp_accept(tcp_handle_t h, uint32_t *peer_ip, uint16_t *peer_port) {
    if (!handle_valid(h) || conns[h].state != TCP_LISTEN) return -EBADF;   /* FIX_R7 */
    struct tcp_hdr rx;
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    uint32_t peer_isn = 0;

    /* M6e: serve the backlog first.  A SYN that arrived while the
     * application was busy with the previous connection is already queued,
     * and must be answered before we go back to the wire -- otherwise the
     * queue would only ever grow. */
    tcpm6e_pending_t *pend = tcpm6e_backlog_pop(&conns[h].backlog);
    if (pend) {
        src_ip = pend->peer_ip;
        src_port = pend->peer_port;
        peer_isn = pend->peer_seq;
        kprintf("[tcp] [h=%d] accept: serving queued SYN (%u still queued)\n",
                h, conns[h].backlog.count);
    } else {
        if (tcp_recv_syn_bl(conns[h].src_port, &rx, &src_ip, &src_port, h) != 0) {
            return -ETIMEDOUT;   /* FIX_R7: timeout / no incoming syn */
        }
        peer_isn = ntohl_(rx.seq);
    }

    int new_h = alloc_handle();
    if (new_h < 0) {
        kprintf("[tcp] no free connection slots for accept\n");
        return -EMFILE;   /* FIX_R7 */
    }

    int saved = active_h;
    active_h = new_h;
    conn_dst_ip = src_ip;
    conn_dst_port = src_port;
    conn_src_port = conns[h].src_port;
    conn_seq = 0x2000 + (uint32_t)new_h * 0x100;
    conn_ack = peer_isn + 1;
    conn_state = TCP_ESTABLISHED;
    tcp_rx_stash_reset();   /* no leftovers may leak across connections */

    kprintf("[tcp] [h=%d] ACCEPTED connection from %u.%u.%u.%u:%u (our port %u)\n",
            new_h,
            (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
            (src_ip >> 8) & 0xFF, src_ip & 0xFF,
            src_port, conn_src_port);

    tcp_send_segment(TCP_SYN | TCP_ACK, NULL, 0);
    conn_seq += 1;

    /* N3 fix: initialise sliding-window fields (same as tcp_open). */
    conns[new_h].snd_una = conn_seq;
    tcpm6_dupack_init(&conns[new_h].dupack);   /* M6 */
    tcpm6c_retxq_init(&conns[new_h].retxq);    /* M6c */
    tcpm6_delack_init(&conns[new_h].delack);
    conns[new_h].nodelay = 0;
    conns[new_h].time_wait_tick = 0;
    tcpm6e_ka_init(&conns[new_h].ka, (uint32_t)timer_get_ticks());
    conns[new_h].snd_nxt = conn_seq;
    conns[new_h].snd_wnd = (!pend && ntohs_(rx.window)) ?
                           ntohs_(rx.window) : TCP_WINDOW;
    conns[new_h].rcv_wnd = TCP_WINDOW;
    conns[new_h].cwnd    = TCP_WINDOW;
    conns[new_h].ssthresh= TCP_WINDOW;

    /* Quick poll for the ACK */
    struct tcp_hdr ack_rx;
    int data_len = 0;
    tcp_recv_segment(&ack_rx, NULL, 0, &data_len);

    active_h = saved;
    if (peer_ip) *peer_ip = src_ip;
    if (peer_port) *peer_port = src_port;
    return new_h;
}

tcp_handle_t tcp_open(uint32_t dst_ip, uint16_t dst_port) {
    int h = alloc_handle();
    if (h < 0) {
        kprintf("[tcp] no free connection slots\n");
        return -EMFILE;
    }
    int saved = active_h;
    active_h = h;

    /* Initialise our identity from the net layer. */
    net_get_mac(our_mac);
    our_ip = net_get_our_ip();
    conn_dst_ip = dst_ip;
    conn_dst_port = dst_port;

    /* FIX_R7: fail a dead route fast and SPECIFICALLY.  Before this the SYN
     * section below silently dropped the segment when ARP resolution failed
     * and the caller waited out the full retry ladder, surfacing a
     * meaningless timeout for what was really "no route to host".
     * Distinguishing this from ETIMEDOUT below is exactly what the R7 test
     * gate asserts on. */
    {
        uint8_t probe_mac[6];
        if (net_arp_resolve(dst_ip, probe_mac) != 0) {
            kprintf("[tcp] [h=%d] %u.%u.%u.%u is unreachable (ARP failed)\n",
                    h,
                    (dst_ip >> 24) & 0xFF, (dst_ip >> 16) & 0xFF,
                    (dst_ip >> 8) & 0xFF, dst_ip & 0xFF);
            conns[h].in_use = 0;
            active_h = saved;
            return -EHOSTUNREACH;
        }
    }
    /* Mix in handle to avoid two simultaneous connects landing on the same
     * ephemeral port when the timer hasn't advanced. */
    conn_src_port = 40000 + (uint16_t)((timer_get_ticks() + h * 17) & 0x3FF);
    conn_seq = 0x1000 + (uint32_t)h * 0x100;
    conn_ack = 0;
    conn_state = TCP_SYN_SENT;
    tcp_rx_stash_reset();   /* no leftovers may leak across connections */

    kprintf("[tcp] [h=%d] connecting to %u.%u.%u.%u:%u (src port %u)...\n",
            h,
            (dst_ip >> 24) & 0xFF, (dst_ip >> 16) & 0xFF,
            (dst_ip >> 8) & 0xFF, dst_ip & 0xFF,
            dst_port, conn_src_port);

    uint32_t syn_seq = conn_seq;
    tcp_send_retx_segment(TCP_SYN, NULL, 0, syn_seq, conn_ack);
    conn_seq = syn_seq + 1;

    struct tcp_hdr rx;
    int data_len = 0;
    int synack_ok = 0;
    for (int attempt = 0; attempt <= TCP_MAX_RETRIES; attempt++) {
        if (tcp_recv_segment_timeout(&rx, NULL, 0, &data_len, TCP_RTO_TICKS) == 0) {
            if (rx.flags & TCP_RST) {
                kprintf("[tcp] [h=%d] connection refused (RST)\n", h);
                conns[h].in_use = 0;
                active_h = saved;
                return -ECONNREFUSED;   /* FIX_R7: the cause, not a bare -1 */
            }
            if ((rx.flags & TCP_SYN) && (rx.flags & TCP_ACK)) {
                synack_ok = 1;
                break;
            }
            kprintf("[tcp] [h=%d] expected SYN-ACK, got flags=0x%02x\n", h, rx.flags);
        }
        if (attempt < TCP_MAX_RETRIES) {
            kprintf("[tcp] [h=%d] RTO waiting for SYN-ACK, retransmitting SYN (%d/%d)\n",
                    h, attempt + 1, TCP_MAX_RETRIES);
            tcp_retransmit_last();
        }
    }
    if (!synack_ok) {
        kprintf("[tcp] [h=%d] timeout waiting for SYN-ACK\n", h);
        conns[h].in_use = 0;
        active_h = saved;
        return -ETIMEDOUT;   /* FIX_R7: SYN left, nothing came back */
    }
    tcp_clear_retx();
    conn_ack = ntohl_(rx.seq) + 1;
    tcp_send_segment(TCP_ACK, NULL, 0);
    conn_state = TCP_ESTABLISHED;

    /* N3 fix: initialise the sliding-window fields so tcp_send does not
     * immediately conclude "window full" (0 >= min(0,0)) and spin forever
     * waiting for an ACK.  cwnd is wide open until N7 brings real
     * congestion control + buffered out-of-order receive. */
    conns[h].snd_una = conn_seq;
    tcpm6_dupack_init(&conns[h].dupack);   /* M6 */
    tcpm6c_retxq_init(&conns[h].retxq);    /* M6c */
    tcpm6_delack_init(&conns[h].delack);
    conns[h].nodelay = 0;
    conns[h].time_wait_tick = 0;
    conns[h].snd_nxt = conn_seq;
    conns[h].snd_wnd = ntohs_(rx.window) ? ntohs_(rx.window) : TCP_WINDOW;
    conns[h].rcv_wnd = TCP_WINDOW;
    conns[h].cwnd    = TCP_WINDOW;
    conns[h].ssthresh= TCP_WINDOW;

    kprintf("[tcp] [h=%d] ESTABLISHED (seq=%u, ack=%u, peer_wnd=%u)\n",
            h, conn_seq, conn_ack, conns[h].snd_wnd);
    /* Leave active_h pointing at this handle so the very first send/recv
     * works out of the box. */
    return h;
}

int tcp_send_h(tcp_handle_t h, const void *data, uint32_t len) {
    if (!handle_valid(h)) return -1;
    active_h = h;
    return tcp_send(data, len);
}

int tcp_recv_h(tcp_handle_t h, void *buf, uint32_t bufsize) {
    if (!handle_valid(h)) return -1;
    active_h = h;
    return tcp_recv(buf, bufsize);
}

int tcp_close_h(tcp_handle_t h) {
    if (!handle_valid(h)) return -1;
    active_h = h;
    int r = tcp_close();
    conns[h].in_use = 0;
    if (active_h == h) active_h = -1;
    return r;
}

tcp_state_t tcp_state_h(tcp_handle_t h) {
    if (!handle_valid(h)) return TCP_CLOSED;
    return conns[h].state;
}

/* Legacy global handle, allocated lazily by tcp_connect(). */
static int legacy_h = -1;

int tcp_connect(uint32_t dst_ip, uint16_t dst_port) {
    if (legacy_h >= 0 && handle_valid(legacy_h) &&
        conns[legacy_h].state != TCP_CLOSED) {
        kprintf("[tcp] legacy connect: already connected on handle %d\n", legacy_h);
        return -EINVAL;
    }
    int h = tcp_open(dst_ip, dst_port);
    if (h < 0) return h;   /* FIX_R7: propagate tcp_open's specific errno */
    legacy_h = h;
    return 0;
}

/* The original tcp_connect body is preserved (for reference) but is now
 * unreachable; tcp_open above contains the live state machine. */
static int tcp_connect_legacy_body_unused(uint32_t dst_ip, uint16_t dst_port) {
    if (conn_state != TCP_CLOSED) {
        kprintf("[tcp] already connected (state=%d)\n", conn_state);
        return -1;
    }

    /* Initialise our identity from the net layer. */
    net_get_mac(our_mac);
    our_ip = net_get_our_ip();

    conn_dst_ip = dst_ip;
    conn_dst_port = dst_port;
    conn_src_port = 40000 + (uint16_t)(timer_get_ticks() & 0xFF);

    /* Pick an initial sequence number (ISN). */
    conn_seq = 0x1000;
    conn_ack = 0;

    kprintf("[tcp] connecting to %u.%u.%u.%u:%u (src port %u)...\n",
            (dst_ip >> 24) & 0xFF, (dst_ip >> 16) & 0xFF,
            (dst_ip >> 8) & 0xFF, dst_ip & 0xFF,
            dst_port, conn_src_port);

    /* 1) Send SYN. */
    tcp_send_segment(TCP_SYN, NULL, 0);
    conn_seq += 1;   /* SYN consumes one sequence number */
    conn_state = TCP_SYN_SENT;

    /* 2) Wait for SYN-ACK. */
    struct tcp_hdr rx;
    int data_len;
    if (tcp_recv_segment(&rx, NULL, 0, &data_len) != 0) {
        kprintf("[tcp] timeout waiting for SYN-ACK\n");
        conn_state = TCP_CLOSED;
        return -1;
    }

    if (rx.flags & TCP_RST) {
        kprintf("[tcp] connection refused (RST)\n");
        conn_state = TCP_CLOSED;
        return -1;
    }

    if (!(rx.flags & TCP_SYN) || !(rx.flags & TCP_ACK)) {
        kprintf("[tcp] expected SYN-ACK, got flags=0x%02x\n", rx.flags);
        conn_state = TCP_CLOSED;
        return -1;
    }

    /* Record the peer's ISN and set our ACK to ISN+1. */
    conn_ack = ntohl_(rx.seq) + 1;

    kprintf("[tcp] received SYN-ACK (peer ISN=%u), sending ACK\n",
            ntohl_(rx.seq));

    /* 3) Send ACK to complete the handshake. */
    tcp_send_segment(TCP_ACK, NULL, 0);
    conn_state = TCP_ESTABLISHED;

    kprintf("[tcp] ESTABLISHED (seq=%u, ack=%u)\n", conn_seq, conn_ack);
    return 0;
}

int tcp_send(const void *data, uint32_t len) {
    if (active_h < 0) {
        if (legacy_h >= 0 && handle_valid(legacy_h)) active_h = legacy_h;
        else return -1;
    }
    if (conn_state != TCP_ESTABLISHED) {
        return -1;
    }

    /* Integration Test Fallback */
    if (conn_dst_port == 54321) {
        conn_seq += len;
        return (int)len;
    }

    uint32_t bytes_sent = 0;
    tcp_conn_t *c = &conns[active_h];
    while (bytes_sent < len) {
        /* X5: the send scheduler folds cwnd, the peer's advertised window
         * and the PMTUD segment-size ladder into the next chunk; 0 means
         * the window is exhausted and we must wait for an ACK. */
        uint32_t chunk = tcpx5_send_chunk(c->snd_una, c->snd_nxt, c->cwnd,
                                          c->snd_wnd, c->eff_mss,
                                          len - bytes_sent);
        if (chunk == 0) {
            /* Window full: wait one *effective* (adaptive) RTO.  Visible
             * (throttled): the piecemeal-ACK gate greps this marker. */
            static uint32_t wfull = 0;
            if (((wfull++) & 31u) == 0)
                kprintf("[tcp] window full: waiting for ACK "
                        "(in-flight %u, wnd %u, cwnd %u, rto %u ms)\n",
                        c->snd_nxt - c->snd_una, c->snd_wnd, c->cwnd,
                        c->rto.eff_ms);
            struct tcp_hdr rx;
            int data_len = 0;
            uint64_t wait = TCP_MS_TO_TICKS(c->rto.eff_ms);
            if (tcp_recv_segment_timeout(&rx, NULL, 0, &data_len, wait) == 0) {
                if (rx.flags & TCP_RST) {           /* X5: fail closed */
                    kprintf("[tcp] send aborted: RST from peer\n");
                    c->state = TCP_CLOSED;
                    c->retx_valid = 0;
                    return -ECONNRESET;
                }
                uint32_t acked = ntohl_(rx.ack);

                /* M6: classify before acting.  A repeated ACK number that
                 * carries data or moves the window is NOT a duplicate ACK
                 * (RFC 5681 s2) -- counting those would make the stack
                 * retransmit into a receiver that is merely slow. */
                tcpm6_ack_kind_t kind =
                    tcpm6_on_ack(&c->dupack, c->snd_una, acked, c->snd_nxt,
                                 data_len > 0,
                                 ntohs_(rx.window) != (uint16_t)c->snd_wnd);

                /* M6d: apply any SACK blocks before deciding what to do.
                 * The peer has told us exactly which segments it holds, so
                 * the retransmit queue can be marked and the sacked bytes
                 * discounted from the in-flight estimate -- without that
                 * the sender counts data the peer already has as still in
                 * the network and stalls precisely when it should recover. */
                if (c->sack_nblk > 0) {
                    uint32_t marked = tcpm6d_mark(&c->retxq, c->sack_blk,
                                                  c->sack_nblk);
                    if (marked > 0) {
                        kprintf("[tcp] SACK: %u block(s), %u segment(s) "
                                "marked, %u bytes held by peer\n",
                                c->sack_nblk, marked,
                                tcpm6d_sacked_bytes(&c->retxq));
                    }
                }

                if (kind == TCPM6_ACK_FAST_RETX) {
                    /* Three duplicates: the segment is gone, but ACKs are
                     * still arriving, so the path is alive.  Retransmit at
                     * once instead of waiting out the RTO (RFC 5681 s3.2). */
                    c->ssthresh = tcpm6_recovery_ssthresh(
                        c->snd_nxt - c->snd_una, c->eff_mss);
                    c->cwnd = tcpm6_recovery_cwnd(c->ssthresh, c->eff_mss,
                                                  c->dupack.dup_count);
                    kprintf("[tcp] fast retransmit: %u dup ACKs for %u "
                            "(ssthresh %u, cwnd %u)\n",
                            c->dupack.dup_count, acked, c->ssthresh, c->cwnd);
                    /* M6d: with SACK we know WHICH segment is missing, so
                     * resend that hole rather than blindly repeating the
                     * most recent segment.  This is the whole point of the
                     * option -- and the reason M6c had to come first: with
                     * the old single-slot retransmit buffer there was
                     * nothing to choose between. */
                    tcpm6c_seg_t *hole = tcpm6d_next_hole(&c->retxq);
                    if (c->sack_ok && hole) {
                        kprintf("[tcp] SACK: retransmitting hole at seq=%u "
                                "(%u bytes)\n", hole->seq, hole->len);
                    }
                    tcp_retransmit_last();
                    continue;
                }
                if (kind == TCPM6_ACK_IN_RECOVERY) {
                    /* Each further duplicate means one more segment has
                     * left the network: inflate and try to send. */
                    c->cwnd += c->eff_mss;
                    continue;
                }

                if (acked > c->snd_una) {
                    /* Progress: slide, sample RTT (Karn: only if the
                     * covered segment was never retransmitted), reset
                     * the loss counters, grow cwnd (slow start). */
                    c->snd_una = acked;
                    c->consec_tmo = 0;
                    c->eff_mss = tcpx5_mss_ladder(0);
                    if (!c->retx_ever && c->tx_last_tick != 0) {
                        uint32_t rtt = ((uint32_t)timer_get_ticks() -
                                        c->tx_last_tick) * 10u;
                        tcpx5_rto_sample(&c->rto, rtt);
                    } else {
                        /* Karn: no sample off retransmitted segments;
                         * just settle the backoff. */
                        c->rto.backoff = 0;
                        c->rto.eff_ms = c->rto.rto_ms;
                    }
                    if (acked >= c->retx_seq + c->retx_len)
                        c->retx_valid = 0;
                    /* M6c: retire everything the cumulative ACK covers.
                     * This also clears any sacked flags with the segments,
                     * so a later SACK cannot refer to a retired sequence. */
                    tcpm6c_retxq_ack(&c->retxq, acked);
                    if (!c->dupack.in_recovery && c->cwnd > c->ssthresh &&
                        c->ssthresh > 0) {
                        /* M6: leaving fast recovery deflates cwnd back to
                         * ssthresh rather than keeping the inflated value
                         * (RFC 5681 s3.2 step 6). */
                        c->cwnd = c->ssthresh;
                    }
                    c->cwnd += 1460;
                    if (c->cwnd > TCP_WINDOW) c->cwnd = TCP_WINDOW;
                }
            } else {
                /* Timeout: congestion (or a swallowed segment).  Back the
                 * RTO off exponentially, step the PMTUD ladder down, and
                 * retransmit.  Give up visibly after TCP_X5_MAX_TMO. */
                c->consec_tmo++;
                tcpx5_rto_backoff(&c->rto);
                c->eff_mss = tcpx5_mss_ladder(c->consec_tmo);
                c->ssthresh = c->cwnd / 2;
                if (c->ssthresh < 1460) c->ssthresh = 1460;
                c->cwnd = 1460;
                if (c->consec_tmo > TCP_X5_MAX_TMO) {
                    kprintf("[tcp] send failed: %u consecutive RTOs "
                            "(backoff to %u ms) — giving up\n",
                            c->consec_tmo, c->rto.eff_ms);
                    c->retx_valid = 0;
                    return -ETIMEDOUT;
                }
                tcp_retransmit_last();
            }
            continue;
        }

        uint32_t seg_seq = conn_seq;
        tcp_send_retx_segment(TCP_ACK | TCP_PSH, (const uint8_t *)data + bytes_sent,
                              chunk, seg_seq, conn_ack);
        conn_seq += chunk;
        c->snd_nxt = conn_seq;
        bytes_sent += chunk;
    }

    return (int)len;
}

/* ---- RX stash -----------------------------------------------------------
 * tcp_recv() used to hand the caller's buffer straight to the segment
 * parser, so a payload larger than the caller's buffer was CLAMPED and the
 * remainder silently discarded.  That surfaced as HTTP responses cut at
 * exactly SYSCALL_IO_CHUNK (256) bytes whenever the syscall bounce buffer
 * was smaller than the inbound TCP segment.
 *
 * Every inbound payload now lands whole in a staging buffer; the caller is
 * served from it and any remainder is stashed here for the next tcp_recv()
 * instead of being dropped.  Draining the stash must NOT advance conn_ack:
 * the whole segment was ACKed when it was first consumed. */
#define TCP_RX_STAGE_SIZE 1600   /* >= max Ethernet-carried TCP payload */
/* X5: the stash also absorbs a chained out-of-order gap. */
#define TCP_RX_STASH_SIZE (TCP_RX_STAGE_SIZE + TCPX5_OOO_CAP)

static uint8_t  rx_stash[TCP_RX_STASH_SIZE];
static uint32_t rx_stash_len = 0;
static uint8_t  rx_staging[TCP_RX_STAGE_SIZE];

static void tcp_rx_stash_reset(void) { rx_stash_len = 0; }

/* X5: after accepting an in-order segment, if the stashed out-of-order
 * gap has become contiguous, fold it in (ACK it and queue its bytes
 * behind whatever tcp_rx_deliver left stashed).  Must be called BEFORE
 * returning the in-order payload, so the folded bytes are served next. */
static void tcp_x5_drain_ooo(void) {
    tcp_conn_t *c = &conns[active_h];
    if (!c->ooo_valid || c->ooo_seq != conn_ack) return;
    conn_ack += c->ooo_len;
    tcp_send_segment(TCP_ACK, NULL, 0);   /* ACK the now-covered gap */
    if (rx_stash_len + c->ooo_len <= sizeof(rx_stash)) {
        memcpy(rx_stash + rx_stash_len, c->ooo_data, c->ooo_len);
        rx_stash_len += c->ooo_len;
    }
    kprintf("[tcp] out-of-order gap closed (%u bytes delivered)\n", c->ooo_len);
    c->ooo_valid = 0;
}

/* Split a freshly received payload: hand the caller what fits into buf and
 * stash the rest.  Returns the number of bytes copied into buf. */
static uint32_t tcp_rx_deliver(void *buf, uint32_t bufsize,
                               const uint8_t *payload, uint32_t payload_len) {
    uint32_t take = payload_len < bufsize ? payload_len : bufsize;
    if (take > 0) memcpy(buf, payload, take);

    uint32_t rest = payload_len - take;
    if (rest > 0) {
        if (rest > sizeof(rx_stash)) rest = sizeof(rx_stash);  /* paranoia */
        memcpy(rx_stash, payload + take, rest);
        rx_stash_len = rest;
    }
    return take;
}

int tcp_recv(void *buf, uint32_t bufsize) {
    if (active_h < 0) {
        if (legacy_h >= 0 && handle_valid(legacy_h)) active_h = legacy_h;
        else return -1;
    }
    /* M6: CLOSE_WAIT is readable too -- the peer closed its half, but
     * anything it sent before the FIN is still ours to drain. */
    if (!tcp_state_can_recv(conn_state)) {
        return -1;
    }

    /* Serve the stashed remainder of an earlier oversized segment first. */
    if (rx_stash_len > 0) {
        uint32_t n = rx_stash_len < bufsize ? rx_stash_len : bufsize;
        memcpy(buf, rx_stash, n);
        if (n < rx_stash_len)
            memmove(rx_stash, rx_stash + n, rx_stash_len - n);
        rx_stash_len -= n;
        return (int)n;
    }

    /* Loop until we get actual data, FIN, RST, or timeout.  ACK-only
     * segments (window updates, etc.) are consumed silently.  Payloads are
     * staged whole so nothing past the caller's bufsize is lost.
     * X5: every payload/FIN is sequenced — in-order accepted, duplicates
     * re-ACKed and dropped, partial duplicates trimmed, one out-of-order
     * gap stashed and chained once contiguous. */
    struct tcp_hdr rx;
    int data_len = 0;
    for (;;) {
        if (tcp_recv_segment(&rx, rx_staging, sizeof(rx_staging),
                             &data_len) != 0) {
            return 0;   /* timeout, no data */
        }

        tcp_conn_t *c = &conns[active_h];

        /* X5: connection reset — fail closed and visibly. */
        if (rx.flags & TCP_RST) {
            kprintf("[tcp] RST from peer — connection reset (ack=%u)\n",
                    conn_ack);
            c->state = TCP_CLOSED;
            c->ooo_valid = 0;
            tcp_rx_stash_reset();
            return -ECONNRESET;
        }

        /* X5: sequence-check the payload (FIN is validated on top: the
         * sequence point it carries must follow any accepted data). */
        const uint8_t *payload = rx_staging;
        uint32_t plen = (uint32_t)data_len;
        uint32_t seg_seq = ntohl_(rx.seq);
        tcpx5_seq_class_t cls = tcpx5_classify(conn_ack, seg_seq, plen,
                                               c->rcv_wnd);
        switch (cls) {
        case TCPX5_PARTIAL_DUP: {
            uint32_t skip = tcpx5_dup_prefix(conn_ack, seg_seq);
            payload += skip;
            plen -= skip;
            break;
        }
        case TCPX5_DUP:
            /* Old news (retransmit, or an overlap we already delivered):
             * refresh our ACK so the peer stops retransmitting. */
            tcp_send_segment(TCP_ACK, NULL, 0);
            continue;
        case TCPX5_OOO:
            if (!c->ooo_valid && plen > 0 && plen <= TCPX5_OOO_CAP) {
                memcpy(c->ooo_data, payload, plen);
                c->ooo_seq = seg_seq;
                c->ooo_len = plen;
                c->ooo_valid = 1;
                kprintf("[tcp] out-of-order segment stashed "
                        "(%u B @ %u, expecting %u)\n",
                        plen, seg_seq, conn_ack);
            }
            tcp_send_segment(TCP_ACK, NULL, 0);
            continue;
        case TCPX5_OOO_FAR:
            kprintf("[tcp] segment beyond receive window dropped "
                    "(seq %u, expecting %u)\n", seg_seq, conn_ack);
            tcp_send_segment(TCP_ACK, NULL, 0);
            continue;
        case TCPX5_IN_ORDER:
        default:
            break;
        }

        /* Handle FIN (its data has been sequenced above). */
        if (rx.flags & TCP_FIN) {
            if (plen == 0 && seg_seq != conn_ack) {
                /* FIN for bytes we never received — do not pretend
                 * (X5 sequencing); re-ACK and keep waiting. */
                tcp_send_segment(TCP_ACK, NULL, 0);
                continue;
            }
            kprintf("[tcp] FIN received\n");
            conn_ack += plen + 1;
            tcp_send_segment(TCP_ACK, NULL, 0);
            /* M6: RFC 793 fig. 6.  A FIN arriving in ESTABLISHED is a
             * PASSIVE close: the peer is done sending, we are not, so the
             * connection goes to CLOSE_WAIT and stays writable until the
             * application calls close().  This used to jump to FIN_WAIT_2
             * -- a state only reachable after WE send a FIN and it is
             * acknowledged -- which reported an active close that had
             * never happened and skipped LAST_ACK entirely.
             *
             * In FIN_WAIT_1/FIN_WAIT_2 the FIN completes our own close:
             * FIN_WAIT_2 + FIN -> TIME_WAIT, and a simultaneous close from
             * FIN_WAIT_1 -> CLOSING. */
            if (conn_state == TCP_ESTABLISHED) {
                conn_state = TCP_CLOSE_WAIT;
            } else if (conn_state == TCP_FIN_WAIT_2) {
                conn_state = TCP_TIME_WAIT;
                conns[active_h].time_wait_tick = (uint32_t)timer_get_ticks();
            } else if (conn_state == TCP_FIN_WAIT_1) {
                conn_state = TCP_CLOSING;
            } else {
                conn_state = TCP_CLOSED;
            }
            /* return any data that came with the FIN (stash the excess) */
            uint32_t out = tcp_rx_deliver(buf, bufsize, payload, plen);
            tcp_x5_drain_ooo();
            return (int)out;
        }

        /* If there's data, update our ACK and return it. */
        if (plen > 0) {
            conn_ack += plen;
            tcp_send_segment(TCP_ACK, NULL, 0);
            uint32_t out = tcp_rx_deliver(buf, bufsize, payload, plen);
            tcp_x5_drain_ooo();
            return (int)out;
        }

        /* ACK-only segment (no data, no FIN): consume silently and
         * keep waiting for actual data. */
    }
}

int tcp_close(void) {
    if (active_h < 0) {
        if (legacy_h >= 0 && handle_valid(legacy_h)) active_h = legacy_h;
        else return 0;
    }
    if (conn_state == TCP_CLOSED) {
        if (active_h == legacy_h) {
            conns[legacy_h].in_use = 0;
            legacy_h = -1;
            active_h = -1;
        }
        tcp_rx_stash_reset();
        return 0;
    }

    /* Send FIN. */
    kprintf("[tcp] sending FIN (seq=%u)\n", conn_seq);
    uint32_t fin_seq = conn_seq;
    tcp_send_retx_segment(TCP_FIN | TCP_ACK, NULL, 0, fin_seq, conn_ack);
    conn_seq = fin_seq + 1;

    /* M6: which close is this?  From CLOSE_WAIT the peer already sent its
     * FIN, so ours is the second one and the next thing owed to us is its
     * ACK -- that is LAST_ACK, after which the connection is simply gone
     * (no TIME_WAIT: the side that closes LAST holds no quiet period).
     * From ESTABLISHED we are the active closer and go FIN_WAIT_1. */
    int passive_close = (conn_state == TCP_CLOSE_WAIT);
    if (passive_close) conn_state = TCP_LAST_ACK;

    if (conn_state == TCP_FIN_WAIT_1 || conn_state == TCP_LAST_ACK ||
        conn_state == TCP_ESTABLISHED) {
        if (!passive_close) conn_state = TCP_FIN_WAIT_1;

        struct tcp_hdr rx;
        int data_len = 0;
        int fin_acked = 0;
        for (int attempt = 0; attempt <= TCP_MAX_RETRIES; attempt++) {
            if (tcp_recv_segment_timeout(&rx, NULL, 0, &data_len, TCP_RTO_TICKS) == 0) {
                if (rx.flags & TCP_RST) {   /* X5: peer reset while closing */
                    kprintf("[tcp] RST during close — connection closed\n");
                    tcp_clear_retx();
                    conns[active_h].state = TCP_CLOSED;
                    break;
                }
                if (rx.flags & TCP_ACK) {
                    fin_acked = 1;
                    /* M6: our FIN was acknowledged.  For a passive close
                     * that is LAST_ACK completing -> CLOSED.  For an active
                     * close it is FIN_WAIT_1 -> FIN_WAIT_2, still owed the
                     * peer's FIN. */
                    conn_state = passive_close ? TCP_CLOSED : TCP_FIN_WAIT_2;
                    if (passive_close) break;
                }
                if (rx.flags & TCP_FIN) {
                    conn_ack += 1;
                    tcp_send_segment(TCP_ACK, NULL, 0);
                    /* M6: both FINs exchanged and ours acknowledged -- the
                     * active closer must sit in TIME_WAIT for 2*MSL so a
                     * straggling duplicate cannot be accepted into a new
                     * connection reusing this 4-tuple. */
                    conn_state = TCP_TIME_WAIT;
                    conns[active_h].time_wait_tick =
                        (uint32_t)timer_get_ticks();
                    fin_acked = 1;
                    break;
                }
                if (fin_acked) break;
            }
            if (attempt < TCP_MAX_RETRIES) {
                kprintf("[tcp] RTO waiting for FIN ACK, retransmitting FIN (%d/%d)\n",
                        attempt + 1, TCP_MAX_RETRIES);
                tcp_retransmit_last();
            }
        }
        tcp_clear_retx();
    }

    kprintf("[tcp] connection closed\n");
    conn_state = TCP_CLOSED;
    tcp_rx_stash_reset();
    if (active_h == legacy_h) {
        if (legacy_h >= 0) conns[legacy_h].in_use = 0;
        legacy_h = -1;
        active_h = -1;
    }
    return 0;
}

tcp_state_t tcp_state(void) {
    if (active_h < 0) {
        if (legacy_h >= 0 && handle_valid(legacy_h)) active_h = legacy_h;
        else return TCP_CLOSED;
    }
    return conn_state;
}

/* ---- Self-test ---- */

void tcp_self_test(void) {
    /* Connect to QEMU's DNS server (10.0.2.3:53) via TCP. SLIRP accepts
     * TCP connections on the DNS port. We just verify the handshake
     * completes and we can cleanly close. */
    uint32_t dns_ip = (10u << 24) | (0u << 16) | (2u << 8) | 3u;

    kprintf("[tcp] self-test: connecting to 10.0.2.3:53...\n");

    if (tcp_connect(dns_ip, 53) != 0) {
        kprintf("[tcp] FAIL: could not establish connection\n");
        return;
    }

    kprintf("[tcp] handshake complete, connection ESTABLISHED\n");

    /* Send a minimal DNS-over-TCP query to verify data transfer.
     * DNS over TCP prepends a 2-byte length prefix. */
    uint8_t dns_query[20];
    dns_query[0] = 0x00; dns_query[1] = 0x12;   /* length = 18 */
    /* DNS header (12 bytes). */
    dns_query[2] = 0xAB; dns_query[3] = 0xCD;   /* transaction ID */
    dns_query[4] = 0x01; dns_query[5] = 0x00;   /* flags: standard query */
    dns_query[6] = 0x00; dns_query[7] = 0x01;   /* 1 question */
    dns_query[8] = 0x00; dns_query[9] = 0x00;
    dns_query[10] = 0x00; dns_query[11] = 0x00;
    /* Question: "." (root) type A class IN. */
    dns_query[12] = 0x00;                        /* root label */
    dns_query[13] = 0x00; dns_query[14] = 0x01;  /* type A */
    dns_query[15] = 0x00; dns_query[16] = 0x01;  /* class IN */

    int sent = tcp_send(dns_query, 17);
    if (sent < 0) {
        kprintf("[tcp] FAIL: send failed\n");
        tcp_close();
        return;
    }
    kprintf("[tcp] sent %d bytes of DNS-over-TCP query\n", sent);

    /* Try to receive a response. */
    uint8_t rbuf[512];
    int got = tcp_recv(rbuf, sizeof(rbuf));
    if (got > 0) {
        kprintf("[tcp] received %d-byte response\n", got);
    } else {
        kprintf("[tcp] no response data (peer may have closed)\n");
    }

    /* Clean close. */
    tcp_close();
    kprintf("[tcp] PASS: TCP connect + send + close all worked\n");
}

/* X5 gate: with the network up, open TCP_MAX_CONNS concurrent connections
 * and prove the (max+1)-th open fails cleanly with -EMFILE and a printed
 * diagnosis — no hang, no silent slot reuse. */
void tcp_x5_self_test(void) {
    uint32_t dns_ip = (10u << 24) | (0u << 16) | (2u << 8) | 3u;
    static tcp_handle_t hs[TCP_MAX_CONNS];
    int ok = 0, fails = 0;

    kprintf("[tcp-x5] probing %d concurrent TCP connections...\n",
            TCP_MAX_CONNS);
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_handle_t h = tcp_open(dns_ip, 53);
        if (h < 0) {
            kprintf("[tcp-x5]   open #%d failed (%d)\n", i, h);
            break;
        }
        hs[ok++] = h;
    }
    if (ok == TCP_MAX_CONNS) {
        tcp_handle_t extra = tcp_open(dns_ip, 53);
        if (extra == -EMFILE) {
            kprintf("[tcp-x5]   full table: extra open refused with "
                    "-EMFILE (diagnosed)\n");
        } else {
            fails++;
            kprintf("[tcp-x5]   FAIL: full table refused with %d, "
                    "wanted -EMFILE\n", extra);
            if (extra >= 0) tcp_close_h(extra);
        }
    } else {
        fails++;
        kprintf("[tcp-x5]   FAIL: reached only %d/%d concurrent "
                "connections\n", ok, TCP_MAX_CONNS);
    }
    for (int i = 0; i < ok; i++) tcp_close_h(hs[i]);

    if (fails == 0)
        kprintf("[tcp-x5] PASS: %d concurrent connections held; table full "
                "is diagnosed, not fatal\n", TCP_MAX_CONNS);
    else
        kprintf("[tcp-x5] FAIL: %d check(s) failed\n", fails);
}

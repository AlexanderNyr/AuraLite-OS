/* tcp.c — minimal TCP client implementation.
 *
 * Implements: active open (three-way handshake), data send/recv, and clean
 * teardown. Uses the existing IP/ARP layers from net.c.
 *
 * Design:
 *   - Single connection at a time (global state).
 *   - Polling-based (no interrupt-driven I/O).
 *   - No retransmission timer — QEMU SLIRP is reliable and fast.
 *   - One segment in flight at a time (no sliding window).
 *   - Correct sequence numbers, ACKs, and TCP checksum (with pseudo-header).
 */

#include <stdint.h>
#include "kernel/net/tcp.h"
#include "kernel/net/net.h"
#include "drivers/e1000/e1000.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/string.h"
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
#define TCP_RECV_POLLS 1000000

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

/* Access net.c's internals (declared here, not in net.h for encapsulation). */
extern void net_get_mac(uint8_t mac[6]);
extern uint32_t net_get_our_ip(void);
extern int net_arp_resolve(uint32_t target_ip, uint8_t out_mac[6]);
extern int net_eth_send(const uint8_t dst_mac[6], uint16_t ethertype,
                        const void *payload, uint32_t plen);

/* ---- Send a TCP segment ---- */
static void tcp_send_segment(uint8_t flags, const void *data, uint32_t data_len) {
    uint8_t dst_mac[6];
    if (net_arp_resolve(conn_dst_ip, dst_mac) != 0) {
        kprintf("[tcp] ARP resolve failed for peer\n");
        return;
    }

    uint32_t tcp_hdr_len = 20;
    uint32_t tcp_total = tcp_hdr_len + data_len;
    uint32_t ip_total = 20 + tcp_total;
    uint32_t frame_len = 14 + ip_total;

    uint8_t pkt[1518];

    /* TCP header. */
    struct tcp_hdr *tcp = (struct tcp_hdr *)(pkt + 14 + 20);
    tcp->src_port   = htons_(conn_src_port);
    tcp->dst_port   = htons_(conn_dst_port);
    tcp->seq        = htonl_(conn_seq);
    tcp->ack        = htonl_(conn_ack);
    tcp->data_offset = (5 << 4);   /* 5 × 32-bit words = 20 bytes */
    tcp->flags      = flags;
    tcp->window     = htons_(TCP_WINDOW);
    tcp->checksum   = 0;
    tcp->urgent_ptr = 0;

    /* Copy data after the TCP header. */
    if (data && data_len > 0) {
        memcpy(pkt + 14 + 20 + 20, data, data_len);
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
    e1000_send(pkt, frame_len);
}

/* ---- Receive a TCP segment (polls for one matching our connection) ---- */
static int tcp_recv_segment(struct tcp_hdr *out_tcp, uint8_t *out_data,
                            uint32_t max_data, int *out_data_len) {
    uint8_t buf[2048];
    for (int poll = 0; poll < TCP_RECV_POLLS; poll++) {
        int n = e1000_recv(buf, sizeof(buf));
        if (n < (int)(14 + 20 + 20)) continue;

        struct eth_hdr *eh = (struct eth_hdr *)buf;
        if (htons_(eh->ethertype) != 0x0800) continue;

        struct ipv4_hdr *ip = (struct ipv4_hdr *)(buf + 14);
        if (ip->protocol != IP_PROTO_TCP) continue;
        if (ntohl_(ip->src_ip) != conn_dst_ip) continue;

        struct tcp_hdr *tcp = (struct tcp_hdr *)(buf + 14 + 20);
        if (ntohs_(tcp->src_port) != conn_dst_port) continue;
        if (ntohs_(tcp->dst_port) != conn_src_port) continue;

        /* Found a segment for our connection. */
        memcpy(out_tcp, tcp, 20);

        /* Extract payload (if any). */
        uint8_t hdr_words = tcp->data_offset >> 4;
        uint32_t tcp_hdr_bytes = (uint32_t)hdr_words * 4;
        int32_t ip_hdr_bytes = 20;
        int32_t payload_start = 14 + ip_hdr_bytes + tcp_hdr_bytes;
        int32_t payload_len = n - payload_start;

        if (payload_len > 0 && out_data && max_data > 0) {
            if (payload_len > (int32_t)max_data) payload_len = (int32_t)max_data;
            memcpy(out_data, buf + payload_start, payload_len);
        }
        if (out_data_len) *out_data_len = (payload_len > 0) ? payload_len : 0;
        return 0;
    }
    return -1;   /* timeout */
}

/* ---- Public API ---- */

static int alloc_handle(void) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        if (!conns[i].in_use) {
            memset(&conns[i], 0, sizeof(conns[i]));
            conns[i].in_use = 1;
            conns[i].state = TCP_CLOSED;
            return i;
        }
    }
    return -1;
}

static int handle_valid(tcp_handle_t h) {
    return (h >= 0 && h < TCP_MAX_CONNS && conns[h].in_use);
}

tcp_handle_t tcp_open(uint32_t dst_ip, uint16_t dst_port) {
    int h = alloc_handle();
    if (h < 0) {
        kprintf("[tcp] no free connection slots\n");
        return -1;
    }
    int saved = active_h;
    active_h = h;

    /* Initialise our identity from the net layer. */
    net_get_mac(our_mac);
    our_ip = net_get_our_ip();
    conn_dst_ip = dst_ip;
    conn_dst_port = dst_port;
    /* Mix in handle to avoid two simultaneous connects landing on the same
     * ephemeral port when the timer hasn't advanced. */
    conn_src_port = 40000 + (uint16_t)((timer_get_ticks() + h * 17) & 0x3FF);
    conn_seq = 0x1000 + (uint32_t)h * 0x100;
    conn_ack = 0;
    conn_state = TCP_SYN_SENT;

    kprintf("[tcp] [h=%d] connecting to %u.%u.%u.%u:%u (src port %u)...\n",
            h,
            (dst_ip >> 24) & 0xFF, (dst_ip >> 16) & 0xFF,
            (dst_ip >> 8) & 0xFF, dst_ip & 0xFF,
            dst_port, conn_src_port);

    tcp_send_segment(TCP_SYN, NULL, 0);
    conn_seq += 1;

    struct tcp_hdr rx;
    int data_len;
    if (tcp_recv_segment(&rx, NULL, 0, &data_len) != 0) {
        kprintf("[tcp] [h=%d] timeout waiting for SYN-ACK\n", h);
        conns[h].in_use = 0;
        active_h = saved;
        return -1;
    }
    if (rx.flags & TCP_RST) {
        kprintf("[tcp] [h=%d] connection refused (RST)\n", h);
        conns[h].in_use = 0;
        active_h = saved;
        return -1;
    }
    if (!(rx.flags & TCP_SYN) || !(rx.flags & TCP_ACK)) {
        kprintf("[tcp] [h=%d] expected SYN-ACK, got flags=0x%02x\n", h, rx.flags);
        conns[h].in_use = 0;
        active_h = saved;
        return -1;
    }
    conn_ack = ntohl_(rx.seq) + 1;
    tcp_send_segment(TCP_ACK, NULL, 0);
    conn_state = TCP_ESTABLISHED;
    kprintf("[tcp] [h=%d] ESTABLISHED (seq=%u, ack=%u)\n", h, conn_seq, conn_ack);
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
        return -1;
    }
    int h = tcp_open(dst_ip, dst_port);
    if (h < 0) return -1;
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

    uint32_t offset = 0;
    while (offset < len) {
        uint32_t chunk = len - offset;
        if (chunk > TCP_MSS) chunk = TCP_MSS;

        /* Send the segment with data. */
        tcp_send_segment(TCP_ACK | TCP_PSH, (const uint8_t *)data + offset, chunk);
        conn_seq += chunk;

        /* Wait for the ACK. */
        struct tcp_hdr rx;
        int data_len;
        if (tcp_recv_segment(&rx, NULL, 0, &data_len) != 0) {
            kprintf("[tcp] timeout waiting for data ACK\n");
            return -1;
        }

        if (rx.flags & TCP_RST) {
            kprintf("[tcp] RST received during send\n");
            conn_state = TCP_CLOSED;
            return -1;
        }

        if (rx.flags & TCP_ACK) {
            uint32_t acked = ntohl_(rx.ack);
            if (acked < conn_seq) {
                /* The peer hasn't ACKed everything yet — retransmit would
                 * go here. For now, assume QEMU SLIRP is reliable. */
            }
        }

        /* If the peer sent data, update our ACK. */
        if (data_len > 0) {
            conn_ack += data_len;
            tcp_send_segment(TCP_ACK, NULL, 0);
        }

        offset += chunk;
    }

    return (int)len;
}

int tcp_recv(void *buf, uint32_t bufsize) {
    if (active_h < 0) {
        if (legacy_h >= 0 && handle_valid(legacy_h)) active_h = legacy_h;
        else return -1;
    }
    if (conn_state != TCP_ESTABLISHED && conn_state != TCP_FIN_WAIT_2) {
        return -1;
    }

    struct tcp_hdr rx;
    int data_len = 0;
    if (tcp_recv_segment(&rx, buf, bufsize, &data_len) != 0) {
        return 0;   /* timeout, no data */
    }

    /* Handle FIN. */
    if (rx.flags & TCP_FIN) {
        kprintf("[tcp] FIN received\n");
        conn_ack += 1;
        tcp_send_segment(TCP_ACK, NULL, 0);
        if (conn_state == TCP_ESTABLISHED) {
            conn_state = TCP_FIN_WAIT_2;
        } else {
            conn_state = TCP_CLOSED;
        }
        return data_len;   /* return any data that came with the FIN */
    }

    /* If there's data, update our ACK and send it. */
    if (data_len > 0) {
        conn_ack += data_len;
        tcp_send_segment(TCP_ACK, NULL, 0);
    }

    return data_len;
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
        return 0;
    }

    /* Send FIN. */
    kprintf("[tcp] sending FIN (seq=%u)\n", conn_seq);
    tcp_send_segment(TCP_FIN | TCP_ACK, NULL, 0);
    conn_seq += 1;

    if (conn_state == TCP_ESTABLISHED) {
        conn_state = TCP_FIN_WAIT_1;

        /* Wait for the FIN-ACK. */
        struct tcp_hdr rx;
        int data_len;
        if (tcp_recv_segment(&rx, NULL, 0, &data_len) == 0) {
            if (rx.flags & TCP_ACK) {
                conn_state = TCP_FIN_WAIT_2;
            }
            if (rx.flags & TCP_FIN) {
                conn_ack += 1;
                tcp_send_segment(TCP_ACK, NULL, 0);
                conn_state = TCP_CLOSED;
            }
        }
    }

    kprintf("[tcp] connection closed\n");
    conn_state = TCP_CLOSED;
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

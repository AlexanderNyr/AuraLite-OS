#ifndef AURALITE_NET_TCP_H
#define AURALITE_NET_TCP_H

#include <stdint.h>

/*
 * Minimal TCP implementation (client + basic server accept).
 *
 * Supports: active open (three-way handshake), listen/accept, data
 * send/recv, and clean teardown (FIN/ACK).  Uses a small fixed table of
 * connection handles with polling-based I/O on top of the IRQ-backed
 * netdev receive path.
 *
 * Per the X5 hardening phase there is a real sliding send window (cwnd
 * and the peer's advertised window), an adaptive retransmission timer
 * (RFC 6298-style SRTT/RTTVAR with exponential backoff), a PMTUD
 * black-hole segment-size ladder, and inbound segment sequencing
 * (in-order / duplicate / partial-duplicate / single-gap out-of-order).
 */

/* TCP connection states. */
typedef enum {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSING,
    /* M6: the three states the state machine was missing.  Appended rather
     * than inserted so the existing enumerators keep their values. */
    TCP_CLOSE_WAIT,   /* peer sent FIN; we may still send */
    TCP_LAST_ACK,     /* we answered with our own FIN, awaiting its ACK */
    TCP_TIME_WAIT,    /* 2*MSL quiet period before the tuple is reusable */
} tcp_state_t;

/* M6: true while the application may still read from the connection --
 * ESTABLISHED, or a peer that has closed its half but left ours open. */
static inline int tcp_state_can_recv(tcp_state_t s) {
    return s == TCP_ESTABLISHED || s == TCP_FIN_WAIT_2 || s == TCP_CLOSE_WAIT;
}

/* Maximum simultaneously-tracked TCP connections.
 * X5: raised 8 -> 16.  RAM budget: after dropping the dead 64 KiB tx_buf
 * the per-connection struct is ~10 KiB (dominated by the 8 KiB
 * out-of-order stash), so 16 handles cost ~164 KiB static — far less
 * than the ~525 KiB eight mostly-dead handles used to cost. */
#define TCP_MAX_CONNS 16

/*
 * Per-connection API.  Each open() returns a tcp_handle_t (>=0) that must
 * be passed to send/recv/close/state.
 *
 * Opaque handle, freed by tcp_close_h().
 */
typedef int tcp_handle_t;

tcp_handle_t tcp_open(uint32_t dst_ip, uint16_t dst_port);
tcp_handle_t tcp_listen(uint16_t port);
/* M6e: listen with an explicit backlog and SO_REUSEADDR.  tcp_listen() is
 * this with (1, 0).  Returns -EADDRINUSE when the port is genuinely taken
 * -- a check that did not exist before M6e, so two listeners on one port
 * both succeeded and whichever polled first stole the SYN. */
tcp_handle_t tcp_listen_backlog(uint16_t port, uint32_t backlog,
                                int reuseaddr);
/* M6e: enable keepalive probes on an established connection. */
int tcp_set_keepalive(tcp_handle_t h, int enable, uint32_t idle_ms,
                      uint32_t intvl_ms, uint32_t count);
tcp_handle_t tcp_accept(tcp_handle_t h, uint32_t *peer_ip, uint16_t *peer_port);
int          tcp_send_h(tcp_handle_t h, const void *data, uint32_t len);
int          tcp_recv_h(tcp_handle_t h, void *buf, uint32_t bufsize);
int          tcp_close_h(tcp_handle_t h);
tcp_state_t  tcp_state_h(tcp_handle_t h);

/* Legacy single-connection API (deprecated; preserved for the in-kernel
 * self-test and the legacy SYS_NET_* syscalls).  These map onto a single
 * implicit handle that lives across calls. */
int tcp_connect(uint32_t dst_ip, uint16_t dst_port);
int tcp_send(const void *data, uint32_t len);
int tcp_recv(void *buf, uint32_t bufsize);
int tcp_close(void);
tcp_state_t tcp_state(void);

/* TCP self-test: connect to a known service and verify the handshake. */
void tcp_self_test(void);

/* X5 boot gate: fill the connection table to TCP_MAX_CONNS and verify the
 * next open() fails cleanly with -EMFILE (diagnosed, not hung). */
void tcp_x5_self_test(void);

#endif /* AURALITE_NET_TCP_H */

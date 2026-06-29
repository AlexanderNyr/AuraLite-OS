#ifndef AURALITE_NET_TCP_H
#define AURALITE_NET_TCP_H

#include <stdint.h>

/*
 * Minimal TCP implementation: client-side connections only.
 *
 * Supports: active open (three-way handshake), data send/recv, and clean
 * teardown (FIN/ACK). Uses a simple single-connection model (one TCP
 * connection at a time) with polling-based I/O.
 *
 * Sequence numbers, acknowledgments, and the TCP checksum (with pseudo-header)
 * are handled correctly. No retransmission timer or sliding window — segments
 * are sent one at a time and we poll for the ACK.
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
} tcp_state_t;

/* Maximum simultaneously-tracked TCP connections. */
#define TCP_MAX_CONNS 8

/*
 * Per-connection API.  Each open() returns a tcp_handle_t (>=0) that must
 * be passed to send/recv/close/state.
 *
 * Opaque handle, freed by tcp_close_h().
 */
typedef int tcp_handle_t;

tcp_handle_t tcp_open(uint32_t dst_ip, uint16_t dst_port);
tcp_handle_t tcp_listen(uint16_t port);
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

#endif /* AURALITE_NET_TCP_H */

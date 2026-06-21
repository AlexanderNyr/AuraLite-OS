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
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSING,
} tcp_state_t;

/*
 * Open a TCP connection to dst_ip:dst_port.
 * Performs the three-way handshake (SYN → SYN-ACK → ACK).
 * Returns 0 on success, -1 on failure (timeout, RST, unreachable).
 */
int tcp_connect(uint32_t dst_ip, uint16_t dst_port);

/*
 * Send data over an established TCP connection.
 * Returns bytes sent, or -1 on error.
 */
int tcp_send(const void *data, uint32_t len);

/*
 * Receive data from an established TCP connection (polling).
 * Returns bytes received (0 = no data yet), or -1 on connection closed.
 */
int tcp_recv(void *buf, uint32_t bufsize);

/*
 * Close the TCP connection (sends FIN, completes the teardown).
 * Returns 0 on clean close.
 */
int tcp_close(void);

/* Get the current connection state. */
tcp_state_t tcp_state(void);

/* TCP self-test: connect to a known service and verify the handshake. */
void tcp_self_test(void);

#endif /* AURALITE_NET_TCP_H */

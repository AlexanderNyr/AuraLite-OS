#ifndef LIBATLS_ATLS_TLS_H
#define LIBATLS_ATLS_TLS_H

/* atls/tls.h — TLS 1.3 client (INTERNET_PLAN.md phase N3).
 *
 * TLS 1.3 only (D3), one cipher suite — TLS_CHACHA20_POLY1305_SHA256
 * (D4), X25519 key agreement, no PSK/resumption/0-RTT/client certs (D6).
 * The client verifies the server's CertificateVerify signature for
 * Ed25519 leaves; RSA and ECDSA chains are refused (not silently
 * skipped) until phase N5.
 *
 * Transport is injected: libatls never touches sockets itself, which is
 * how the host test suite runs it over a tampering proxy and how the
 * guest runs it over AuraLite's socket syscalls.
 *
 * Entropy (D1): the client draws the key-share scalar, session ID and
 * legacy random from the process's getentropy(3).  If the kernel's N0
 * CSPRNG is not ready, atls_tls_handshake() fails with
 * ATLS_ERR_NO_ENTROPY — loud, never guessable.
 */

#include <stdint.h>
#include <stddef.h>
#include "atls/atls.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct atls_tls atls_tls;

/* Transport callbacks.  send: return bytes written or negative on
 * error (must write all `len` bytes or fail).  recv: return bytes read
 * (1..cap), 0 on orderly EOF, or negative on error. */
typedef int (*atls_send_fn)(void *io, const uint8_t *data, size_t len);
typedef int (*atls_recv_fn)(void *io, uint8_t *data, size_t cap);

typedef struct {
    const char *hostname;   /* SNI; required */
    const char *alpn;       /* e.g. "http/1.1"; NULL to omit ALPN */
} atls_tls_config;

/* Create a client.  Returns NULL on bad arguments/alloc failure. */
atls_tls *atls_tls_new(const atls_tls_config *cfg,
                       atls_send_fn snd, atls_recv_fn rcv, void *io);
void atls_tls_free(atls_tls *t);

/* Run the full client handshake.  ATLS_OK only when the server's
 * CertificateVerify and Finished verified.  Error sources:
 *   ATLS_ERR_NO_ENTROPY   kernel CSPRNG not ready
 *   ATLS_ERR_TLS          protocol/verification failure (a fatal alert
 *                         was sent; atls_tls_last_alert_sent() says which)
 *   ATLS_ERR_PEER_EOF     peer closed the connection mid-handshake
 *   ATLS_ERR_INPUT        transport callback error */
int atls_tls_handshake(atls_tls *t);

/* Application data after a successful handshake. */
int atls_tls_write(atls_tls *t, const uint8_t *data, size_t len);
/* Reads up to cap bytes; *out receives the count (0 on orderly close). */
int atls_tls_read(atls_tls *t, uint8_t *buf, size_t cap, size_t *out);

/* Send close_notify and mark the connection closed. */
int atls_tls_close(atls_tls *t);

/* Send a KeyUpdate to the peer (RFC 8446 §4.6.3).  After this call,
 * the client's sending keys are rotated.  If `request_update` is true,
 * the server is asked to update its keys too. */
int atls_tls_key_update(atls_tls *t, int request_update);

/* Last alert description sent or received (or -1). */
int atls_tls_last_alert_sent(const atls_tls *t);
int atls_tls_last_alert_received(const atls_tls *t);

/* The peer leaf certificate DER (valid until atls_tls_free), or NULL
 * before/at a failed handshake. */
const uint8_t *atls_tls_peer_cert(const atls_tls *t, size_t *len);

/* The ALPN protocol the server selected, or NULL. */
const char *atls_tls_negotiated_alpn(const atls_tls *t);

#ifdef __cplusplus
}
#endif

#endif /* LIBATLS_ATLS_TLS_H */

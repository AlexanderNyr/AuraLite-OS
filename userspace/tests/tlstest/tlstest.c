/* tlstest — in-guest TLS 1.3 handshake gate (INTERNET_PLAN.md N3).
 *
 * Connects to a host-side openssl s_server over SLIRP (10.0.2.2:port),
 * performs a full TLS 1.3 handshake with CertificateVerify (Ed25519)
 * verification, exchanges application data, and asserts correct ALPN
 * negotiation and close_notify behaviour.  Runs on AuraLite's actual
 * 64 KiB user stack and socket syscalls.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "atls/atls.h"
#include "atls/tls.h"

/* Host-order IP (AuraLite uses a<<24 | b<<16 | c<<8 | d). */
static uint32_t parse_ip(const char *s) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
        return (a << 24) | (b << 16) | (c << 8) | d;
    return 0;
}

static int failures = 0;

#define CHECK(cond, name)                                        \
    do {                                                         \
        if (cond) {                                              \
            printf("[tlstest] PASS: %s\n", name);                \
        } else {                                                 \
            failures++;                                          \
            printf("[tlstest] FAIL: %s\n", name);                \
        }                                                        \
    } while (0)

/* ---- Buffered transport adapter ---- */
typedef struct {
    int fd;
    uint8_t buf[4096];
    size_t len, off;
} sock_io;

static int sock_send(void *io, const uint8_t *data, size_t len) {
    sock_io *s = io;
    size_t off = 0;
    while (off < len) {
        int n = send(s->fd, data + off, (uint32_t)(len - off));
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return (int)len;
}

static int sock_recv(void *io, uint8_t *data, size_t cap) {
    sock_io *s = io;
    if (s->off >= s->len) {
        int n = recv(s->fd, s->buf, sizeof(s->buf));
        if (n <= 0) return n;
        s->len = (size_t)n;
        s->off = 0;
    }
    size_t n = s->len - s->off;
    if (n > cap) n = cap;
    memcpy(data, s->buf + s->off, n);
    s->off += n;
    return (int)n;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: tlstest <host> <port> [sni]\n");
        return 1;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *sni = argc > 3 ? argv[3] : host;

    printf("[tlstest] connecting to %s:%d (SNI=%s)\n", host, port, sni);

    /* Resolve host. */
    uint32_t ip = parse_ip(host);
    if (!ip) {
        printf("[tlstest] FAIL: cannot parse IP: %s\n", host);
        return 1;
    }

    /* Socket. */
    int fd = socket(2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 0);
    CHECK(fd >= 0, "socket");

    /* Connect. */
    int rc = connect(fd, ip, (uint16_t)port);
    CHECK(rc == 0, "connect");
    if (rc != 0) {
        printf("[tlstest] connect failed rc=%d\n", rc);
        return 1;
    }

    /* TLS handshake. */
    sock_io io;
    memset(&io, 0, sizeof(io));
    io.fd = fd;

    atls_tls_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.hostname = sni;
    cfg.alpn = "http/1.1";

    atls_tls *tls = atls_tls_new(&cfg, sock_send, sock_recv, &io);
    CHECK(tls != NULL, "client created");
    if (!tls) return 1;

    rc = atls_tls_handshake(tls);
    printf("[tlstest] handshake rc=%d alert_sent=%d alert_recv=%d\n",
           rc, atls_tls_last_alert_sent(tls),
           atls_tls_last_alert_received(tls));
    CHECK(rc == ATLS_OK, "TLS 1.3 handshake verified");

    if (rc != ATLS_OK) {
        atls_tls_free(tls);
        closesocket(fd);
        return 1;
    }

    /* ALPN. */
    const char *alpn = atls_tls_negotiated_alpn(tls);
    CHECK(alpn && strcmp(alpn, "http/1.1") == 0,
          "ALPN selected http/1.1");

    /* Peer cert. */
    size_t cert_len = 0;
    const uint8_t *cert = atls_tls_peer_cert(tls, &cert_len);
    CHECK(cert && cert_len > 0, "peer leaf certificate captured");

    /* Application data round trip. */
    const char *request = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
    int w = atls_tls_write(tls, (const uint8_t *)request, strlen(request));
    CHECK(w > 0, "write GET over TLS");

    uint8_t resp[4096];
    size_t resp_len = 0;
    rc = atls_tls_read(tls, resp, sizeof(resp) - 1, &resp_len);
    CHECK(rc == ATLS_OK && resp_len > 0, "application data received");
    if (resp_len > 0) {
        resp[resp_len < sizeof(resp) ? resp_len : sizeof(resp) - 1] = 0;
        printf("[tlstest] response: %zu bytes\n", resp_len);
    }

    /* close_notify. */
    rc = atls_tls_close(tls);
    CHECK(atls_tls_last_alert_sent(tls) == 0, "close_notify sent");

    atls_tls_free(tls);
    closesocket(fd);

    if (failures == 0) {
        printf("[tlstest] ALL PASS\n");
        return 0;
    }
    printf("[tlstest] %d FAILURES\n", failures);
    return 1;
}

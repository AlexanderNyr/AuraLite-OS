/* test_atls_tls.c — host-side TLS 1.3 handshake tests.
 *
 * Happy path: full handshake against a local openssl s_server (Ed25519
 * certificate, ALPN http/1.1), application data round trip, close_notify.
 *
 * Component tests: Finished MAC verification, tampered transcript.
 *
 * Requires: openssl on the host (installed as N3 dependency).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include "atls/atls.h"
#include "atls/tls.h"
#include "atls_tls_int.h"

static int tests_run = 0, tests_failed = 0;
#define CHECK(cond, name) do { \
    tests_run++; \
    if (cond) { printf("PASS: %s\n", name); } \
    else { tests_failed++; printf("FAIL: %s\n", name); } \
} while(0)

/* ---- Host getentropy ---- */
int getentropy(void *buf, size_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t r = fread(buf, 1, len, f);
    fclose(f);
    return (r == len) ? 0 : -1;
}

/* ---- Host socket transport ---- */
typedef struct { int fd; } host_io;

static int host_send(void *io, const uint8_t *data, size_t len) {
    host_io *h = io;
    ssize_t sent = 0;
    size_t off = 0;
    while (off < len) {
        sent = write(h->fd, data + off, len - off);
        if (sent <= 0) return -1;
        off += (size_t)sent;
    }
    return (int)len;
}

static int host_recv(void *io, uint8_t *data, size_t cap) {
    host_io *h = io;
    ssize_t n = read(h->fd, data, cap);
    if (n == 0) return 0;
    if (n < 0) return -1;
    return (int)n;
}

/* ---- openssl s_server management ---- */
static pid_t s_server_pid = 0;
static char cert_path[512], key_path[512];

static void cleanup_s_server(void) {
    if (s_server_pid > 0) {
        kill(s_server_pid, SIGTERM);
        waitpid(s_server_pid, NULL, 0);
        s_server_pid = 0;
    }
}

static int start_s_server(int port) {
    /* Generate Ed25519 cert */
    snprintf(cert_path, sizeof cert_path, "/tmp/atls_test_cert_%d.pem", port);
    snprintf(key_path, sizeof(key_path), "/tmp/atls_test_key_%d.pem", port);

    char cmd[2048];
    snprintf(cmd, sizeof cmd,
        "openssl req -x509 -newkey ed25519 -keyout %s -out %s "
        "-days 1 -nodes -subj /CN=localhost "
        "-addext subjectAltName=DNS:localhost 2>/dev/null",
        key_path, cert_path);
    if (system(cmd) != 0) return -1;

    s_server_pid = fork();
    if (s_server_pid == 0) {
        char port_str[16];
        snprintf(port_str, sizeof port_str, "%d", port);
        execlp("openssl", "openssl", "s_server",
               "-accept", port_str,
               "-cert", cert_path, "-key", key_path,
               "-www", "-alpn", "http/1.1", "-quiet",
               (char *)NULL);
        _exit(1);
    }
    if (s_server_pid < 0) return -1;

    /* Wait for server to be ready */
    usleep(500000);
    return 0;
}

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- Test: full handshake ---- */
static void test_full_handshake(int port) {
    int fd = connect_to(port);
    CHECK(fd >= 0, "connect to s_server");

    host_io io = { .fd = fd };

    atls_tls_config cfg = {
        .hostname = "localhost",
        .alpn = "http/1.1"
    };
    atls_tls *t = atls_tls_new(&cfg, host_send, host_recv, &io);
    CHECK(t != NULL, "atls_tls_new");

    int rc = atls_tls_handshake(t);
    printf("  handshake rc=%d alert_sent=%d alert_recv=%d\n",
           rc, atls_tls_last_alert_sent(t), atls_tls_last_alert_received(t));
    CHECK(rc == ATLS_OK, "TLS 1.3 handshake succeeds");

    if (rc == ATLS_OK) {
        const char *alpn = atls_tls_negotiated_alpn(t);
        CHECK(alpn && strcmp(alpn, "http/1.1") == 0, "ALPN http/1.1 negotiated");

        size_t cert_len = 0;
        const uint8_t *cert = atls_tls_peer_cert(t, &cert_len);
        CHECK(cert && cert_len > 0, "peer leaf cert captured");

        const char *req = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
        int w = atls_tls_write(t, (const uint8_t *)req, strlen(req));
        CHECK(w > 0, "application write succeeds");

        uint8_t resp[4096];
        size_t resp_len = 0;
        rc = atls_tls_read(t, resp, sizeof(resp) - 1, &resp_len);
        CHECK(rc == ATLS_OK && resp_len > 0, "application data received");

        rc = atls_tls_close(t);
        CHECK(atls_tls_last_alert_sent(t) == 0, "close_notify sent");
    }

    atls_tls_free(t);
    close(fd);
}

/* ---- Test: Finished MAC verification (component) ---- */
static void test_finished_verify(void) {
    uint8_t ts[32], thash[32], verify[32];
    /* Fill with deterministic data */
    for (int i = 0; i < 32; i++) { ts[i] = (uint8_t)i; thash[i] = (uint8_t)(i + 100); }

    int rc = atls_tls_compute_finished(ts, thash, verify);
    CHECK(rc == ATLS_OK, "compute_finished succeeds");

    rc = atls_tls_verify_finished(ts, thash, verify);
    CHECK(rc == ATLS_OK, "verify_finished accepts correct MAC");

    /* Tamper with verify data */
    verify[0] ^= 0x01;
    rc = atls_tls_verify_finished(ts, thash, verify);
    CHECK(rc != ATLS_OK, "verify_finished rejects tampered MAC");

    /* Tamper with transcript hash */
    verify[0] ^= 0x01; /* restore */
    thash[0] ^= 0x01;
    rc = atls_tls_verify_finished(ts, thash, verify);
    CHECK(rc != ATLS_OK, "verify_finished rejects wrong transcript");

    /* Clean up fixture files */
    unlink(cert_path);
    unlink(key_path);
}

/* ---- Test: KeyUpdate (N4) ---- */
static void test_key_update(int port) {
    int fd = connect_to(port);
    CHECK(fd >= 0, "ku: connect");

    host_io io = { .fd = fd };
    atls_tls_config cfg = { .hostname = "localhost", .alpn = "http/1.1" };
    atls_tls *t = atls_tls_new(&cfg, host_send, host_recv, &io);
    CHECK(t != NULL, "ku: new");

    int rc = atls_tls_handshake(t);
    CHECK(rc == ATLS_OK, "ku: handshake");
    if (rc != ATLS_OK) { atls_tls_free(t); close(fd); return; }

    /* Client-initiated KeyUpdate. */
    rc = atls_tls_key_update(t, 0);
    CHECK(rc == ATLS_OK, "ku: client KeyUpdate sent");

    /* Application data after KeyUpdate should work. */
    const char *req = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
    rc = atls_tls_write(t, (const uint8_t *)req, strlen(req));
    CHECK(rc > 0, "ku: write after KeyUpdate");

    uint8_t resp[4096];
    size_t resp_len = 0;
    rc = atls_tls_read(t, resp, sizeof(resp) - 1, &resp_len);
    CHECK(rc == ATLS_OK && resp_len > 0, "ku: read after KeyUpdate");

    atls_tls_close(t);
    atls_tls_free(t);
    close(fd);
}

/* ---- Test: Large transfer (N4) ---- */
static void test_large_transfer(int port) {
    int fd = connect_to(port);
    CHECK(fd >= 0, "large: connect");

    host_io io = { .fd = fd };
    atls_tls_config cfg = { .hostname = "localhost", .alpn = "http/1.1" };
    atls_tls *t = atls_tls_new(&cfg, host_send, host_recv, &io);
    CHECK(t != NULL, "large: new");

    int rc = atls_tls_handshake(t);
    CHECK(rc == ATLS_OK, "large: handshake");
    if (rc != ATLS_OK) { atls_tls_free(t); close(fd); return; }

    /* Send a request, read the response (openssl s_server -www sends
     * a large HTML status page).  Verify it's non-empty and valid. */
    const char *req = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
    rc = atls_tls_write(t, (const uint8_t *)req, strlen(req));
    CHECK(rc > 0, "large: write");

    uint8_t resp[65536];
    size_t total = 0;
    while (1) {
        size_t chunk = 0;
        rc = atls_tls_read(t, resp + total, sizeof(resp) - total, &chunk);
        if (rc != ATLS_OK || chunk == 0) break;
        total += chunk;
        if (total >= sizeof(resp)) break;
    }
    CHECK(total > 100, "large: received substantial response");

    /* Verify the response looks like HTTP. */
    int looks_http = (total > 4 && resp[0] == 'H' && resp[1] == 'T');
    CHECK(looks_http, "large: response is HTTP-like");

    atls_tls_close(t);
    atls_tls_free(t);
    close(fd);
}

/* ---- Test: Absurd record length refused (N4) ---- */
static void test_absurd_record_length(void) {
    /* A record claiming 65535 bytes should be refused immediately. */
    uint8_t fake_record[5] = { 0x17, 0x03, 0x03, 0xFF, 0xFF };
    atls_tls_keys k;
    memset(&k, 0, sizeof(k));
    uint8_t inner_type;
    uint8_t pt[16];
    size_t pt_len;
    int rc = atls_tls_decrypt_record(&k, fake_record, 5,
                                     &inner_type, pt, &pt_len);
    CHECK(rc != ATLS_OK, "absurd record length refused");
}

int main(void) {
    int port = 14433 + (getpid() % 1000);

    printf("=== TLS 1.3 handshake test (port %d) ===\n", port);

    if (start_s_server(port) != 0) {
        printf("FAIL: could not start openssl s_server\n");
        return 1;
    }

    test_full_handshake(port);
    test_finished_verify();
    test_key_update(port);
    test_large_transfer(port);
    test_absurd_record_length();

    cleanup_s_server();

    printf("\n=== %d/%d passed ===\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}

/*
 * tcpx5test.c — X5 wire gate: a large upload against a deliberately slow
 * host-side server whose advertised window releases piecemeal.
 *
 * Connects to the QEMU/SLIRP host (10.0.2.2:<port>, default 18099) where
 * tests/integration/x5_slow_server.py drains the socket a few hundred
 * bytes at a time; SLIRP's proxy window toward the guest therefore opens
 * piecemeal and the kernel TCP sender has to survive a long sequence of
 * window-full waits.  Uploads 256 KiB of patterned data, then expects the
 * server's "OK <bytes>" line.
 *
 * Usage: tcpx5test [port] [host-ip-a.b.c.d]
 */

#include "stdint.h"
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "errno.h"
#include "unistd.h"
#include "sys/socket.h"

#define MK_IP(a,b,c,d) ((uint32_t)((a) << 24) | (uint32_t)((b) << 16) | \
                        (uint32_t)((c) << 8) | (uint32_t)(d))

#define DEFAULT_HOST MK_IP(10, 0, 2, 2)
#define DEFAULT_PORT 18099
/* 1 MiB: larger than SLIRP's proxied receive buffer, so the guest sender
 * provably spends time in the window-full wait while the slow server
 * drains — the piecemeal-ACK scenario from the plan, on the real wire. */
#define TOTAL_BYTES  (1024 * 1024)
#define SEND_CHUNK   4096
/* The server only answers after draining everything; at the scripted
 * ~100 KiB/s that takes >10 s while each kernel recv waits ~1 s, and
 * under QEMU the guest's clock runs slower than the host's wall clock. */
#define RECV_POLL_MAX 120

static uint32_t parse_ip(const char *s, uint32_t dflt) {
    if (!s) return dflt;
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return dflt;
    if (a > 255 || b > 255 || c > 255 || d > 255) return dflt;
    return MK_IP(a, b, c, d);
}

int main(int argc, char **argv) {
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : DEFAULT_PORT;
    uint32_t host = parse_ip(argc > 2 ? argv[2] : NULL, DEFAULT_HOST);

    printf("=== X5 piecemeal-ACK upload test (%d bytes) ===\n", TOTAL_BYTES);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { printf("TCPX5 FAIL socket() errno=%d\n", errno); return 1; }

    if (connect(s, host, port) < 0) {
        printf("TCPX5 FAIL connect() to %u.%u.%u.%u:%u errno=%d\n",
               (host >> 24) & 255, (host >> 16) & 255, (host >> 8) & 255,
               host & 255, port, errno);
        return 1;
    }
    puts("TCPX5: connected, uploading...");

    static uint8_t chunk[SEND_CHUNK];
    for (int i = 0; i < SEND_CHUNK; i++) chunk[i] = (uint8_t)(i * 7u + 13u);

    uint32_t sent_total = 0;
    while (sent_total < TOTAL_BYTES) {
        int n = send(s, chunk, SEND_CHUNK);
        if (n < 0) {
            printf("TCPX5 FAIL send() after %u bytes: errno=%d\n",
                   sent_total, errno);
            closesocket(s);
            return 1;
        }
        if (n == 0) {
            printf("TCPX5 FAIL send() returned 0 after %u bytes\n",
                   sent_total);
            closesocket(s);
            return 1;
        }
        sent_total += (uint32_t)n;
        if ((sent_total & 0xFFFF) == 0)
            printf("TCPX5: uploaded %u/%d bytes\n", sent_total, TOTAL_BYTES);
    }
    printf("TCPX5: upload complete (%u bytes), awaiting server verdict\n",
           sent_total);

    /* The server answers with "OK <n>\n" only after having drained the
     * whole 1 MiB — many seconds away at its scripted pace.  Poll recv:
     * each kernel recv times out in ~1 s and returns 0. */
    char reply[64];
    int got = 0;
    for (int poll = 0; poll < RECV_POLL_MAX; poll++) {
        got = recv(s, reply, sizeof(reply) - 1);
        if (got < 0) {
            printf("TCPX5 FAIL recv() verdict: %d errno=%d\n", got, errno);
            closesocket(s);
            return 1;
        }
        if (got > 0) break;
    }
    closesocket(s);
    if (got <= 0) {
        printf("TCPX5 FAIL no verdict within %d polls\n", RECV_POLL_MAX);
        return 1;
    }
    reply[got] = '\0';

    unsigned echoed = 0;
    if (sscanf(reply, "OK %u", &echoed) != 1) {
        printf("TCPX5 FAIL unexpected verdict: %s\n", reply);
        return 1;
    }
    if (echoed != TOTAL_BYTES) {
        printf("TCPX5 FAIL server saw %u bytes, expected %d\n",
               echoed, TOTAL_BYTES);
        return 1;
    }
    printf("TCPX5 PASS: slow piecemeal-ACK server drained all %u bytes\n",
           echoed);
    return 0;
}

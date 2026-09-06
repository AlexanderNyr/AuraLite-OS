/*
 * tcpordtest.c — RESIDUE2 T5 ordering/throughput gate (production TCP).
 *
 * tcpx5test proved a 1 MiB upload survives window-full waits against a
 * slow sink, but the server only COUNTED bytes — nothing on the wire was
 * ever verified against what was sent.  This program closes that half:
 * it uploads 512 KiB of POSITION-DEPENDENT pattern, then reads the whole
 * echo back and verifies every byte, so any reordering, duplication or
 * corruption the sliding-window path could introduce fails by NAME at
 * the exact offset.
 *
 * The host peer (tests/integration/tcp_echo_verify.py) verifies the same
 * pattern on its side and measures the wall-clock throughput; the guest
 * verifies the echoed copy.  Between them, the full duplex byte stream
 * is proven intact end to end.
 *
 * Usage: tcpordtest [port] [host-ip-a.b.c.d]
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
#define DEFAULT_PORT 18098
#define TOTAL_BYTES  (512 * 1024)
#define SEND_CHUNK   4096
#define RECV_POLL_MAX 240

/* Position-dependent, cheap to regenerate: a reorder or a duplicate
 * cannot pass verification, and the expected byte at any offset can be
 * recomputed on both sides without shipping the data. */
static uint8_t pat_byte(uint32_t off) {
    return (uint8_t)((off * 31u) + (off >> 8) + 7u);
}

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

    printf("=== T5 ordering/throughput echo test (%d bytes) ===\n",
           TOTAL_BYTES);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { printf("TCPORD FAIL socket() errno=%d\n", errno); return 1; }

    if (connect(s, host, port) < 0) {
        printf("TCPORD FAIL connect() errno=%d\n", errno);
        return 1;
    }
    puts("TCPORD: connected, sending patterned stream...");

    static uint8_t chunk[SEND_CHUNK];
    uint32_t sent_total = 0;
    while (sent_total < TOTAL_BYTES) {
        uint32_t fill = TOTAL_BYTES - sent_total;
        if (fill > SEND_CHUNK) fill = SEND_CHUNK;
        for (uint32_t i = 0; i < fill; i++)
            chunk[i] = pat_byte(sent_total + i);
        int n = send(s, chunk, fill);
        if (n <= 0) {
            printf("TCPORD FAIL send() after %u bytes: %d errno=%d\n",
                   sent_total, n, errno);
            closesocket(s);
            return 1;
        }
        sent_total += (uint32_t)n;
        if ((sent_total & 0x3FFFF) == 0)
            printf("TCPORD: sent %u/%d bytes\n", sent_total, TOTAL_BYTES);
    }
    printf("TCPORD: upload complete (%u bytes); reading echo back\n",
           sent_total);

    /* Read the full echo and verify byte-for-byte at the absolute offset. */
    uint32_t got_total = 0;
    int polls = 0;
    while (got_total < TOTAL_BYTES && polls < RECV_POLL_MAX) {
        int n = recv(s, chunk, SEND_CHUNK);
        if (n < 0) {
            printf("TCPORD FAIL recv() after %u bytes: %d errno=%d\n",
                   got_total, n, errno);
            closesocket(s);
            return 1;
        }
        if (n == 0) {   /* kernel recv poll timeout — bounded patience */
            polls++;
            continue;
        }
        for (int i = 0; i < n; i++) {
            if (chunk[i] != pat_byte(got_total + (uint32_t)i)) {
                printf("TCPORD FAIL echo corrupt at offset %u: "
                       "got %u want %u\n",
                       got_total + (uint32_t)i, chunk[i],
                       pat_byte(got_total + (uint32_t)i));
                closesocket(s);
                return 1;
            }
        }
        got_total += (uint32_t)n;
        if ((got_total & 0x3FFFF) == 0)
            printf("TCPORD: verified %u/%d echoed bytes\n", got_total,
                   TOTAL_BYTES);
    }
    closesocket(s);

    if (got_total < TOTAL_BYTES) {
        printf("TCPORD FAIL echo truncated: %u of %d bytes\n", got_total,
               TOTAL_BYTES);
        return 1;
    }
    printf("TCPORD PASS: %d bytes echoed verbatim — stream ordering intact "
           "end to end\n", TOTAL_BYTES);
    return 0;
}

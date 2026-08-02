#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "sys/socket.h"
#include "netinet/in.h"

static int dns_encode_name(const char *name, unsigned char *out) {
    int pos = 0;
    const char *seg = name;
    while (*seg) {
        const char *dot = seg;
        while (*dot && *dot != '.') dot++;
        int len = (int)(dot - seg);
        if (len <= 0 || len > 63) return -1;
        out[pos++] = (unsigned char)len;
        for (int i = 0; i < len; i++) out[pos++] = (unsigned char)seg[i];
        seg = dot;
        if (*seg == '.') seg++;
    }
    out[pos++] = 0;
    return pos;
}

static unsigned short rd16(const unsigned char *p) {
    return (unsigned short)(((unsigned short)p[0] << 8) | p[1]);
}

int main(void) {
    puts("UDPTEST: starting SOCK_DGRAM DNS probe");

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        puts("UDPTEST FAIL: socket(AF_INET, SOCK_DGRAM) failed");
        return 1;
    }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(53000);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&local, sizeof(local)) < 0) {
        puts("UDPTEST FAIL: bind() failed");
        closesocket(s);
        return 1;
    }

    unsigned char query[128];
    memset(query, 0, sizeof(query));
    query[0] = 0x12; query[1] = 0x34;       /* ID */
    query[2] = 0x01; query[3] = 0x00;       /* recursion desired */
    query[4] = 0x00; query[5] = 0x01;       /* one question */
    int qname_len = dns_encode_name("example.com", query + 12);
    if (qname_len < 0) {
        puts("UDPTEST FAIL: DNS name encode failed");
        closesocket(s);
        return 1;
    }
    int qoff = 12 + qname_len;
    query[qoff + 0] = 0; query[qoff + 1] = 1;  /* A */
    query[qoff + 2] = 0; query[qoff + 3] = 1;  /* IN */
    int qlen = qoff + 4;

    struct sockaddr_in dns;
    memset(&dns, 0, sizeof(dns));
    dns.sin_family = AF_INET;
    dns.sin_port = htons(53);
    dns.sin_addr.s_addr = htonl((10u << 24) | (0u << 16) | (2u << 8) | 3u);

    ssize_t sent = sendto(s, query, (size_t)qlen, 0,
                          (struct sockaddr *)&dns, sizeof(dns));
    if (sent != qlen) {
        printf("UDPTEST FAIL: sendto returned %d\n", (int)sent);
        closesocket(s);
        return 1;
    }
    puts("UDPTEST: DNS query sent via sendto");

    unsigned char resp[512];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom(s, resp, sizeof(resp), 0,
                         (struct sockaddr *)&from, &from_len);
    if (n < 24) {
        printf("UDPTEST FAIL: recvfrom returned %d\n", (int)n);
        closesocket(s);
        return 1;
    }
    if (from.sin_family != AF_INET || ntohs(from.sin_port) != 53) {
        puts("UDPTEST FAIL: unexpected source address");
        closesocket(s);
        return 1;
    }
    if (resp[0] != 0x12 || resp[1] != 0x34) {
        puts("UDPTEST FAIL: DNS transaction ID mismatch");
        closesocket(s);
        return 1;
    }

    unsigned short an = rd16(resp + 6);
    if (an == 0) {
        puts("UDPTEST FAIL: DNS response has no answers");
        closesocket(s);
        return 1;
    }

    int off = qlen;
    int found_a = 0;
    for (unsigned short i = 0; i < an && off + 12 <= n; i++) {
        if ((resp[off] & 0xC0) == 0xC0) {
            off += 2;
        } else {
            while (off < n && resp[off] != 0) off += resp[off] + 1;
            off++;
        }
        if (off + 10 > n) break;
        unsigned short typ = rd16(resp + off);
        unsigned short rdlen = rd16(resp + off + 8);
        off += 10;
        if (typ == 1 && rdlen == 4 && off + 4 <= n) {
            printf("UDPTEST PASS: example.com A %u.%u.%u.%u from 10.0.2.3:53\n",
                   resp[off], resp[off + 1], resp[off + 2], resp[off + 3]);
            found_a = 1;
            break;
        }
        off += rdlen;
    }

    closesocket(s);
    if (!found_a) {
        puts("UDPTEST FAIL: no A record in DNS response");
        return 1;
    }
    return 0;
}

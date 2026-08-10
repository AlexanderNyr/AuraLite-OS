/* ipv6_addr.c — pure IPv6 address helpers.  See ipv6_addr.h. */

#include "kernel/net/ipv6_addr.h"

#ifdef AURALITE_IPV6_HOST_TEST
#include <string.h>
#else
#include "kernel/lib/string.h"
#endif

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ipv6_pton(const char *s, ipv6_addr_t *out) {
    uint16_t g[8];
    int ng = 0, dc = -1;
    if (!s || !out) return -1;

    const char *p = s;
    int val = -1;        /* -1 => current group not started */
    int trailing_sep = 0;
    while (1) {
        char c = *p;
        if (c == ':' || c == '\0') {
            if (val >= 0) {
                if (ng >= 8) return -1;
                g[ng++] = (uint16_t)val;
                val = -1;
                trailing_sep = 0;
            } else if (c == ':') {
                /* empty group: only legal as part of "::" */
                if (*(p + 1) == ':') {
                    if (dc >= 0) return -1;   /* only one :: allowed */
                    dc = ng;
                    p += 2;
                    continue;
                }
                return -1;
            }
            if (c == '\0') {
                if (trailing_sep) return -1;  /* trailing lone ':' */
                break;
            }
            if (*(p + 1) == ':') {           /* this colon starts "::" */
                if (dc >= 0) return -1;
                dc = ng;
                p += 2;
                continue;
            }
            p++;                              /* ordinary separator */
            trailing_sep = 1;
            continue;
        }
        int v = hexval(c);
        if (v < 0) return -1;
        val = (val < 0) ? v : (val << 4) | v;
        if (val > 0xFFFF) return -1;
        p++;
    }

    /* Assemble.  With "::" the dc position marks where the zero fill goes. */
    memset(out->b, 0, IPV6_ADDR_LEN);
    if (dc >= 0) {
        int fill = 8 - ng;
        if (fill < 1) return -1;             /* :: must encode >=1 zero group */
        int k = 0;
        for (int i = 0; i < dc; i++) { out->b[k++] = g[i] >> 8; out->b[k++] = g[i] & 0xFF; }
        k += fill * 2;                        /* zeros already in out->b */
        for (int i = dc; i < ng; i++) { out->b[k++] = g[i] >> 8; out->b[k++] = g[i] & 0xFF; }
        return 0;
    }
    if (ng != 8) return -1;
    for (int i = 0; i < 8; i++) { out->b[i * 2] = g[i] >> 8; out->b[i * 2 + 1] = g[i] & 0xFF; }
    return 0;
}

int ipv6_ntop(const ipv6_addr_t *a, char *buf, uint32_t buflen) {
    if (!a || !buf || buflen == 0) return -1;
    uint16_t g[8];
    for (int i = 0; i < 8; i++) g[i] = (uint16_t)((a->b[i * 2] << 8) | a->b[i * 2 + 1]);

    /* RFC 5952: compress the longest run of >=2 zero groups; leftmost wins. */
    int best_start = -1, best_len = 0;
    for (int i = 0; i < 8; i++) {
        if (g[i] == 0) {
            int j = i;
            while (j < 8 && g[j] == 0) j++;
            int len = j - i;
            if (len >= 2 && len > best_len) { best_len = len; best_start = i; }
            i = j - 1;
        }
    }

    uint32_t pos = 0;
    int compressed = 0;   /* previous group was the compressed "::" region */
    for (int i = 0; i < 8; i++) {
        if (best_len > 0 && i == best_start) {
            /* Exactly two colons: the first is the separator that would have
             * preceded this group (or the start marker when i==0), the second
             * marks the compression. */
            if (pos + 1 >= buflen) return -1;
            buf[pos++] = ':';
            if (pos + 1 >= buflen) return -1;
            buf[pos++] = ':';
            i += best_len - 1;
            compressed = 1;
            continue;
        }
        if (i > 0 && !compressed) {
            if (pos + 1 >= buflen) return -1;
            buf[pos++] = ':';
        }
        compressed = 0;
        /* minimal hex, no leading zeros */
        uint16_t v = g[i];
        char tmp[5];
        int n = 0;
        do { tmp[n++] = "0123456789abcdef"[v & 0xF]; v >>= 4; } while (v);
        while (n > 0) {
            if (pos + 1 >= buflen) return -1;
            buf[pos++] = tmp[--n];
        }
    }
    if (pos + 1 >= buflen) return -1;
    buf[pos] = '\0';
    return (int)pos;
}

void ipv6_linklocal_from_mac(const uint8_t mac[6], ipv6_addr_t *out) {
    uint8_t id[8];
    id[0] = mac[0] ^ 0x02;   /* flip the U/L (locally-administered) bit */
    id[1] = mac[1];
    id[2] = mac[2];
    id[3] = 0xFF;
    id[4] = 0xFE;
    id[5] = mac[3];
    id[6] = mac[4];
    id[7] = mac[5];

    memset(out->b, 0, IPV6_ADDR_LEN);
    out->b[0] = 0xFE;        /* fe80::/10 */
    out->b[1] = 0x80;
    memcpy(out->b + 8, id, 8);
}

int ipv6_is_unspecified(const ipv6_addr_t *a) {
    for (int i = 0; i < IPV6_ADDR_LEN; i++) if (a->b[i] != 0) return 0;
    return 1;
}

int ipv6_is_loopback(const ipv6_addr_t *a) {
    for (int i = 0; i < IPV6_ADDR_LEN; i++) {
        if (i == 15) { if (a->b[i] != 1) return 0; }
        else         { if (a->b[i] != 0) return 0; }
    }
    return 1;
}

int ipv6_is_linklocal(const ipv6_addr_t *a) {
    /* fe80::/10 => first byte 0xFE and second byte has top 6 bits 10xxxxxx
     * i.e. second byte & 0xC0 == 0x80. */
    return a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0x80;
}

int ipv6_eq(const ipv6_addr_t *a, const ipv6_addr_t *b) {
    return memcmp(a->b, b->b, IPV6_ADDR_LEN);
}

static uint16_t ones_complement_sum(const void *data, uint32_t len) {
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint16_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)sum;
}

uint16_t ipv6_checksum_pseudo(const ipv6_addr_t *src, const ipv6_addr_t *dst,
                              uint32_t len, uint8_t next_header,
                              const void *msg, uint32_t msg_len) {
    /* RFC 8200 s8.1 pseudo-header: src(16) dst(16) len(4) zeros(3) next(1). */
    uint8_t ph[40];
    memcpy(ph, src->b, 16);
    memcpy(ph + 16, dst->b, 16);
    ph[32] = (uint8_t)(len >> 24);
    ph[33] = (uint8_t)(len >> 16);
    ph[34] = (uint8_t)(len >> 8);
    ph[35] = (uint8_t)len;
    ph[36] = 0;
    ph[37] = 0;
    ph[38] = 0;
    ph[39] = next_header;

    uint32_t sum = ones_complement_sum(ph, sizeof(ph));
    sum += ones_complement_sum(msg, msg_len);

    /* fold */
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

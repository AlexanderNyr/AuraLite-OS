/*
 * test_ipv6_addr.c — host unit tests for REALINTERNET_PLAN phase X7 (IPv6).
 * Drives the pure helpers in kernel/net/ipv6_addr.c: address parse/format
 * (RFC 5952), EUI-64 link-local derivation, and the ICMPv6 pseudo-header
 * checksum.  The checksum is cross-checked against an independent, explicit
 * byte-buffer reference sum.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define AURALITE_IPV6_HOST_TEST 1
#include "kernel/net/ipv6_addr.c"   /* test the implementation directly */

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long _a = (long)(a), _e = (long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=%ld want %ld\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

/* Reference: explicit pseudo-header bytes + message, plain one's complement. */
static uint16_t ref_checksum(const ipv6_addr_t *src, const ipv6_addr_t *dst,
                             uint32_t len, uint8_t nh, const void *msg, uint32_t msglen) {
    uint8_t ph[40];
    memcpy(ph, src->b, 16);
    memcpy(ph + 16, dst->b, 16);
    ph[32] = (uint8_t)(len >> 24); ph[33] = (uint8_t)(len >> 16);
    ph[34] = (uint8_t)(len >> 8);  ph[35] = (uint8_t)len;
    ph[36] = 0; ph[37] = 0; ph[38] = 0; ph[39] = nh;

    uint32_t sum = 0;
    for (int i = 0; i < 40; i += 2) sum += (uint16_t)((ph[i] << 8) | ph[i + 1]);
    uint32_t m = msglen;
    const uint8_t *p = msg;
    while (m > 1) { sum += (uint16_t)((p[0] << 8) | p[1]); p += 2; m -= 2; }
    if (m) sum += (uint16_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void t_pton_ntop(void) {
    struct { const char *in; const char *want; } ok[] = {
        { "2001:db8::1",          "2001:db8::1" },
        { "::1",                  "::1" },
        { "::",                   "::" },
        { "fe80::5054:ff:fe12:3456", "fe80::5054:ff:fe12:3456" },
        { "2001:0db8:0000:0000:0000:0000:0000:0001", "2001:db8::1" },
        { "1::",                  "1::" },
        { "1:2:3:4:5:6:7:8",      "1:2:3:4:5:6:7:8" },
        { "::2",                  "::2" },
        { "fe80::1",              "fe80::1" },
        { "2001:db8:0:0:0:0:2:1", "2001:db8::2:1" },
    };
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        ipv6_addr_t a;
        char s[48];
        CHECK_EQ(ipv6_pton(ok[i].in, &a), 0);
        int n = ipv6_ntop(&a, s, sizeof(s));
        CHECK(n > 0);
        if (strcmp(s, ok[i].want) != 0)
            printf("  FAIL L%d: %s ntop-> '%s' want '%s'\n", __LINE__, ok[i].in, s, ok[i].want);
    }
}

static void t_pton_invalid(void) {
    const char *bad[] = {
        "", ":", ":::", "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9",
        "1::2::3", ":1:2:3:4:5:6:7", "gg::", "1:2:3:4:5:6:7:8:",
        "12345::1", "1:2:3:4:5:6:7:8:9:10", "1:2:3:4:5:6:7::8:9",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ipv6_addr_t a;
        if (ipv6_pton(bad[i], &a) == 0)
            printf("  FAIL L%d: accepted '%s'\n", __LINE__, bad[i]);
    }
}

static void t_eui64(void) {
    /* QEMU-style MAC 52:54:00:12:34:56 -> fe80::5054:ff:fe12:3456 */
    uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    ipv6_addr_t ll;
    ipv6_linklocal_from_mac(mac, &ll);
    const uint8_t expect[16] = {
        0xfe, 0x80, 0, 0, 0, 0, 0, 0,
        0x50, 0x54, 0x00, 0xff, 0xfe, 0x12, 0x34, 0x56
    };
    CHECK_EQ(memcmp(ll.b, expect, 16), 0);
    CHECK(ipv6_is_linklocal(&ll));
    CHECK_EQ(ipv6_is_loopback(&ll), 0);
    CHECK_EQ(ipv6_is_unspecified(&ll), 0);

    /* another MAC */
    uint8_t mac2[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    ipv6_addr_t ll2;
    ipv6_linklocal_from_mac(mac2, &ll2);
    /* U/L flip: 0x02 ^ 0x02 = 0x00 */
    CHECK_EQ(ll2.b[8], 0x00);
    CHECK_EQ(ll2.b[9], 0x00);
    CHECK_EQ(ll2.b[10], 0x00);
    CHECK_EQ(ll2.b[11], 0xff);
    CHECK_EQ(ll2.b[12], 0xfe);
    CHECK_EQ(ll2.b[15], 0x01);
}

static void t_checksum(void) {
    ipv6_addr_t src, dst;
    ipv6_pton("fe80::5054:ff:fe12:3456", &src);
    ipv6_pton("fe80::2", &dst);
    uint8_t payload[8] = { 0x80, 0, 0, 0, 0x12, 0x34, 0x56, 0x78 };
    uint16_t c = ipv6_checksum_pseudo(&src, &dst, 8, 58, payload, 8);
    uint16_t r = ref_checksum(&src, &dst, 8, 58, payload, 8);
    CHECK_EQ(c, r);
    CHECK(c != 0);

    /* deterministic */
    CHECK_EQ(ipv6_checksum_pseudo(&src, &dst, 8, 58, payload, 8), c);

    /* sensitive to a data byte */
    uint8_t payload2[8]; memcpy(payload2, payload, 8); payload2[0] ^= 0xFF;
    uint16_t c2 = ipv6_checksum_pseudo(&src, &dst, 8, 58, payload2, 8);
    CHECK(c2 != c);

    /* different length changes the checksum */
    uint16_t c3 = ipv6_checksum_pseudo(&src, &dst, 16, 58, payload, 8);
    CHECK(c3 != c);

    /* different next-header (UDP=17) changes the checksum */
    uint16_t c4 = ipv6_checksum_pseudo(&src, &dst, 8, 17, payload, 8);
    CHECK(c4 != c);

    /* changing a source-address byte changes it (the one's-complement sum is
     * order-commutative, so swapping whole src/dst does NOT — assert on a
     * real address change instead). */
    ipv6_addr_t src2 = src;
    src2.b[15] ^= 0x01;
    uint16_t c5 = ipv6_checksum_pseudo(&src2, &dst, 8, 58, payload, 8);
    CHECK(c5 != c);
}

int main(void) {
    RUN(t_pton_ntop);
    RUN(t_pton_invalid);
    RUN(t_eui64);
    RUN(t_checksum);
    printf("ipv6_addr: %d subtests, %d passed, %d failed\n", tn, passed, failed);
    return failed ? 1 : 0;
}

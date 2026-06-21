/*
 * test_net.c — unit tests for network utility functions.
 *
 * Tests: internet checksum (RFC 1071), byte-swap helpers, DNS name encoding,
 * DHCP option parsing, IP address manipulation.
 *
 * 30+ test cases. Compiled standalone (no kernel deps).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;
static int test_num = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    int before = failed; name(); test_num++; \
    if (failed == before) passed++; \
} while(0)

#define CHECK(cond) do { \
    if (!(cond)) { printf("  FAIL: %d: %s\n", __LINE__, #cond); failed++; } \
} while(0)
#define CHECK_EQ(actual, expected) do { \
    if ((long long)(actual) != (long long)(expected)) { \
        printf("  FAIL: %d: %s=0x%llx expected 0x%llx\n", \
               __LINE__, #actual, (unsigned long long)(actual), \
               (unsigned long long)(expected)); failed++; \
    } \
} while(0)

/* ---- Byte-swap helpers ---- */
static uint16_t swap16(uint16_t v) { return (v >> 8) | (v << 8); }
static uint32_t swap32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}

/* ---- Internet checksum (RFC 1071) ---- */
static uint16_t net_checksum(const void *data, uint32_t len) {
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len > 1) { sum += (uint16_t)(p[0] << 8 | p[1]); p += 2; len -= 2; }
    if (len) sum += p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return swap16((uint16_t)(~sum & 0xFFFF));
}

/* ---- DNS name encoder ---- */
static int dns_encode(const char *name, uint8_t *out) {
    int pos = 0;
    const char *seg = name;
    while (*seg) {
        const char *dot = seg;
        while (*dot && *dot != '.') dot++;
        int len = dot - seg;
        if (len > 63 || len == 0) return -1;
        out[pos++] = (uint8_t)len;
        memcpy(out + pos, seg, len);
        pos += len;
        seg = dot;
        if (*seg == '.') seg++;
    }
    out[pos++] = 0;
    return pos;
}

/* ---- DHCP option finder ---- */
static const uint8_t *dhcp_find(const uint8_t *opts, int len, uint8_t code, int *olen) {
    int i = 0;
    while (i < len) {
        uint8_t c = opts[i];
        if (c == 255) break;
        if (c == 0) { i++; continue; }
        if (i + 1 >= len) break;
        uint8_t l = opts[i + 1];
        if (c == code) { if (olen) *olen = l; return opts + i + 2; }
        i += 2 + l;
    }
    return NULL;
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* ---- IP helpers ---- */
static uint32_t ip_make(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) |
           ((uint32_t)c << 8) | d;
}
static int ip_same_subnet(uint32_t a, uint32_t b, uint32_t mask) {
    return (a & mask) == (b & mask);
}

/* ====== TESTS ====== */

/* ---- Byte-swap ---- */
TEST(test_swap16_basic) {
    CHECK_EQ(swap16(0x1234), 0x3412);
}
TEST(test_swap16_zero) {
    CHECK_EQ(swap16(0), 0);
}
TEST(test_swap16_ff) {
    CHECK_EQ(swap16(0xFFFF), 0xFFFF);
}
TEST(test_swap32_basic) {
    CHECK_EQ(swap32(0x12345678), 0x78563412);
}
TEST(test_swap32_zero) {
    CHECK_EQ(swap32(0), 0);
}
TEST(test_swap32_roundtrip) {
    CHECK_EQ(swap32(swap32(0xDEADBEEF)), 0xDEADBEEF);
}

/* ---- Checksum ---- */
TEST(test_checksum_known) {
    uint8_t data[] = {0x00,0x01,0xF2,0x03,0xF4,0xF5,0xF6,0xF7};
    uint16_t cs = net_checksum(data, 8);
    CHECK_EQ(cs, 0x0D22);  /* RFC 1071 computed value */
}
TEST(test_checksum_empty) {
    CHECK_EQ(net_checksum("", 0), 0xFFFF);
}
TEST(test_checksum_odd_len) {
    uint8_t data[] = {0x12, 0x34, 0x56};
    uint16_t cs = net_checksum(data, 3);
    /* (0x1234 + 0x5600) = 0x6834 -> ~ = 0x97CB -> swapped = 0xCB97 */
    CHECK_EQ(cs, 0xCB97);
}
TEST(test_checksum_even_len) {
    uint8_t data[] = {0x12, 0x34};
    uint16_t cs = net_checksum(data, 2);
    /* 0x1234 -> ~ = 0xEDCB -> swapped = 0xCBED */
    CHECK_EQ(cs, 0xCBED);
}
TEST(test_checksum_zeros) {
    uint8_t data[20] = {0};
    uint16_t cs = net_checksum(data, 20);
    CHECK_EQ(cs, 0xFFFF);
}
TEST(test_checksum_all_ff) {
    uint8_t data[8];
    memset(data, 0xFF, 8);
    uint16_t cs = net_checksum(data, 8);
    CHECK_EQ(cs, 0x0000);
}
TEST(test_checksum_with_carry) {
    /* Large values that cause carry folding. */
    uint8_t data[] = {0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF, 0xFF,0xFF};
    uint16_t cs = net_checksum(data, 8);
    CHECK_EQ(cs, 0x0000);
}
TEST(test_checksum_large) {
    static uint8_t data[2048];
    memset(data, 0xAA, 2048);
    uint16_t cs = net_checksum(data, 2048);
    /* Each 0xAAAA pair contributes 0xAAAA. 1024 pairs = 0xAAAA*1024.
     * The final checksum should be deterministic. */
    CHECK(cs != 0); /* just verify it runs */
}

/* ---- DNS encoding ---- */
TEST(test_dns_simple) {
    uint8_t out[64];
    int len = dns_encode("hello", out);
    CHECK_EQ(len, 7); /* 1 + "hello" + 1(null) */
    CHECK_EQ(out[0], 5);
    CHECK_EQ(memcmp(out+1, "hello", 5), 0);
    CHECK_EQ(out[6], 0);
}
TEST(test_dns_dotted) {
    uint8_t out[64];
    int len = dns_encode("a.b", out);
    CHECK_EQ(len, 5); /* 1+"a" + 1+"b" + 1(null) = 5 */
    CHECK_EQ(out[0], 1);
    CHECK_EQ(out[1], 'a');
    CHECK_EQ(out[2], 1);
    CHECK_EQ(out[3], 'b');
    CHECK_EQ(out[4], 0);
}
TEST(test_dns_multi) {
    uint8_t out[64];
    int len = dns_encode("example.com", out);
    CHECK_EQ(len, 13); /* 1+7 + 1+3 + 1(null) = 13 */
    CHECK_EQ(out[0], 7);
    CHECK_EQ(memcmp(out+1, "example", 7), 0);
    CHECK_EQ(out[8], 3);
    CHECK_EQ(memcmp(out+9, "com", 3), 0);
    CHECK_EQ(out[12], 0);
}
TEST(test_dns_empty_label) {
    uint8_t out[64];
    int len = dns_encode("a..b", out);
    CHECK_EQ(len, -1); /* empty label is invalid */
}
TEST(test_dns_long_label) {
    uint8_t out[128];
    char name[70];
    memset(name, 'x', 64);
    name[64] = 0;
    int len = dns_encode(name, out);
    CHECK_EQ(len, -1); /* label > 63 chars is invalid */
}
TEST(test_dns_root) {
    uint8_t out[8];
    int len = dns_encode("", out);
    CHECK_EQ(len, 1);
    CHECK_EQ(out[0], 0);
}

/* ---- DHCP option parsing ---- */
TEST(test_dhcp_find_type) {
    uint8_t opts[] = {53, 1, 2, 255};
    int olen;
    const uint8_t *v = dhcp_find(opts, 4, 53, &olen);
    CHECK(v != NULL);
    CHECK_EQ(olen, 1);
    CHECK_EQ(*v, 2);
}
TEST(test_dhcp_find_missing) {
    uint8_t opts[] = {53, 1, 2, 255};
    CHECK(dhcp_find(opts, 4, 99, NULL) == NULL);
}
TEST(test_dhcp_find_after_other) {
    uint8_t opts[] = {1, 4, 0xFF,0xFF,0xFF,0x00, 3, 4, 10,0,2,2, 255};
    int olen;
    const uint8_t *v = dhcp_find(opts, 13, 3, &olen);
    CHECK(v != NULL);
    CHECK_EQ(olen, 4);
    CHECK_EQ(v[0], 10);
    CHECK_EQ(v[3], 2);
}
TEST(test_dhcp_find_with_padding) {
    uint8_t opts[] = {0, 0, 53, 1, 5, 255};
    int olen;
    const uint8_t *v = dhcp_find(opts, 6, 53, &olen);
    CHECK(v != NULL);
    CHECK_EQ(*v, 5);
}
TEST(test_dhcp_find_multi_value) {
    uint8_t opts[] = {6, 4, 10,0,2,3, 255};
    int olen;
    const uint8_t *v = dhcp_find(opts, 7, 6, &olen);
    CHECK_EQ(olen, 4);
    CHECK_EQ(be32(v), ip_make(10,0,2,3));
}
TEST(test_dhcp_find_end_only) {
    uint8_t opts[] = {255};
    CHECK(dhcp_find(opts, 1, 53, NULL) == NULL);
}

/* ---- IP helpers ---- */
TEST(test_ip_make) {
    CHECK_EQ(ip_make(10,0,2,15), 0x0A00020F);
    CHECK_EQ(ip_make(255,255,255,0), 0xFFFFFF00);
    CHECK_EQ(ip_make(0,0,0,0), 0);
}
TEST(test_ip_same_subnet_match) {
    uint32_t a = ip_make(10,0,2,15);
    uint32_t b = ip_make(10,0,2,99);
    uint32_t mask = ip_make(255,255,255,0);
    CHECK(ip_same_subnet(a, b, mask));
}
TEST(test_ip_same_subnet_mismatch) {
    uint32_t a = ip_make(10,0,2,15);
    uint32_t b = ip_make(10,0,3,15);
    uint32_t mask = ip_make(255,255,255,0);
    CHECK(!ip_same_subnet(a, b, mask));
}
TEST(test_ip_is_broadcast) {
    uint32_t bcast = ip_make(255,255,255,255);
    CHECK_EQ(bcast, 0xFFFFFFFF);
}
TEST(test_be32_roundtrip) {
    uint8_t bytes[4] = {10, 0, 2, 15};
    uint32_t ip = be32(bytes);
    CHECK_EQ(ip, 0x0A00020F);
}

/* ---- TCP/UDP port manipulation ---- */
TEST(test_port_swap) {
    uint16_t port = 80;
    uint16_t network_order = swap16(port);
    CHECK_EQ(network_order, 0x5000);
    CHECK_EQ(swap16(network_order), 80);
}
TEST(test_port_well_known) {
    CHECK_EQ(swap16(53), 0x3500);
    CHECK_EQ(swap16(67), 0x4300);
    CHECK_EQ(swap16(80), 0x5000);
    CHECK_EQ(swap16(443), 0xBB01);
}

/* ---- Ethernet frame validation ---- */
TEST(test_ethertype) {
    CHECK_EQ(swap16(0x0800), 0x0008); /* IPv4 in wire order */
    CHECK_EQ(swap16(0x0806), 0x0608); /* ARP */
}
TEST(test_broadcast_mac) {
    uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    int all_ff = 1;
    for (int i = 0; i < 6; i++) if (bcast[i] != 0xFF) all_ff = 0;
    CHECK(all_ff);
}

int main(void) {
    printf("=== Network Utility Tests ===\n\n");

    printf("--- byte-swap ---\n");
    RUN(test_swap16_basic); RUN(test_swap16_zero); RUN(test_swap16_ff);
    RUN(test_swap32_basic); RUN(test_swap32_zero); RUN(test_swap32_roundtrip);

    printf("--- checksum ---\n");
    RUN(test_checksum_known); RUN(test_checksum_empty); RUN(test_checksum_odd_len);
    RUN(test_checksum_even_len); RUN(test_checksum_zeros); RUN(test_checksum_all_ff);
    RUN(test_checksum_with_carry); RUN(test_checksum_large);

    printf("--- DNS encoding ---\n");
    RUN(test_dns_simple); RUN(test_dns_dotted); RUN(test_dns_multi);
    RUN(test_dns_empty_label); RUN(test_dns_long_label); RUN(test_dns_root);

    printf("--- DHCP options ---\n");
    RUN(test_dhcp_find_type); RUN(test_dhcp_find_missing);
    RUN(test_dhcp_find_after_other); RUN(test_dhcp_find_with_padding);
    RUN(test_dhcp_find_multi_value); RUN(test_dhcp_find_end_only);

    printf("--- IP helpers ---\n");
    RUN(test_ip_make); RUN(test_ip_same_subnet_match);
    RUN(test_ip_same_subnet_mismatch); RUN(test_ip_is_broadcast);
    RUN(test_be32_roundtrip);

    printf("--- ports/ethernet ---\n");
    RUN(test_port_swap); RUN(test_port_well_known);
    RUN(test_ethertype); RUN(test_broadcast_mac);

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           passed, test_num, failed);
    return failed ? 1 : 0;
}

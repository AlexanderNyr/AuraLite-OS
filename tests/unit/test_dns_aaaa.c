/* test_dns_aaaa.c — Y3 host gate: type-AAAA parse. */
#define AURALITE_DNS_HOST_TEST 1
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../kernel/net/dns_parse.h"
#include "../../kernel/net/dns_parse.c"

static int passed, failed;
#define CHECK(c, ...) do { \
    if (c) passed++; \
    else { failed++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* A minimal AAAA answer for qname "x.test", id 0x1111,
 * rdata fec0::2. */
static void build_aaaa_answer(uint8_t *m, int *len) {
    uint8_t qname[] = { 1, 'x', 4, 't', 'e', 's', 't', 0 };
    int p = 0, i;
    memset(m, 0, 128);
    m[0] = 0x11; m[1] = 0x11;
    m[2] = 0x81; m[3] = 0x80;           /* QR|RD|RA */
    m[4] = 0; m[5] = 1;                 /* QD=1 */
    m[6] = 0; m[7] = 1;                 /* AN=1 */
    p = 12;
    memcpy(m + p, qname, sizeof qname); p += (int)sizeof qname;
    m[p++] = 0; m[p++] = 28;            /* QTYPE AAAA */
    m[p++] = 0; m[p++] = 1;             /* IN */
    memcpy(m + p, qname, sizeof qname); p += (int)sizeof qname;
    m[p++] = 0; m[p++] = 28;            /* TYPE AAAA */
    m[p++] = 0; m[p++] = 1;
    m[p++] = 0; m[p++] = 0; m[p++] = 0; m[p++] = 60; /* TTL 60 */
    m[p++] = 0; m[p++] = 16;            /* rdlen */
    for (i = 0; i < 16; i++) m[p++] = 0;
    m[p - 16] = 0xfe; m[p - 15] = 0xc0; m[p - 1] = 2;
    *len = p;
}

int main(void) {
    uint8_t msg[128], addr[16];
    uint32_t ttl = 0;
    int len = 0;
    int pr;

    build_aaaa_answer(msg, &len);
    pr = dns_parse_aaaa(msg, len, "x.test", 0x1111, addr, &ttl);
    CHECK(pr == DNS_PARSE_ANSWER, "AAAA parse returns ANSWER (got %d)", pr);
    CHECK(ttl == 60, "TTL is 60");
    CHECK(addr[0] == 0xfe && addr[1] == 0xc0 && addr[15] == 2,
          "rdata is fec0::2");

    pr = dns_parse_aaaa(msg, len, "x.test", 0x2222, addr, &ttl);
    CHECK(pr == DNS_PARSE_BAD, "wrong ID is BAD");

    msg[2] = 0x83; msg[3] = 0x80;      /* TC */
    pr = dns_parse_aaaa(msg, len, "x.test", 0x1111, addr, &ttl);
    CHECK(pr == DNS_PARSE_TRUNCATED, "TC is TRUNCATED");

    CHECK(DNS_RTYPE_AAAA == 28, "AAAA type is 28");
    printf("test_dns_aaaa: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

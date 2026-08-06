/* x509test — in-guest X.509 parsing gate (INTERNET_PLAN.md phase N2).
 *
 * The full RFC/field battery runs host-side (tests/unit/test_atls_x509.c).
 * THIS program exists because the N2 test gate demands the hostile cases
 * be run "in QEMU where the 64 KB limit is real": the parser faces the
 * guest's actual 64 KiB user stack and the kernel's guard pages, so any
 * recursion over attacker-supplied nesting shows up as a SIGSEGV on the
 * guard page instead of silently passing on the host's 8 MiB stack.
 *
 * Covered here: real + local certificates parse, the truncation sweep,
 * the bit-flip battery, indefinite/huge lengths, and the 10 000-deep
 * nesting refused at the depth budget — built by the SAME helpers the
 * host battery uses (tests/unit/atls_x509_testdata.c), so both halves
 * of the gate exercise identical bytes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "atls/atls.h"
#include "atls/x509.h"
#include "tests/atls_test_certs.h"

extern size_t atls_test_build_deep_nest(uint8_t *buf, size_t bufcap, int levels);
extern size_t build_cert_with_san_blob(uint8_t *out, const uint8_t *blob,
                                       size_t blob_len);
extern int atls_der_skip_value_test(const uint8_t *p, size_t n);

static int failures = 0;

#define CHECK(cond, name)                                    \
    do {                                                     \
        if (cond) {                                          \
            printf("[x509test] PASS: %s\n", name);           \
        } else {                                             \
            failures++;                                      \
            printf("[x509test] FAIL: %s\n", name);           \
        }                                                    \
    } while (0)

static int span_streq(const atls_span *s, const char *lit) {
    size_t n = strlen(lit);
    return s->len == n && memcmp(s->data, lit, n) == 0;
}

static void test_embedded_certs(void) {
    atls_x509_cert c;

    CHECK(atls_x509_parse(ATLS_TEST_CERT_EXAMPLE_COM,
                          sizeof(ATLS_TEST_CERT_EXAMPLE_COM), &c) == ATLS_OK,
          "example.com leaf parses in guest");
    CHECK(c.version == 3 && c.san_dns_count == 2 &&
          span_streq(&c.san_dns[0], "example.com") &&
          span_streq(&c.san_dns[1], "*.example.com"),
          "example.com: version + SANs correct");
    CHECK(atls_oid_eq(&c.outer_sig_oid, ATLS_OID_ECDSA_SHA256, 8),
          "example.com: signature OID correct");

    CHECK(atls_x509_parse(ATLS_TEST_CERT_GOOGLE,
                          sizeof(ATLS_TEST_CERT_GOOGLE), &c) == ATLS_OK &&
          c.san_dns_count == 1 && span_streq(&c.san_dns[0], "www.google.com"),
          "www.google.com leaf parses in guest");

    CHECK(atls_x509_parse(ATLS_TEST_CERT_EC_LOCAL,
                          sizeof(ATLS_TEST_CERT_EC_LOCAL), &c) == ATLS_OK &&
          c.is_ca == 1 && c.san_dns_count == 2 &&
          span_streq(&c.san_dns[0], "localhost"),
          "local EC: CA flag + SANs correct");

    CHECK(atls_x509_parse(ATLS_TEST_CERT_ED_LOCAL,
                          sizeof(ATLS_TEST_CERT_ED_LOCAL), &c) == ATLS_OK &&
          atls_oid_eq(&c.outer_sig_oid, ATLS_OID_ED25519, 3),
          "local Ed25519 parses in guest");

    CHECK(atls_x509_parse(ATLS_TEST_CERT_CA_LOCAL,
                          sizeof(ATLS_TEST_CERT_CA_LOCAL), &c) == ATLS_OK &&
          c.is_ca == 1 && c.path_len_present && c.path_len == 0 &&
          c.key_usage == 0x0060,
          "local RSA CA: pathlen + keyUsage correct");

    CHECK(atls_x509_parse(ATLS_TEST_CERT_RSA_LEAF,
                          sizeof(ATLS_TEST_CERT_RSA_LEAF), &c) == ATLS_OK &&
          c.is_ca == 0 && c.key_usage == 0x0005,
          "local RSA leaf: CA:FALSE + keyUsage correct");
}

static void test_truncation_sweep(void) {
    atls_x509_cert c;
    size_t n = sizeof(ATLS_TEST_CERT_EC_LOCAL);
    int all_refused = 1;
    for (size_t cut = 0; cut < n; cut++) {
        if (atls_x509_parse(ATLS_TEST_CERT_EC_LOCAL, cut, &c) == ATLS_OK) {
            all_refused = 0;
            break;
        }
    }
    CHECK(all_refused, "every truncated prefix refused (418 prefixes)");
}

static void test_bit_flips(void) {
    atls_x509_cert c;
    static uint8_t buf[2048];
    size_t n = sizeof(ATLS_TEST_CERT_EXAMPLE_COM);
    int ok_count = 0, err_count = 0;

    for (size_t pos = 0; pos < n; pos += 13) {
        for (int bit = 0; bit < 8; bit += 3) {
            memcpy(buf, ATLS_TEST_CERT_EXAMPLE_COM, n);
            buf[pos] ^= (uint8_t)(1u << bit);
            int rc = atls_x509_parse(buf, n, &c);
            if (rc == ATLS_OK) ok_count++; else err_count++;
        }
    }
    CHECK(err_count > 0 && ok_count > 0,
          "bit-flip battery: parser discriminates, nothing crashed");
}

static void test_malformed(void) {
    atls_x509_cert c;
    static const uint8_t indef[] = { 0x30, 0x80, 0x00, 0x00 };
    CHECK(atls_x509_parse(indef, sizeof(indef), &c) == ATLS_ERR_BAD_LENGTH,
          "indefinite length refused");
    static const uint8_t huge[] = { 0x30, 0x84, 0xff, 0xff, 0xff, 0xff };
    CHECK(atls_x509_parse(huge, sizeof(huge), &c) == ATLS_ERR_TRUNCATED,
          "4 GiB length claim refused without allocating");
    CHECK(atls_x509_parse(ATLS_TEST_CERT_EC_LOCAL,
                          sizeof(ATLS_TEST_CERT_EC_LOCAL) + 0, &c) == ATLS_OK,
          "sane certificate still parses");
}

static void test_depth_gate(void) {
    atls_x509_cert c;
    uint8_t *blob = malloc(60000);
    static uint8_t crafted[65536];
    if (!blob) {
        CHECK(0, "deep-nest buffer allocation");
        return;
    }

    size_t n10000 = atls_test_build_deep_nest(blob, 60000, 10000);
    CHECK(n10000 > 0, "10 000-deep blob built in guest");

    size_t cn = build_cert_with_san_blob(crafted, blob, n10000);
    CHECK(atls_x509_parse(crafted, cn, &c) == ATLS_ERR_DEPTH,
          "10 000-deep nesting refused with ATLS_ERR_DEPTH (no stack death)");
    CHECK(atls_der_skip_value_test(blob, n10000) == ATLS_ERR_DEPTH,
          "skip_value enforces the depth budget in guest");

    size_t n20 = atls_test_build_deep_nest(blob, 60000, 20);
    cn = build_cert_with_san_blob(crafted, blob, n20);
    CHECK(atls_x509_parse(crafted, cn, &c) == ATLS_OK,
          "20-deep nesting accepted within budget");

    free(blob);
}

int main(void) {
    printf("[x509test] in-guest X.509 gate (INTERNET_PLAN N2, 64 KiB stack)\n");
    test_embedded_certs();
    test_truncation_sweep();
    test_bit_flips();
    test_malformed();
    test_depth_gate();
    if (failures == 0) {
        printf("[x509test] ALL PASS\n");
        return 0;
    }
    printf("[x509test] %d FAILURES\n", failures);
    return 1;
}

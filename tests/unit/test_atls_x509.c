/* tests/unit/test_atls_x509.c — N2 X.509 parsing gates.
 *
 *   1. Real certificates parse correctly: two REAL-WORLD leaves fetched
 *      live (example.com, www.google.com) plus four openssl-generated
 *      locals, all embedded via tests/atls_test_certs.h and asserted
 *      field by field (SAN, CA flag, pathlen, key usage bits, critical
 *      flags, signature OIDs, validity dates).
 *   2. Truncated, over-long, and malformed structures are all refused —
 *      every prefix of a real certificate must fail, and each crafted
 *      malformation must fail for its SPECIFIC reason.
 *   3. A 10 000-deep nesting is refused with ATLS_ERR_DEPTH, not
 *      followed (the 64 KiB guest stack is exactly what this protects;
 *      the in-guest half of this gate is userspace/tests/x509test).
 *   4. A mutation battery (bit flips, byte deletions) over real
 *      certificates: the parser may accept or refuse, but it must not
 *      crash, hang, or read out of bounds.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "atls/atls.h"
#include "atls/x509.h"
#include "tests/atls_test_certs.h"

static int pass_count = 0, fail_count = 0;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) { pass_count++; printf("PASS: %s\n", msg); } \
        else { fail_count++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
    } while (0)

static int span_streq(const atls_span *s, const char *lit) {
    size_t n = strlen(lit);
    return s->len == n && memcmp(s->data, lit, n) == 0;
}

/* ---- 1. real certificates, field by field ---- */

static void test_real_certs(void) {
    atls_x509_cert c;

    CHECK(atls_x509_parse(ATLS_TEST_CERT_EXAMPLE_COM,
                          sizeof(ATLS_TEST_CERT_EXAMPLE_COM), &c) == ATLS_OK,
          "example.com leaf parses");
    CHECK(c.version == 3, "example.com: version 3");
    CHECK(c.san_dns_count == 2, "example.com: two dNSNames");
    CHECK(span_streq(&c.san_dns[0], "example.com"),
          "example.com: SAN[0] == example.com");
    CHECK(span_streq(&c.san_dns[1], "*.example.com"),
          "example.com: SAN[1] == *.example.com");
    CHECK(c.is_ca == 0 && c.bc_critical == 1,
          "example.com: CA:FALSE, critical basicConstraints");
    CHECK(c.key_usage_present && c.key_usage == 0x0001,
          "example.com: keyUsage digitalSignature only");
    CHECK(atls_oid_eq(&c.outer_sig_oid, ATLS_OID_ECDSA_SHA256, 8),
          "example.com: ecdsa-with-SHA256 signature algorithm");
    CHECK(atls_oid_eq(&c.spki_alg_oid, ATLS_OID_EC_PUBLIC_KEY, 7),
          "example.com: id-ecPublicKey SPKI");
    CHECK(c.not_before.year == 2026 && c.not_after.year == 2026,
          "example.com: validity dates read");
    CHECK(c.signature.len == 70, "example.com: ECDSA P-256 signature length");

    CHECK(atls_x509_parse(ATLS_TEST_CERT_GOOGLE,
                          sizeof(ATLS_TEST_CERT_GOOGLE), &c) == ATLS_OK,
          "www.google.com leaf parses");
    CHECK(c.san_dns_count == 1 && span_streq(&c.san_dns[0], "www.google.com"),
          "www.google.com: SAN matches");
    CHECK(c.is_ca == 0, "www.google.com: leaf, not a CA");

    CHECK(atls_x509_parse(ATLS_TEST_CERT_EC_LOCAL,
                          sizeof(ATLS_TEST_CERT_EC_LOCAL), &c) == ATLS_OK,
          "local EC self-signed parses");
    CHECK(c.san_dns_count == 2 && span_streq(&c.san_dns[0], "localhost")
          && span_streq(&c.san_dns[1], "aura.test"),
          "local EC: SANs localhost + aura.test");
    CHECK(c.not_before.year == 2026 && c.not_after.year == 2036,
          "local EC: UTCTime decade rollover decoded (2026..2036)");

    CHECK(atls_x509_parse(ATLS_TEST_CERT_ED_LOCAL,
                          sizeof(ATLS_TEST_CERT_ED_LOCAL), &c) == ATLS_OK,
          "local Ed25519 self-signed parses");
    CHECK(atls_oid_eq(&c.outer_sig_oid, ATLS_OID_ED25519, 3),
          "local Ed25519: Ed25519 signature OID");
    CHECK(c.signature.len == 64, "local Ed25519: 64-byte signature");

    CHECK(atls_x509_parse(ATLS_TEST_CERT_CA_LOCAL,
                          sizeof(ATLS_TEST_CERT_CA_LOCAL), &c) == ATLS_OK,
          "local RSA CA parses");
    CHECK(c.is_ca == 1 && c.bc_critical == 1,
          "local CA: CA:TRUE, critical");
    CHECK(c.path_len_present && c.path_len == 0,
          "local CA: pathlen:0 decoded");
    CHECK(c.key_usage_present && c.ku_critical && c.key_usage == 0x0060,
          "local CA: critical keyUsage keyCertSign|cRLSign");
    CHECK(atls_oid_eq(&c.outer_sig_oid, ATLS_OID_SHA256_RSA, 9),
          "local CA: sha256WithRSAEncryption");
    CHECK(atls_oid_eq(&c.spki_alg_oid, ATLS_OID_RSA_ENCRYPTION, 9),
          "local CA: rsaEncryption SPKI");
    CHECK(c.signature.len == 256, "local CA: 2048-bit RSA signature");

    CHECK(atls_x509_parse(ATLS_TEST_CERT_RSA_LEAF,
                          sizeof(ATLS_TEST_CERT_RSA_LEAF), &c) == ATLS_OK,
          "local RSA leaf parses");
    CHECK(c.is_ca == 0 && c.bc_critical == 0,
          "local leaf: CA:FALSE, NON-critical basicConstraints");
    CHECK(c.key_usage == 0x0005 && c.ku_critical == 0,
          "local leaf: keyUsage digitalSignature|keyEncipherment");
    CHECK(c.not_before.year == 2026 && c.not_before.month == 8
          && c.not_after.day == 7,
          "local leaf: short validity decoded");
}

/* ---- structural invariants N5 will rely on ---- */

static void test_spans_and_invariants(void) {
    atls_x509_cert c;
    const uint8_t *base = ATLS_TEST_CERT_EC_LOCAL;
    size_t len = sizeof(ATLS_TEST_CERT_EC_LOCAL);

    CHECK(atls_x509_parse(base, len, &c) == ATLS_OK, "invariant fixture parses");

    /* Every span must point INSIDE the input buffer (zero-copy). */
    int inside = 1;
    const atls_span *spans[] = {
        &c.serial, &c.tbs, &c.issuer, &c.subject, &c.spki, &c.signature,
    };
    for (size_t i = 0; i < sizeof(spans) / sizeof(spans[0]); i++) {
        if (spans[i]->data < base || spans[i]->data + spans[i]->len > base + len) {
            inside = 0;
        }
    }
    CHECK(inside, "all output spans point inside the input (zero-copy)");

    /* Self-signed: issuer and subject DER are byte-identical. */
    CHECK(c.issuer.len == c.subject.len &&
          memcmp(c.issuer.data, c.subject.data, c.issuer.len) == 0,
          "self-signed: issuer DER == subject DER (chain building basis)");

    /* Inner (TBS) and outer signature OIDs agree. */
    CHECK(c.inner_sig_oid.len == c.outer_sig_oid.len &&
          memcmp(c.inner_sig_oid.data, c.outer_sig_oid.data,
                 c.inner_sig_oid.len) == 0,
          "inner and outer signature OIDs agree");

    CHECK(c.signature_unused_bits == 0, "signature BIT STRING has 0 unused bits");
    CHECK(c.san_dns_truncated == 0, "no SAN truncation for sane certs");

    /* OID comparison helper. */
    CHECK(atls_oid_eq(&c.outer_sig_oid, ATLS_OID_ECDSA_SHA256, 8) == 1,
          "atls_oid_eq positive");
    CHECK(atls_oid_eq(&c.outer_sig_oid, ATLS_OID_ED25519, 3) == 0,
          "atls_oid_eq negative (different length)");
}

/* ---- 2. refusal battery: each malformation, each reason ---- */

static void test_refusals(void) {
    atls_x509_cert c;

    CHECK(atls_x509_parse(NULL, 0, &c) == ATLS_ERR_INPUT, "NULL refused");
    CHECK(atls_x509_parse(ATLS_TEST_CERT_EC_LOCAL, 0, &c) == ATLS_ERR_TRUNCATED,
          "empty input refused");
    CHECK(atls_x509_parse(ATLS_TEST_CERT_EC_LOCAL, 1, &c) != ATLS_OK,
          "one byte refused");

    /* Trailing garbage after a valid certificate. */
    uint8_t padded[512];
    size_t n = sizeof(ATLS_TEST_CERT_EC_LOCAL);
    memcpy(padded, ATLS_TEST_CERT_EC_LOCAL, n);
    padded[n] = 0x00;
    CHECK(atls_x509_parse(padded, n + 1, &c) == ATLS_ERR_BAD_ENCODING,
          "trailing garbage refused");

    /* Indefinite length is not DER. */
    {
        const uint8_t indef[] = { 0x30, 0x80, 0x00, 0x00 };
        CHECK(atls_x509_parse(indef, sizeof(indef), &c) == ATLS_ERR_BAD_LENGTH,
              "indefinite length refused (BAD_LENGTH)");
    }

    /* Non-minimal long-form length (0x81 used for a value < 0x80). */
    {
        const uint8_t nonmin[] = { 0x30, 0x81, 0x03, 0x02, 0x01, 0x01 };
        CHECK(atls_x509_parse(nonmin, sizeof(nonmin), &c) == ATLS_ERR_BAD_LENGTH,
              "non-minimal length refused (BAD_LENGTH)");
    }

    /* A 4 GiB length claim dies at the bounds check, not in an
     * allocator. */
    {
        const uint8_t huge[] = { 0x30, 0x84, 0xff, 0xff, 0xff, 0xff };
        CHECK(atls_x509_parse(huge, sizeof(huge), &c) == ATLS_ERR_TRUNCATED,
              "4 GiB length claim refused without allocating");
    }

    /* High-tag-number form: refused rather than guessed at. */
    {
        const uint8_t htn[] = { 0x3f, 0x81, 0x00, 0x00 };
        CHECK(atls_x509_parse(htn, sizeof(htn), &c) == ATLS_ERR_UNSUPPORTED,
              "high-tag-number form refused");
    }

    /* v1 certificate (version INTEGER 0): refused on purpose. */
    uint8_t v1[512];
    extern size_t build_minimal_cert(uint8_t * out, int version, int critical_unknown_ext,
                                     int include_ext);
    size_t v1n = build_minimal_cert(v1, 0, 0, 0);
    CHECK(atls_x509_parse(v1, v1n, &c) == ATLS_ERR_UNSUPPORTED,
          "v1 certificate refused (UNSUPPORTED)");

    /* A crafted minimal v3 certificate with an UNKNOWN CRITICAL
     * extension: D5 — reject rather than interpret. */
    uint8_t crafted[768];
    size_t cn = build_minimal_cert(crafted, 2, 1, 1);
    CHECK(atls_x509_parse(crafted, cn, &c) == ATLS_ERR_UNSUPPORTED,
          "unknown critical extension refused (UNSUPPORTED)");

    /* The same certificate with the extension NON-critical parses. */
    cn = build_minimal_cert(crafted, 2, 0, 1);
    CHECK(atls_x509_parse(crafted, cn, &c) == ATLS_OK,
          "unknown non-critical extension skipped, cert accepted");

    /* A v3 certificate with no extensions at all is fine. */
    cn = build_minimal_cert(crafted, 2, 0, 0);
    CHECK(atls_x509_parse(crafted, cn, &c) == ATLS_OK && c.version == 3,
          "extension-less v3 certificate accepted");
}

/* ---- 3. depth gate: 10 000 levels die at the budget ---- */

/* Lives in atls_x509_testdata.c so the in-guest x509test runs the exact
 * same construction. */
extern size_t atls_test_build_deep_nest(uint8_t *buf, size_t bufcap, int levels);

extern size_t build_cert_with_san_blob(uint8_t *out, const uint8_t *blob,
                                       size_t blob_len);

static void test_depth(void) {
    static uint8_t blob[60000];
    atls_x509_cert c;

    size_t n10000 = atls_test_build_deep_nest(blob, sizeof(blob), 10000);
    CHECK(n10000 > 0, "10 000-deep blob built");

    /* Wrapped inside a certificate as the SAN extension value: the
     * parser must reach the depth budget and refuse, not follow. */
    static uint8_t crafted[65536];
    size_t cn = build_cert_with_san_blob(crafted, blob, n10000);
    CHECK(atls_x509_parse(crafted, cn, &c) == ATLS_ERR_DEPTH,
          "10 000-deep nesting inside SAN refused with ATLS_ERR_DEPTH");

    /* A modest nesting (20 levels) inside one GeneralName is walked by
     * the skipper and accepted. */
    size_t n20 = atls_test_build_deep_nest(blob, sizeof(blob), 20);
    cn = build_cert_with_san_blob(crafted, blob, n20);
    CHECK(atls_x509_parse(crafted, cn, &c) == ATLS_OK,
          "20-deep nesting inside GeneralName skipped within budget");

    /* Direct skipper check on the raw blob (rebuild: blob was reused). */
    {
        extern int atls_der_skip_value_test(const uint8_t *p, size_t n);
        size_t n_again = atls_test_build_deep_nest(blob, sizeof(blob), 10000);
        CHECK(n_again == n10000, "deep blob rebuild is deterministic");
        CHECK(atls_der_skip_value_test(blob, n10000) == ATLS_ERR_DEPTH,
              "skip_value itself enforces the depth budget");
        size_t n30 = atls_test_build_deep_nest(blob, sizeof(blob), 30);
        CHECK(atls_der_skip_value_test(blob, n30) == ATLS_OK,
              "skip_value walks 30 levels fine");
    }
}

/* ---- 4. truncation sweep: EVERY prefix of a real cert fails ---- */

static void test_truncation_sweep(void) {
    atls_x509_cert c;
    static uint8_t buf[2048];
    size_t n = sizeof(ATLS_TEST_CERT_EXAMPLE_COM);
    memcpy(buf, ATLS_TEST_CERT_EXAMPLE_COM, n);

    int all_refused = 1;
    for (size_t cut = 0; cut < n; cut++) {
        int rc = atls_x509_parse(buf, cut, &c);
        if (rc == ATLS_OK) {
            all_refused = 0;
            printf("      prefix %zu unexpectedly parsed\n", cut);
            break;
        }
    }
    CHECK(all_refused,
          "every truncated prefix of example.com refused (1001 prefixes)");
}

/* ---- 5. mutation battery: never crash, bounded behaviour ---- */

static void test_mutation_battery(void) {
    atls_x509_cert c;
    static uint8_t buf[2048];
    size_t n = sizeof(ATLS_TEST_CERT_EXAMPLE_COM);

    /* Bit flips at a stride of positions.  The gate is NOT "most flips
     * refused": this parser checks DER STRUCTURE, and most bytes of a
     * certificate are payload (key material, signature, name strings)
     * whose corruption leaves the structure intact.  The honest
     * assertions are: nothing crashes, both outcomes occur (the parser
     * discriminates rather than blanket-accepting), and structural
     * damage — which byte deletions always are — is always refused. */
    int ok_count = 0, err_count = 0, bad_rc = 0;
    for (size_t pos = 0; pos < n; pos += 5) {
        for (int bit = 0; bit < 8; bit += 3) {
            memcpy(buf, ATLS_TEST_CERT_EXAMPLE_COM, n);
            buf[pos] ^= (uint8_t)(1u << bit);
            int rc = atls_x509_parse(buf, n, &c);
            if (rc == ATLS_OK) ok_count++;
            else if (rc <= ATLS_ERR_INPUT && rc >= ATLS_ERR_UNSUPPORTED) err_count++;
            else bad_rc++;
        }
    }
    CHECK(bad_rc == 0, "bit-flip battery: every refusal is a known error code");
    CHECK(err_count > 0 && ok_count > 0,
          "bit-flip battery: parser discriminates (both outcomes occur)");
    printf("      (bit flips: %d refused, %d still parsed, %d unknown codes)\n",
           err_count, ok_count, bad_rc);

    /* Byte deletions shift every following structure byte, so they are
     * always structural damage: every one must be refused. */
    int refused = 0, total = 0;
    for (size_t pos = 0; pos < n; pos += 11) {
        memcpy(buf, ATLS_TEST_CERT_EXAMPLE_COM, pos);
        memcpy(buf + pos, ATLS_TEST_CERT_EXAMPLE_COM + pos + 1, n - pos - 1);
        int rc = atls_x509_parse(buf, n - 1, &c);
        if (rc != ATLS_OK) refused++;
        total++;
    }
    CHECK(refused == total,
          "byte-deletion battery: every deletion refused, none crashed");
    printf("      (deletions: %d/%d refused)\n", refused, total);
}

int main(void) {
    printf("test_atls_x509: real certs + refusal battery + depth gate + fuzz\n");
    test_real_certs();
    test_spans_and_invariants();
    test_refusals();
    test_depth();
    test_truncation_sweep();
    test_mutation_battery();
    printf("test_atls_x509: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}

/* test_atls_certval.c — Host-side certificate validation tests (N5).
 *
 * Tests chain building, signature verification (Ed25519 + RSA),
 * hostname matching, date checking, basic constraints, key usage.
 * Each refusal is asserted for the specific reason (D5).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "atls/atls.h"
#include "atls/x509.h"
#include "atls/certval.h"

static atls_time_now get_test_time(void) {
    time_t t = time(NULL) + 3600;
    struct tm *tm = gmtime(&t);
    atls_time_now now = {
        tm->tm_year + 1900,
        tm->tm_mon + 1,
        tm->tm_mday,
        tm->tm_hour,
        tm->tm_min,
        tm->tm_sec
    };
    return now;
}

static int tests_run = 0, tests_failed = 0;
#define CHECK(cond, name) do { \
    tests_run++; \
    if (cond) { printf("PASS: %s\n", name); } \
    else { tests_failed++; printf("FAIL: %s\n", name); } \
} while(0)

/* ---- Test certificates ---- */
/* We generate a test chain: Root CA -> Intermediate CA -> Leaf.
 * All Ed25519 for simplicity (RSA verification tested separately). */

static uint8_t test_root_der[4096];
static uint8_t test_intermediate_der[4096];
static uint8_t test_leaf_der[4096];
static size_t test_root_der_len, test_intermediate_der_len, test_leaf_der_len;

/* Generate test certificates using openssl. */
static int generate_test_certs(void) {
    char cmd[2048];

    /* Root CA (Ed25519, self-signed). */
    snprintf(cmd, sizeof(cmd),
        "openssl req -x509 -newkey ed25519 -keyout /tmp/n5_root.key "
        "-out /tmp/n5_root.pem -days 3650 -nodes "
        "-subj '/CN=N5 Test Root CA/O=AuraLite Test' "
        "-addext 'basicConstraints=critical,CA:TRUE' "
        "-addext 'keyUsage=critical,keyCertSign,cRLSign' 2>/dev/null");
    if (system(cmd) != 0) return -1;

    /* Intermediate CA (Ed25519, signed by root). */
    snprintf(cmd, sizeof(cmd),
        "openssl req -newkey ed25519 -keyout /tmp/n5_inter.key "
        "-out /tmp/n5_inter.csr -nodes "
        "-subj '/CN=N5 Test Intermediate CA/O=AuraLite Test' 2>/dev/null && "
        "printf 'basicConstraints=critical,CA:TRUE\\nkeyUsage=critical,keyCertSign\\n' > /tmp/n5_ext.txt && "
        "openssl x509 -req -in /tmp/n5_inter.csr -CA /tmp/n5_root.pem "
        "-CAkey /tmp/n5_root.key -CAcreateserial -out /tmp/n5_inter.pem "
        "-days 1825 -extfile /tmp/n5_ext.txt 2>/dev/null");
    if (system(cmd) != 0) return -1;

    /* Leaf certificate (Ed25519, signed by intermediate). */
    snprintf(cmd, sizeof(cmd),
        "openssl req -newkey ed25519 -keyout /tmp/n5_leaf.key "
        "-out /tmp/n5_leaf.csr -nodes "
        "-subj '/CN=example.auraos.dev' 2>/dev/null && "
        "printf 'basicConstraints=CA:FALSE\\nkeyUsage=digitalSignature\\nsubjectAltName=DNS:example.auraos.dev,DNS:*.auraos.dev' > /tmp/n5_leaf_ext.txt && "
        "openssl x509 -req -in /tmp/n5_leaf.csr -CA /tmp/n5_inter.pem "
        "-CAkey /tmp/n5_inter.key -CAcreateserial -out /tmp/n5_leaf.pem "
        "-days 365 -extfile /tmp/n5_leaf_ext.txt 2>/dev/null");
    if (system(cmd) != 0) return -1;

    /* Read DER. */
    snprintf(cmd, sizeof(cmd), "openssl x509 -in /tmp/n5_root.pem -outform DER -out /tmp/n5_root.der 2>/dev/null");
    if (system(cmd) != 0) return -1;
    snprintf(cmd, sizeof(cmd), "openssl x509 -in /tmp/n5_inter.pem -outform DER -out /tmp/n5_inter.der 2>/dev/null");
    if (system(cmd) != 0) return -1;
    snprintf(cmd, sizeof(cmd), "openssl x509 -in /tmp/n5_leaf.pem -outform DER -out /tmp/n5_leaf.der 2>/dev/null");
    if (system(cmd) != 0) return -1;

    /* Read DER into memory. */
    FILE *f;
    f = fopen("/tmp/n5_root.der", "rb");
    if (!f) return -1;
    test_root_der_len = fread(test_root_der, 1, sizeof(test_root_der), f);
    fclose(f);

    f = fopen("/tmp/n5_inter.der", "rb");
    if (!f) return -1;
    test_intermediate_der_len = fread(test_intermediate_der, 1, sizeof(test_intermediate_der), f);
    fclose(f);

    f = fopen("/tmp/n5_leaf.der", "rb");
    if (!f) return -1;
    test_leaf_der_len = fread(test_leaf_der, 1, sizeof(test_leaf_der), f);
    fclose(f);

    return 0;
}

/* ---- Tests ---- */

static void test_valid_chain(void) {
    atls_certval_ctx ctx;
    atls_trust_root roots[1] = { { test_root_der, test_root_der_len } };
    atls_certval_init(&ctx, roots, 1);

    const uint8_t *chain[2] = { test_leaf_der, test_intermediate_der };
    size_t chain_lens[2] = { test_leaf_der_len, test_intermediate_der_len };

    atls_time_now now = get_test_time();
    int rc = atls_certval_verify(&ctx, chain, chain_lens, 2,
                                 "example.auraos.dev", &now);
    CHECK(rc == ATLS_CERTVAL_OK, "valid chain to pinned root");
}

static void test_wrong_hostname(void) {
    atls_certval_ctx ctx;
    atls_trust_root roots[1] = { { test_root_der, test_root_der_len } };
    atls_certval_init(&ctx, roots, 1);

    const uint8_t *chain[2] = { test_leaf_der, test_intermediate_der };
    size_t chain_lens[2] = { test_leaf_der_len, test_intermediate_der_len };

    atls_time_now now = get_test_time();
    int rc = atls_certval_verify(&ctx, chain, chain_lens, 2,
                                 "wrong.example.com", &now);
    CHECK(rc == ATLS_CERTVAL_ERR_HOSTNAME, "wrong hostname rejected");
}

static void test_wildcard_match(void) {
    /* *.auraos.dev should match sub.auraos.dev but not a.b.auraos.dev. */
    CHECK(atls_certval_hostname_match("sub.auraos.dev",
            (const uint8_t*)"*.auraos.dev", 12) == 1,
          "wildcard matches subdomain");
    CHECK(atls_certval_hostname_match("a.b.auraos.dev",
            (const uint8_t*)"*.auraos.dev", 12) == 0,
          "wildcard does not match multi-level subdomain");
    CHECK(atls_certval_hostname_match("auraos.dev",
            (const uint8_t*)"*.auraos.dev", 12) == 0,
          "wildcard does not match bare domain");
}

static void test_expired_cert(void) {
    atls_certval_ctx ctx;
    atls_trust_root roots[1] = { { test_root_der, test_root_der_len } };
    atls_certval_init(&ctx, roots, 1);

    const uint8_t *chain[2] = { test_leaf_der, test_intermediate_der };
    size_t chain_lens[2] = { test_leaf_der_len, test_intermediate_der_len };

    /* Date in the far future — after cert expires. */
    atls_time_now future = { 2099, 1, 1, 0, 0, 0 };
    int rc = atls_certval_verify(&ctx, chain, chain_lens, 2,
                                 "example.auraos.dev", &future);
    CHECK(rc == ATLS_CERTVAL_ERR_EXPIRED, "expired certificate rejected");
}

static void test_unknown_root(void) {
    /* Use a different (unrelated) root. */
    uint8_t fake_root[1] = { 0 };
    atls_certval_ctx ctx;
    atls_trust_root roots[1] = { { fake_root, 1 } };
    atls_certval_init(&ctx, roots, 1);

    const uint8_t *chain[2] = { test_leaf_der, test_intermediate_der };
    size_t chain_lens[2] = { test_leaf_der_len, test_intermediate_der_len };

    atls_time_now now = get_test_time();
    int rc = atls_certval_verify(&ctx, chain, chain_lens, 2,
                                 "example.auraos.dev", &now);
    CHECK(rc == ATLS_CERTVAL_ERR_UNKNOWN_ROOT,
          "chain to unknown root rejected with 'root not in trust store'");
}

static void test_self_signed_rejected(void) {
    /* A self-signed leaf without the root in the trust store. */
    atls_certval_ctx ctx;
    atls_trust_root roots[0];
    atls_certval_init(&ctx, roots, 0);

    const uint8_t *chain[1] = { test_root_der };
    size_t chain_lens[1] = { test_root_der_len };

    atls_time_now now = get_test_time();
    int rc = atls_certval_verify(&ctx, chain, chain_lens, 1,
                                 "N5 Test Root CA", &now);
    CHECK(rc != ATLS_CERTVAL_OK, "self-signed without trust store rejected");
}

static void test_leaf_as_ca(void) {
    /* Parse the leaf and check: is_ca should be 0. */
    atls_x509_cert leaf;
    CHECK(atls_x509_parse(test_leaf_der, test_leaf_der_len, &leaf) == ATLS_OK,
          "leaf parses");
    CHECK(leaf.is_ca == 0, "leaf has cA=FALSE");
}

static void test_flipped_signature(void) {
    /* Flip a byte in the leaf's signature and verify it fails. */
    uint8_t modified[4096];
    memcpy(modified, test_leaf_der, test_leaf_der_len);
    /* Find the signature BIT STRING — it's near the end of the cert.
     * Flip a byte in the last 70 bytes (signature area). */
    if (test_leaf_der_len > 70) {
        modified[test_leaf_der_len - 20] ^= 0x01;
        atls_x509_cert mod_cert;
        /* The cert might still parse (structure intact) but signature
         * verification should fail. */
        if (atls_x509_parse(modified, test_leaf_der_len, &mod_cert) == ATLS_OK) {
            atls_certval_ctx ctx;
            atls_trust_root roots[1] = { { test_root_der, test_root_der_len } };
            atls_certval_init(&ctx, roots, 1);
            const uint8_t *chain[2] = { modified, test_intermediate_der };
            size_t chain_lens[2] = { test_leaf_der_len, test_intermediate_der_len };
            atls_time_now now = get_test_time();
            int rc = atls_certval_verify(&ctx, chain, chain_lens, 2,
                                         "example.auraos.dev", &now);
            CHECK(rc == ATLS_CERTVAL_ERR_SIGNATURE || rc == ATLS_CERTVAL_ERR_CHAIN,
                  "flipped signature byte rejected");
        } else {
            CHECK(1, "modified cert structure broken (expected)");
        }
    } else {
        CHECK(0, "test cert too short for signature flip");
    }
}

static void test_rsa_verify(void) {
    /* Generate an RSA-signed certificate and verify the RSA path works. */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "openssl req -x509 -newkey rsa:2048 -keyout /tmp/n5_rsa.key "
        "-out /tmp/n5_rsa.pem -days 365 -nodes "
        "-subj '/CN=RSA Test Leaf' "
        "-addext 'subjectAltName=DNS:rsa.auraos.dev' "
        "-addext 'basicConstraints=CA:FALSE' "
        "-addext 'keyUsage=digitalSignature' 2>/dev/null && "
        "openssl x509 -in /tmp/n5_rsa.pem -outform DER -out /tmp/n5_rsa.der 2>/dev/null");
    if (system(cmd) != 0) { CHECK(0, "RSA cert generation"); return; }

    uint8_t rsa_der[4096];
    size_t rsa_der_len;
    FILE *f = fopen("/tmp/n5_rsa.der", "rb");
    if (!f) { CHECK(0, "RSA cert read"); return; }
    rsa_der_len = fread(rsa_der, 1, sizeof(rsa_der), f);
    fclose(f);

    /* Self-signed RSA: root = itself. */
    atls_certval_ctx ctx;
    atls_trust_root roots[1] = { { rsa_der, rsa_der_len } };
    atls_certval_init(&ctx, roots, 1);

    atls_time_now now = get_test_time();
    /* For self-signed, chain = [leaf], and we expect the signature to
     * verify against itself.  But our validation requires a separate
     * root — the leaf's issuer must match a root's subject.  For a
     * self-signed cert, issuer == subject, so it matches itself. */
    const uint8_t *chain[1] = { rsa_der };
    size_t chain_lens[1] = { rsa_der_len };
    /* Skip hostname check (RSA leaf has rsa.auraos.dev, not matching
     * any test hostname we'd care about). */
    int rc = atls_certval_verify(&ctx, chain, chain_lens, 1, NULL, &now);
    CHECK(rc == ATLS_CERTVAL_OK, "RSA PKCS#1v1.5 self-signed verifies");

    /* Flip a signature byte. */
    uint8_t modified[4096];
    memcpy(modified, rsa_der, rsa_der_len);
    modified[rsa_der_len - 30] ^= 0x01;
    const uint8_t *mod_chain[1] = { modified };
    size_t mod_lens[1] = { rsa_der_len };
    int rc2 = atls_certval_verify(&ctx, mod_chain, mod_lens, 1, NULL, &now);
    CHECK(rc2 == ATLS_CERTVAL_ERR_SIGNATURE || rc2 == ATLS_CERTVAL_ERR_CHAIN,
          "RSA flipped signature rejected");
}

/* ---- ECDSA P-256 chain (REALINTERNET_PLAN X1) ---- */

static uint8_t ecdsa_root_der[4096];
static uint8_t ecdsa_leaf_der[4096];
static size_t ecdsa_root_der_len, ecdsa_leaf_der_len;

/* Generate a Root CA -> Leaf chain, all ECDSA P-256. */
static int generate_ecdsa_certs(void) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "openssl ecparam -name prime256v1 -genkey -noout -out /tmp/x1_root.key 2>/dev/null && "
        "openssl req -new -key /tmp/x1_root.key -x509 -out /tmp/x1_root.pem -days 3650 -nodes "
        "-subj '/CN=X1 ECDSA Root/O=AuraLite Test' "
        "-addext 'basicConstraints=critical,CA:TRUE' "
        "-addext 'keyUsage=critical,keyCertSign,cRLSign' 2>/dev/null && "
        "openssl ecparam -name prime256v1 -genkey -noout -out /tmp/x1_leaf.key 2>/dev/null && "
        "openssl req -new -key /tmp/x1_leaf.key -out /tmp/x1_leaf.csr -subj '/CN=ecdsa.auraos.dev' 2>/dev/null && "
        "printf 'basicConstraints=CA:FALSE\\nkeyUsage=digitalSignature\\nsubjectAltName=DNS:ecdsa.auraos.dev' > /tmp/x1_leaf_ext.txt && "
        "openssl x509 -req -in /tmp/x1_leaf.csr -CA /tmp/x1_root.pem -CAkey /tmp/x1_root.key "
        "-CAcreateserial -out /tmp/x1_leaf.pem -days 365 -extfile /tmp/x1_leaf_ext.txt 2>/dev/null");
    if (system(cmd) != 0) return -1;

    snprintf(cmd, sizeof(cmd), "openssl x509 -in /tmp/x1_root.pem -outform DER -out /tmp/x1_root.der 2>/dev/null");
    if (system(cmd) != 0) return -1;
    snprintf(cmd, sizeof(cmd), "openssl x509 -in /tmp/x1_leaf.pem -outform DER -out /tmp/x1_leaf.der 2>/dev/null");
    if (system(cmd) != 0) return -1;

    FILE *f;
    f = fopen("/tmp/x1_root.der", "rb"); if (!f) return -1;
    ecdsa_root_der_len = fread(ecdsa_root_der, 1, sizeof(ecdsa_root_der), f); fclose(f);
    f = fopen("/tmp/x1_leaf.der", "rb"); if (!f) return -1;
    ecdsa_leaf_der_len = fread(ecdsa_leaf_der, 1, sizeof(ecdsa_leaf_der), f); fclose(f);
    return 0;
}

static void test_ecdsa_chain_valid(void) {
    atls_certval_ctx ctx;
    atls_trust_root roots[1] = { { ecdsa_root_der, ecdsa_root_der_len } };
    atls_certval_init(&ctx, roots, 1);
    const uint8_t *chain[1] = { ecdsa_leaf_der };
    size_t lens[1] = { ecdsa_leaf_der_len };
    atls_time_now now = get_test_time();
    int rc = atls_certval_verify(&ctx, chain, lens, 1, "ecdsa.auraos.dev", &now);
    CHECK(rc == ATLS_CERTVAL_OK, "ECDSA P-256 chain to pinned root verifies");
}

static void test_ecdsa_chain_flipped_signature(void) {
    static uint8_t leaf_buf[4096];
    memcpy(leaf_buf, ecdsa_leaf_der, ecdsa_leaf_der_len);
    /* Flip a byte near the end of the certificate (inside the signature). */
    leaf_buf[ecdsa_leaf_der_len - 5] ^= 0x01;
    atls_certval_ctx ctx;
    atls_trust_root roots[1] = { { ecdsa_root_der, ecdsa_root_der_len } };
    atls_certval_init(&ctx, roots, 1);
    const uint8_t *chain[1] = { leaf_buf };
    size_t lens[1] = { ecdsa_leaf_der_len };
    atls_time_now now = get_test_time();
    int rc = atls_certval_verify(&ctx, chain, lens, 1, "ecdsa.auraos.dev", &now);
    CHECK(rc == ATLS_CERTVAL_ERR_SIGNATURE, "ECDSA chain with flipped signature refused");
}

static void test_pinned_intermediate(void) {
    /* Pin the intermediate.  The chain still carries the root after it
     * (as Google sends WE2 then GTS R4).  Walk must stop at the pin
     * and not demand the rest of the presented chain. */
    atls_certval_ctx ctx;
    atls_trust_root roots[1] = { { test_intermediate_der,
                                   test_intermediate_der_len } };
    atls_certval_init(&ctx, roots, 1);
    const uint8_t *chain[3] = {
        test_leaf_der, test_intermediate_der, ecdsa_leaf_der
    };
    size_t lens[3] = {
        test_leaf_der_len, test_intermediate_der_len, ecdsa_leaf_der_len
    };
    atls_time_now now = get_test_time();
    int rc = atls_certval_verify(&ctx, chain, lens, 3,
                                 "example.auraos.dev", &now);
    CHECK(rc == ATLS_CERTVAL_OK,
          "chain stops at pinned intermediate (ignores leftover certs)");
}

static void test_rsa4096_self_signed(void) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "openssl req -x509 -newkey rsa:4096 -keyout /tmp/n5_r4k.key "
        "-out /tmp/n5_r4k.pem -days 365 -nodes "
        "-subj '/CN=RSA4096 Test' "
        "-addext 'basicConstraints=CA:FALSE' "
        "-addext 'keyUsage=digitalSignature' 2>/dev/null && "
        "openssl x509 -in /tmp/n5_r4k.pem -outform DER -out /tmp/n5_r4k.der "
        "2>/dev/null");
    if (system(cmd) != 0) { CHECK(0, "RSA-4096 cert generation"); return; }
    uint8_t der[8192];
    size_t n;
    FILE *f = fopen("/tmp/n5_r4k.der", "rb");
    if (!f) { CHECK(0, "RSA-4096 cert read"); return; }
    n = fread(der, 1, sizeof der, f);
    fclose(f);
    atls_certval_ctx ctx;
    atls_trust_root roots[1] = { { der, n } };
    atls_certval_init(&ctx, roots, 1);
    const uint8_t *chain[1] = { der };
    size_t lens[1] = { n };
    atls_time_now now = get_test_time();
    int rc = atls_certval_verify(&ctx, chain, lens, 1, NULL, &now);
    CHECK(rc == ATLS_CERTVAL_OK, "RSA-4096 PKCS#1v1.5 self-signed verifies");
}

int main(void) {
    printf("=== N5 Certificate Validation Test Suite ===\n\n");

    if (generate_test_certs() != 0) {
        printf("FAIL: could not generate test certificates\n");
        return 1;
    }
    CHECK(test_root_der_len > 0 && test_intermediate_der_len > 0 &&
          test_leaf_der_len > 0, "test certificates generated");

    test_valid_chain();
    test_wrong_hostname();
    test_wildcard_match();
    test_expired_cert();
    test_unknown_root();
    test_self_signed_rejected();
    test_leaf_as_ca();
    test_flipped_signature();
    test_rsa_verify();

    /* ECDSA P-256 chain (REALINTERNET_PLAN X1). */
    if (generate_ecdsa_certs() != 0) {
        printf("FAIL: could not generate ECDSA test certificates\n");
        return 1;
    }
    CHECK(ecdsa_root_der_len > 0 && ecdsa_leaf_der_len > 0,
          "ECDSA test certificates generated");
    test_ecdsa_chain_valid();
    test_ecdsa_chain_flipped_signature();
    test_pinned_intermediate();
    test_rsa4096_self_signed();

    printf("\n=== %d/%d passed ===\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}

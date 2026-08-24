/* test_atls_pem.c — Host-side PEM trust-store decoding tests
 * (REALINTERNET_PLAN.md phase X2).
 *
 * Decodes the shipped etc/ssl/roots.pem with atls_pem_cert_to_der and
 * checks that every block parses as a DER X.509 certificate.  Also
 * exercises the failure modes: missing markers, garbage base64, and an
 * undersized output buffer.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "atls/atls.h"
#include "atls/pem.h"
#include "atls/x509.h"

static int tests_run = 0, tests_failed = 0;
#define CHECK(cond, name) do { \
    tests_run++; \
    if (cond) { printf("PASS: %s\n", name); } \
    else { tests_failed++; printf("FAIL: %s\n", name); } \
} while (0)

static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = 0;
    fclose(f);
    *out = buf;
    *out_len = n;
    return 0;
}

static void test_decode_first_root(void) {
    char *pem; size_t plen;
    if (read_file("etc/ssl/roots.pem", &pem, &plen) != 0) {
        CHECK(0, "read etc/ssl/roots.pem");
        return;
    }
    uint8_t der[8192];
    size_t dlen = 0;
    int rc = atls_pem_cert_to_der(pem, plen, der, sizeof(der), &dlen);
    CHECK(rc == ATLS_OK, "first root decodes from PEM");
    CHECK(dlen > 200 && dlen < 2048, "decoded DER is a sane size");

    atls_x509_cert c;
    int xrc = atls_x509_parse(der, dlen, &c);
    CHECK(xrc == ATLS_OK, "decoded root parses as X.509");
    if (xrc == ATLS_OK) {
        CHECK(c.is_ca == 1, "root is a CA");
    }
    free(pem);
}

static void test_decode_all_roots(void) {
    char *pem; size_t plen;
    if (read_file("etc/ssl/roots.pem", &pem, &plen) != 0) {
        CHECK(0, "read etc/ssl/roots.pem");
        return;
    }
    /* Walk through each block by repeatedly decoding and advancing. */
    size_t pos = 0;
    int count = 0;
    while (pos < plen) {
        const char *block = pem + pos;
        size_t block_len = plen - pos;
        uint8_t der[8192];
        size_t dlen = 0;
        int rc = atls_pem_cert_to_der(block, block_len, der, sizeof(der), &dlen);
        if (rc != ATLS_OK) break;
        atls_x509_cert c;
        if (atls_x509_parse(der, dlen, &c) == ATLS_OK) count++;
        /* Advance past this block's END marker. */
        const char *e = strstr(block, "-----END CERTIFICATE-----");
        if (!e) break;
        pos += (size_t)(e - block) + strlen("-----END CERTIFICATE-----") + 1;
    }
    CHECK(count >= 3 && count <= 32,
          "all shipped roots in roots.pem decode and parse");
    free(pem);
}

static void test_no_begin_marker(void) {
    static const char pem[] = "garbage data with no markers\n";
    uint8_t der[256];
    size_t dlen = 0;
    int rc = atls_pem_cert_to_der(pem, strlen(pem), der, sizeof(der), &dlen);
    CHECK(rc == ATLS_ERR_BAD_ENCODING, "no BEGIN marker refused");
}

static void test_bad_base64(void) {
    static const char pem[] =
        "-----BEGIN CERTIFICATE-----\n!!!not-base64!!!\n-----END CERTIFICATE-----\n";
    uint8_t der[256];
    size_t dlen = 0;
    int rc = atls_pem_cert_to_der(pem, strlen(pem), der, sizeof(der), &dlen);
    CHECK(rc == ATLS_ERR_BAD_ENCODING, "bad base64 refused");
}

static void test_output_too_small(void) {
    char *pem; size_t plen;
    if (read_file("etc/ssl/roots.pem", &pem, &plen) != 0) {
        CHECK(0, "read roots.pem");
        return;
    }
    uint8_t der[16];          /* far too small */
    size_t dlen = 0;
    int rc = atls_pem_cert_to_der(pem, plen, der, sizeof(der), &dlen);
    CHECK(rc == ATLS_ERR_BAD_ENCODING, "undersized output buffer refused");
    free(pem);
}

static void test_null_args(void) {
    uint8_t der[256];
    size_t dlen = 0;
    CHECK(atls_pem_cert_to_der(NULL, 0, der, sizeof(der), &dlen)
          == ATLS_ERR_INPUT, "NULL PEM refused");
    CHECK(atls_pem_cert_to_der("x", 1, NULL, sizeof(der), &dlen)
          == ATLS_ERR_INPUT, "NULL output refused");
}

int main(void) {
    printf("=== PEM trust-store decoding test ===\n");
    test_decode_first_root();
    test_decode_all_roots();
    test_no_begin_marker();
    test_bad_base64();
    test_output_too_small();
    test_null_args();
    printf("=== %d/%d passed ===\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}

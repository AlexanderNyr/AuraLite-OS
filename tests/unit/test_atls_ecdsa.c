/* test_atls_ecdsa.c — Host-side ECDSA P-256 verification tests
 * (REALINTERNET_PLAN.md phase X1).
 *
 * Verifies the real `atls_ecdsa_p256_verify` against an embedded
 * openssl-generated vector (the signature was produced and checked with
 * `openssl dgst -sha256 -sign/-verify`), then the hostile negative cases.
 * Each refusal is asserted for the specific reason, following the
 * exact-reason pattern of test_atls_ed25519/test_atls_certval.
 *
 * The point-on-curve check (Y^2 == x^3 - 3x + b) and the r,s range checks
 * are exercised directly because those are the two gates a malicious
 * certificate most often trips.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "atls/atls.h"
#include "atls/ecdsa.h"

static int tests_run = 0, tests_failed = 0;
#define CHECK(cond, name) do { \
    tests_run++; \
    if (cond) { printf("PASS: %s\n", name); } \
    else { tests_failed++; printf("FAIL: %s\n", name); } \
} while (0)

/* ---- Test vector (generated with openssl, signature verified OK) ---- */

static const uint8_t test_msg[] =
    "AuraLite P-256 ECDSA verification vector 2026";
static const size_t test_msg_len = sizeof(test_msg) - 1;

static const uint8_t test_point[65] = {
  0x04,
  0x82,0x6c,0x1e,0xaa,0x32,0x16,0x34,0x64,0x71,0x5a,0xd2,0xfa,0xc5,0x48,0xdd,0x6e,
  0x69,0xd4,0x6e,0xc1,0x95,0xf4,0x0d,0xbb,0xcb,0x28,0x23,0x05,0x7b,0x97,0x13,0x04,
  0xda,0x92,0x7a,0x92,0x07,0x2e,0x87,0xac,0x89,0xa9,0x5c,0x92,0xf7,0xd3,0x13,0xc9,
  0x79,0x3d,0x53,0x22,0x77,0x8e,0x1e,0x5e,0xf3,0x24,0x1f,0xee,0xff,0x8f,0x9d,0xc6
};
static const uint8_t test_sig_der[] = {
  0x30,0x45,0x02,0x20,0x04,0xd1,0x34,0x30,0xe9,0x0d,0xe7,0x80,0x56,0xd2,0xac,0x9a,
  0xb9,0x58,0x96,0x2d,0xe4,0xab,0x1c,0x6f,0x62,0x74,0x6e,0xb4,0xb0,0x26,0xb1,0xf5,
  0xbe,0xf5,0x9d,0xc2,0x02,0x21,0x00,0xdf,0x79,0xdb,0x90,0xc3,0x71,0xd2,0xa2,0x41,
  0xd8,0xaf,0x21,0x74,0x08,0x03,0xa6,0x18,0x3f,0xad,0x70,0xff,0x70,0x64,0xe3,0x76,
  0xeb,0xb4,0xe3,0x82,0x57,0x19,0x89
};

static void test_valid_signature(void) {
    int rc = atls_ecdsa_p256_verify(test_sig_der, sizeof(test_sig_der),
                                    test_point, test_msg, test_msg_len);
    CHECK(rc == ATLS_OK, "valid P-256 signature verifies");
}

static void test_tampered_message(void) {
    static const uint8_t tampered[] = "AuraLite P-256 ECDSA verification vector 2027";
    int rc = atls_ecdsa_p256_verify(test_sig_der, sizeof(test_sig_der),
                                    test_point, tampered, sizeof(tampered) - 1);
    CHECK(rc == ATLS_ERR_BAD_SIGNATURE, "tampered message refused (BAD_SIGNATURE)");
}

static void test_flipped_signature_byte(void) {
    uint8_t sig[sizeof(test_sig_der)];
    memcpy(sig, test_sig_der, sizeof(sig));
    sig[10] ^= 0x01;                          /* inside r */
    int rc = atls_ecdsa_p256_verify(sig, sizeof(sig),
                                    test_point, test_msg, test_msg_len);
    CHECK(rc == ATLS_ERR_BAD_SIGNATURE, "flipped signature byte refused");
}

static void test_flipped_public_key_byte(void) {
    uint8_t pk[65];
    memcpy(pk, test_point, 65);
    pk[1] ^= 0x40;                            /* X coordinate */
    int rc = atls_ecdsa_p256_verify(test_sig_der, sizeof(test_sig_der),
                                    pk, test_msg, test_msg_len);
    CHECK(rc != ATLS_OK, "corrupted public key refused");
}

static void test_off_curve_point(void) {
    /* Y off the curve: set Y = 1 (never on secp256r1 for a real X). */
    uint8_t pk[65];
    memcpy(pk, test_point, 65);
    memset(pk + 33, 0, 32);
    pk[64] = 1;                               /* Y = 1 */
    int rc = atls_ecdsa_p256_verify(test_sig_der, sizeof(test_sig_der),
                                    pk, test_msg, test_msg_len);
    CHECK(rc == ATLS_ERR_BAD_ENCODING, "off-curve point refused (BAD_ENCODING)");
}

static void test_compressed_point_refused(void) {
    uint8_t pk[65];
    memset(pk, 0, sizeof(pk));
    pk[0] = 0x03;                             /* compressed form */
    memcpy(pk + 1, test_point + 1, 32);
    int rc = atls_ecdsa_p256_verify(test_sig_der, sizeof(test_sig_der),
                                    pk, test_msg, test_msg_len);
    CHECK(rc == ATLS_ERR_BAD_ENCODING, "compressed point refused (BAD_ENCODING)");
}

static void test_bad_der_signature(void) {
    static const uint8_t malformed[] = { 0x30, 0x45, 0x02, 0x20 }; /* truncated */
    int rc = atls_ecdsa_p256_verify(malformed, sizeof(malformed),
                                    test_point, test_msg, test_msg_len);
    CHECK(rc == ATLS_ERR_BAD_ENCODING, "truncated DER signature refused");
}

static void test_r_zero_refused(void) {
    /* r = 0 (all-zero first INTEGER content). */
    static const uint8_t sig0[] = {
        0x30,0x44, 0x02,0x20,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x02,0x20,
        0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
    };
    int rc = atls_ecdsa_p256_verify(sig0, sizeof(sig0),
                                    test_point, test_msg, test_msg_len);
    CHECK(rc == ATLS_ERR_BAD_SIGNATURE, "r == 0 refused (BAD_SIGNATURE)");
}

static void test_s_over_group_order_refused(void) {
    /* s >= n: set s to n (ff..ff00000000..bce6faada7179e84f3b9cac2fc632551). */
    static const uint8_t sigbigs[] = {
        0x30,0x46, 0x02,0x21,0x00,
        0x04,0xd1,0x34,0x30,0xe9,0x0d,0xe7,0x80,0x56,0xd2,0xac,0x9a,0xb9,0x58,0x96,0x2d,
        0xe4,0xab,0x1c,0x6f,0x62,0x74,0x6e,0xb4,0xb0,0x26,0xb1,0xf5,0xbe,0xf5,0x9d,0xc2,
        0x02,0x21,0x00,
        0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51
    };
    int rc = atls_ecdsa_p256_verify(sigbigs, sizeof(sigbigs),
                                    test_point, test_msg, test_msg_len);
    CHECK(rc == ATLS_ERR_BAD_SIGNATURE, "s >= n refused (BAD_SIGNATURE)");
}

static void test_null_arguments(void) {
    int rc = atls_ecdsa_p256_verify(NULL, sizeof(test_sig_der),
                                    test_point, test_msg, test_msg_len);
    CHECK(rc == ATLS_ERR_BAD_ENCODING, "NULL signature refused");
}

int main(void) {
    test_valid_signature();
    test_tampered_message();
    test_flipped_signature_byte();
    test_flipped_public_key_byte();
    test_off_curve_point();
    test_compressed_point_refused();
    test_bad_der_signature();
    test_r_zero_refused();
    test_s_over_group_order_refused();
    test_null_arguments();

    printf("ECDSA P-256: %d/%d passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}

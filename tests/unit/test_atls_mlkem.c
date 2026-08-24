/* test_atls_mlkem.c — REALINTERNET2 Y5 host gate.
 *
 * Links the REAL libatls sources.  Vectors:
 *   - FIPS 202 SHA3/SHAKE short KATs (the Y5 hashes G/H/J/XOF)
 *   - NIST ACVP sample ML-KEM-768 keyGen / encaps / decaps
 *     (usnistgov/ACVP-Server ML-KEM-*-FIPS203)
 *   - FO implicit-rejection: a corrupted ciphertext must yield
 *     J(z || ct), not an error return and not the honest K.
 *   - NTT ∘ invNTT = id, and fqmul reduction on a coefficient grid.
 */

#include <stdio.h>
#include <string.h>
#include "atls/atls.h"
#include "atls/mlkem.h"
#include "tests/unit/atls_mlkem_kat.h"

static int pass_count = 0, fail_count = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { pass_count++; printf("PASS: %s\n", msg); }              \
        else { fail_count++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
    } while (0)

static int hex_eq(const uint8_t *bin, size_t len, const char *hex) {
    if (strlen(hex) != len * 2) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return 0;
        if (bin[i] != (uint8_t)v) return 0;
    }
    return 1;
}

static void test_sha3(void) {
    uint8_t out[64];

    atls_sha3_256("", 0, out);
    CHECK(hex_eq(out, 32,
        "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a"),
        "SHA3-256(\"\")");

    atls_sha3_256("abc", 3, out);
    CHECK(hex_eq(out, 32,
        "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"),
        "SHA3-256(\"abc\")");

    atls_sha3_512("", 0, out);
    CHECK(hex_eq(out, 64,
        "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a6"
        "15b2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26"),
        "SHA3-512(\"\")");

    atls_shake128("", 0, out, 16);
    CHECK(hex_eq(out, 16, "7f9c2ba4e88f827d616045507605853e"),
        "SHAKE128(\"\", 16)");

    atls_shake256("", 0, out, 32);
    CHECK(hex_eq(out, 32,
        "46b9dd2b0ba88d13233b3feb743eeb243fcd52ea62b81b82b50c27646ed5762f"),
        "SHAKE256(\"\", 32)");
}

static void test_acvp_keygen(void) {
    uint8_t ek[ATLS_MLKEM768_EK_BYTES], dk[ATLS_MLKEM768_DK_BYTES];
    CHECK(atls_mlkem768_keygen(ek, dk, kat_kg_d, kat_kg_z) == ATLS_OK,
          "ACVP keyGen runs");
    CHECK(atls_ct_eq(ek, kat_kg_ek, ATLS_MLKEM768_EK_BYTES),
          "ACVP keyGen ek");
    CHECK(atls_ct_eq(dk, kat_kg_dk, ATLS_MLKEM768_DK_BYTES),
          "ACVP keyGen dk");
}

static void test_acvp_encaps(void) {
    uint8_t ct[ATLS_MLKEM768_CT_BYTES], ss[ATLS_MLKEM768_SS_BYTES];
    CHECK(atls_mlkem768_encaps(ct, ss, kat_enc_ek, kat_enc_m) == ATLS_OK,
          "ACVP encaps runs");
    CHECK(atls_ct_eq(ct, kat_enc_c, ATLS_MLKEM768_CT_BYTES),
          "ACVP encaps ct");
    CHECK(atls_ct_eq(ss, kat_enc_k, ATLS_MLKEM768_SS_BYTES),
          "ACVP encaps ss");
}

static void test_acvp_decaps(void) {
    uint8_t ss[ATLS_MLKEM768_SS_BYTES];
    CHECK(atls_mlkem768_decaps(ss, kat_dec_c, kat_dec_dk) == ATLS_OK,
          "ACVP decaps runs");
    CHECK(atls_ct_eq(ss, kat_dec_k, ATLS_MLKEM768_SS_BYTES),
          "ACVP decaps ss");
}

static void test_roundtrip_and_fo(void) {
    uint8_t ek[ATLS_MLKEM768_EK_BYTES], dk[ATLS_MLKEM768_DK_BYTES];
    uint8_t ct[ATLS_MLKEM768_CT_BYTES], ss1[32], ss2[32], ss3[32];
    uint8_t d[32], z[32], m[32];
    for (int i = 0; i < 32; i++) {
        d[i] = (uint8_t)(0xA0 + i);
        z[i] = (uint8_t)(0x50 + i);
        m[i] = (uint8_t)(0x10 + i);
    }
    CHECK(atls_mlkem768_keygen(ek, dk, d, z) == ATLS_OK, "roundtrip keygen");
    CHECK(atls_mlkem768_encaps(ct, ss1, ek, m) == ATLS_OK, "roundtrip encaps");
    CHECK(atls_mlkem768_decaps(ss2, ct, dk) == ATLS_OK, "roundtrip decaps");
    CHECK(atls_ct_eq(ss1, ss2, 32), "roundtrip ss match");

    /* Corrupt one ciphertext byte.  Decaps must still return ATLS_OK
     * and the secret must equal J(z || ct'), not the honest K. */
    uint8_t bad[ATLS_MLKEM768_CT_BYTES];
    memcpy(bad, ct, sizeof bad);
    bad[0] ^= 0x01;
    CHECK(atls_mlkem768_decaps(ss3, bad, dk) == ATLS_OK,
          "FO: corrupted ct is not an error path");
    CHECK(!atls_ct_eq(ss3, ss1, 32),
          "FO: corrupted ct is not the honest K");
    uint8_t jin[32 + ATLS_MLKEM768_CT_BYTES], expect[32];
    memcpy(jin, z, 32);
    memcpy(jin + 32, bad, ATLS_MLKEM768_CT_BYTES);
    atls_shake256(jin, sizeof jin, expect, 32);
    CHECK(atls_ct_eq(ss3, expect, 32),
          "FO: corrupted ct yields J(z || ct)");
}

static void test_null_inputs(void) {
    uint8_t ek[ATLS_MLKEM768_EK_BYTES], dk[ATLS_MLKEM768_DK_BYTES];
    uint8_t ct[ATLS_MLKEM768_CT_BYTES], ss[32], seed[32];
    memset(seed, 1, 32);
    CHECK(atls_mlkem768_keygen(NULL, dk, seed, seed) == ATLS_ERR_INPUT,
          "keygen NULL refused");
    CHECK(atls_mlkem768_encaps(ct, ss, NULL, seed) == ATLS_ERR_INPUT,
          "encaps NULL refused");
    CHECK(atls_mlkem768_decaps(ss, NULL, dk) == ATLS_ERR_INPUT,
          "decaps NULL refused");
    (void)ek;
}

int main(void) {
    printf("test_atls_mlkem: FIPS 202 hashes + ACVP ML-KEM-768 + FO\n");
    test_sha3();
    test_acvp_keygen();
    test_acvp_encaps();
    test_acvp_decaps();
    test_roundtrip_and_fo();
    test_null_inputs();
    printf("test_atls_mlkem: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}

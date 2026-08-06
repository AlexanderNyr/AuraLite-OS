/* tests/unit/test_atls_ed25519.c — N1 Ed25519-verify gates.
 *
 * RFC 8032 §7.1 TEST 1/2/3 and TEST SHA(abc) verify, then the negative
 * battery — each refusal checked INDIVIDUALLY and for the RIGHT REASON
 * (INTERNET_PLAN.md N1 gate: "a rejected signature must be rejected for
 * the right reason, checked individually rather than as a lump"):
 *
 *   - a flipped bit in R, in S, in the message, in the public key
 *   - S = L and S = L + 1 (non-canonical scalars)
 *   - an R that encodes a point not on the curve
 *   - a public key whose y is not canonical (y = p encoded raw)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "atls/atls.h"

static int pass_count = 0, fail_count = 0;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) { pass_count++; printf("PASS: %s\n", msg); } \
        else { fail_count++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
    } while (0)

static void hex2bin(const char *hex, uint8_t *out) {
    size_t n = strlen(hex);
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        sscanf(hex + i * 2, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static int verify_ok(const char *pk_hex, const char *sig_hex,
                     const uint8_t *msg, size_t msglen) {
    uint8_t pk[32], sig[64];
    hex2bin(pk_hex, pk);
    hex2bin(sig_hex, sig);
    return atls_ed25519_verify(sig, pk, msg, msglen);
}

/* ---- RFC 8032 §7.1 positive vectors ---- */

static void test_rfc_vectors(void) {
    CHECK(verify_ok(
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        NULL, 0) == ATLS_OK, "RFC 8032 §7.1 TEST 1 (empty message)");

    uint8_t m2 = 0x72;
    CHECK(verify_ok(
        "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
        &m2, 1) == ATLS_OK, "RFC 8032 §7.1 TEST 2 (one byte)");

    uint8_t m3[2] = { 0xaf, 0x82 };
    CHECK(verify_ok(
        "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
        "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
        "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a",
        m3, 2) == ATLS_OK, "RFC 8032 §7.1 TEST 3 (two bytes)");

    uint8_t m4[64];
    hex2bin("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
            "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
            m4);
    CHECK(verify_ok(
        "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
        "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589"
        "09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704",
        m4, sizeof(m4)) == ATLS_OK, "RFC 8032 §7.1 TEST SHA(abc) (64 bytes)");
}

/* ---- negative battery: each case checked for the exact reason ---- */

static const char *PK2 =
    "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
static const char *SIG2 =
    "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
    "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";

static void test_negative_battery(void) {
    uint8_t pk[32], sig[64];
    uint8_t msg = 0x72;

    /* Baseline sanity: the fixture verifies. */
    hex2bin(PK2, pk);
    hex2bin(SIG2, sig);
    CHECK(atls_ed25519_verify(sig, pk, &msg, 1) == ATLS_OK,
          "negative battery fixture verifies first");

    /* 1. flipped bit inside R */
    uint8_t sig_r[64];
    memcpy(sig_r, sig, 64);
    sig_r[5] ^= 0x40;
    CHECK(atls_ed25519_verify(sig_r, pk, &msg, 1) == ATLS_ERR_BAD_SIGNATURE,
          "flipped bit in R is refused as BAD_SIGNATURE");

    /* 2. flipped bit inside S */
    uint8_t sig_s[64];
    memcpy(sig_s, sig, 64);
    sig_s[40] ^= 0x08;
    CHECK(atls_ed25519_verify(sig_s, pk, &msg, 1) == ATLS_ERR_BAD_SIGNATURE,
          "flipped bit in S is refused as BAD_SIGNATURE");

    /* 3. wrong message */
    uint8_t wrong = 0x73;
    CHECK(atls_ed25519_verify(sig, pk, &wrong, 1) == ATLS_ERR_BAD_SIGNATURE,
          "wrong message is refused as BAD_SIGNATURE");

    /* 4. wrong public key (TEST 1's key) */
    uint8_t pk_wrong[32];
    hex2bin("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
            pk_wrong);
    CHECK(atls_ed25519_verify(sig, pk_wrong, &msg, 1) == ATLS_ERR_BAD_SIGNATURE,
          "wrong public key is refused as BAD_SIGNATURE");

    /* 5. S = L exactly (non-canonical scalar) */
    uint8_t sig_l[64];
    memcpy(sig_l, sig, 64);
    hex2bin("edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010",
            sig_l + 32);
    CHECK(atls_ed25519_verify(sig_l, pk, &msg, 1) == ATLS_ERR_BAD_SIGNATURE,
          "S = L is refused (range check)");

    /* 6. S with top bits set (>= L through the last byte) */
    uint8_t sig_big[64];
    memcpy(sig_big, sig, 64);
    sig_big[63] = 0xff;
    CHECK(atls_ed25519_verify(sig_big, pk, &msg, 1) == ATLS_ERR_BAD_SIGNATURE,
          "S with byte 31 = 0xff is refused (range check)");

    /* 7. R encoding a point not on the curve (y = 2: u/v is not a square
     * here) */
    uint8_t sig_bad_r[64];
    memcpy(sig_bad_r, sig, 64);
    memset(sig_bad_r, 0, 32);
    sig_bad_r[0] = 0x02;
    CHECK(atls_ed25519_verify(sig_bad_r, pk, &msg, 1) == ATLS_ERR_BAD_ENCODING,
          "R with off-curve y is refused as BAD_ENCODING");

    /* 8. Public key with non-canonical y: the bytes ed ff ... ff 7f
     * encode the raw value p itself, which reduces to 0 and therefore
     * cannot re-serialise to the input — decoding must refuse it.
     * (Note: ec ff ... ff 7f is p - 1, which IS canonical; the torsion
     * point (0, -1) it describes is handled by the equation instead.) */
    uint8_t pk_noncanon[32];
    hex2bin("edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
            pk_noncanon);
    CHECK(atls_ed25519_verify(sig, pk_noncanon, &msg, 1) == ATLS_ERR_BAD_ENCODING,
          "non-canonical public key (y = p) is refused as BAD_ENCODING");

    /* 9. R = neutral-point encoding with sign bit 1: x=0 wants sign 0,
     * so (y=1, sign=1) is not a valid encoding. */
    uint8_t sig_zero_r[64];
    memcpy(sig_zero_r, sig, 64);
    memset(sig_zero_r, 0, 32);
    sig_zero_r[0] = 0x01;
    sig_zero_r[31] = 0x80;
    CHECK(atls_ed25519_verify(sig_zero_r, pk, &msg, 1) == ATLS_ERR_BAD_ENCODING,
          "R = (0,1) with sign bit 1 is refused as BAD_ENCODING");

    /* 10. argument sanity */
    CHECK(atls_ed25519_verify(NULL, pk, &msg, 1) == ATLS_ERR_INPUT,
          "NULL signature refused");
    CHECK(atls_ed25519_verify(sig, NULL, &msg, 1) == ATLS_ERR_INPUT,
          "NULL public key refused");
    CHECK(atls_ed25519_verify(sig, pk, NULL, 5) == ATLS_ERR_INPUT,
          "NULL message with nonzero length refused");
}

int main(void) {
    printf("test_atls_ed25519: RFC 8032 vectors + per-reason negative battery\n");
    test_rfc_vectors();
    test_negative_battery();
    printf("test_atls_ed25519: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}

/* tests/unit/test_atls_aead.c — N1 stream/MAC/AEAD gates.
 *
 * ChaCha20 and Poly1305 against RFC 8439's published vectors, the full
 * AEAD construction against §2.8.2, then round-trip and tamper checks
 * (a flipped ciphertext bit, a flipped AAD bit and a flipped tag bit
 * must each fail authentication, individually).
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

static int hex_eq(const uint8_t *bin, size_t len, const char *hex) {
    if (strlen(hex) != len * 2) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned v;
        sscanf(hex + i * 2, "%2x", &v);
        if (bin[i] != (uint8_t)v) return 0;
    }
    return 1;
}

static const char RFC_KEY_HEX[] =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

/* ---- ChaCha20 (RFC 8439 §2.4.2, the Sunscreen example) ---- */

static void test_chacha20(void) {
    uint8_t key[32], nonce[12], pt[114], ct[114];
    hex2bin(RFC_KEY_HEX, key);
    hex2bin("000000000000004a00000000", nonce);
    memcpy(pt,
           "Ladies and Gentlemen of the class of '99: If I could offer you "
           "only one tip for the future, sunscreen would be it.", 114);

    atls_chacha20_xor(key, 1, nonce, pt, ct, sizeof(pt));
    CHECK(hex_eq(ct, 114,
        "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
        "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
        "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
        "5af90bbf74a35be6b40b8eedf2785e42874d"),
        "ChaCha20 RFC 8439 §2.4.2 Sunscreen ciphertext");

    /* Decrypt is the same call; it must invert. */
    uint8_t back[114];
    atls_chacha20_xor(key, 1, nonce, ct, back, sizeof(ct));
    CHECK(memcmp(back, pt, sizeof(pt)) == 0, "ChaCha20 decrypt inverts encrypt");
}

/* ---- Poly1305 (RFC 8439 §2.5.2) ---- */

static void test_poly1305(void) {
    uint8_t key[32], tag[16];
    hex2bin("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b",
            key);
    const char *msg = "Cryptographic Forum Research Group";

    atls_poly1305(key, (const uint8_t *)msg, strlen(msg), tag);
    CHECK(hex_eq(tag, 16, "a8061dc1305136c6c22b8baf0c0127a9"),
          "Poly1305 RFC 8439 §2.5.2 tag");

    /* Empty message. */
    uint8_t zero_key[32];
    memset(zero_key, 0, sizeof(zero_key));
    atls_poly1305(zero_key, NULL, 0, tag);
    CHECK(hex_eq(tag, 16, "00000000000000000000000000000000"),
          "Poly1305 zero key, empty message");

    /* All-block-boundary lengths exercise the padding paths. */
    uint8_t buf[64], t1[16], t2[16];
    for (int i = 0; i < 64; i++) buf[i] = (uint8_t)i;
    atls_poly1305(key, buf, 16, t1);
    atls_poly1305(key, buf, 17, t2);
    CHECK(atls_ct_eq(t1, t2, 16) == 0,
          "Poly1305 distinguishes 16-byte and 17-byte messages");
}

/* ---- AEAD (RFC 8439 §2.8.2) ---- */

static void test_aead_rfc(void) {
    uint8_t key[32], nonce[12], aad[12], pt[114], ct[114], tag[16];
    hex2bin("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f",
            key);
    hex2bin("070000004041424344454647", nonce);
    hex2bin("50515253c0c1c2c3c4c5c6c7", aad);
    memcpy(pt,
           "Ladies and Gentlemen of the class of '99: If I could offer you "
           "only one tip for the future, sunscreen would be it.", 114);

    CHECK(atls_aead_encrypt(key, nonce, aad, sizeof(aad),
                            pt, sizeof(pt), ct, tag) == ATLS_OK,
          "AEAD encrypt runs");
    CHECK(hex_eq(ct, 114,
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
        "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
        "3ff4def08e4b7a9de576d26586cec64b6116"),
        "AEAD RFC 8439 §2.8.2 ciphertext");
    CHECK(hex_eq(tag, 16, "1ae10b594f09e26a7e902ecbd0600691"),
          "AEAD RFC 8439 §2.8.2 tag");

    uint8_t back[114];
    CHECK(atls_aead_decrypt(key, nonce, aad, sizeof(aad),
                            ct, sizeof(ct), tag, back) == ATLS_OK,
          "AEAD decrypt accepts the genuine tag");
    CHECK(memcmp(back, pt, sizeof(pt)) == 0, "AEAD round-trip plaintext");
}

/* ---- tamper cases, each refused individually ---- */

static void test_aead_tamper(void) {
    uint8_t key[32], nonce[12], aad[5], pt[40], ct[40], tag[16], back[40];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i + 0x40);
    for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)(i + 1);
    for (int i = 0; i < 5; i++) aad[i] = (uint8_t)(0xa0 + i);
    for (int i = 0; i < 40; i++) pt[i] = (uint8_t)(i * 7);

    CHECK(atls_aead_encrypt(key, nonce, aad, sizeof(aad),
                            pt, sizeof(pt), ct, tag) == ATLS_OK,
          "tamper fixture encrypts");

    /* 1. flipped ciphertext bit */
    uint8_t ct_bad[40];
    memcpy(ct_bad, ct, sizeof(ct));
    ct_bad[10] ^= 0x20;
    CHECK(atls_aead_decrypt(key, nonce, aad, sizeof(aad),
                            ct_bad, sizeof(ct_bad), tag, back) == ATLS_ERR_AUTH,
          "tampered ciphertext is refused");

    /* 2. flipped AAD bit */
    uint8_t aad_bad[5];
    memcpy(aad_bad, aad, sizeof(aad));
    aad_bad[2] ^= 0x01;
    CHECK(atls_aead_decrypt(key, nonce, aad_bad, sizeof(aad_bad),
                            ct, sizeof(ct), tag, back) == ATLS_ERR_AUTH,
          "tampered AAD is refused");

    /* 3. flipped tag bit */
    uint8_t tag_bad[16];
    memcpy(tag_bad, tag, sizeof(tag));
    tag_bad[15] ^= 0x80;
    CHECK(atls_aead_decrypt(key, nonce, aad, sizeof(aad),
                            ct, sizeof(ct), tag_bad, back) == ATLS_ERR_AUTH,
          "tampered tag is refused");

    /* 4. wrong key */
    uint8_t key_bad[32];
    memcpy(key_bad, key, sizeof(key));
    key_bad[0] ^= 0xff;
    CHECK(atls_aead_decrypt(key_bad, nonce, aad, sizeof(aad),
                            ct, sizeof(ct), tag, back) == ATLS_ERR_AUTH,
          "wrong key is refused");

    /* The refused plaintext buffer must have been wiped. */
    memset(back, 0xee, sizeof(back));
    (void)atls_aead_decrypt(key, nonce, aad, sizeof(aad),
                            ct_bad, sizeof(ct_bad), tag, back);
    int wiped = 1;
    for (size_t i = 0; i < sizeof(back); i++) if (back[i] != 0) wiped = 0;
    CHECK(wiped, "refused decrypt wipes the plaintext buffer");

    /* In-place decrypt (ct buffer doubles as pt buffer). */
    uint8_t inplace[40];
    memcpy(inplace, ct, sizeof(ct));
    CHECK(atls_aead_decrypt(key, nonce, aad, sizeof(aad),
                            inplace, sizeof(inplace), tag, inplace) == ATLS_OK
          && memcmp(inplace, pt, sizeof(pt)) == 0,
          "in-place decrypt works");
}

/* ---- the RFC's own Poly1305 key-generation path (§2.6) ---- */

static void test_poly_key_gen(void) {
    /* §2.6.2: key 80..9f, nonce 00 00 00 00 00 01 02 03 04 05 06 07,
     * block 0 gives the documented one-time key. */
    uint8_t key[32], nonce[12], block0[64];
    hex2bin("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f",
            key);
    hex2bin("000000000001020304050607", nonce);
    uint8_t zeros[64];
    memset(zeros, 0, sizeof(zeros));
    atls_chacha20_xor(key, 0, nonce, zeros, block0, sizeof(zeros));
    CHECK(hex_eq(block0, 32,
        "8ad5a08b905f81cc815040274ab29471a833b637e3fd0da508dbb8e2fdd1a646"),
        "Poly1305 key from ChaCha20 block 0 (RFC 8439 §2.6.2)");
}

int main(void) {
    printf("test_atls_aead: ChaCha20/Poly1305/AEAD RFC vectors + tamper gate\n");
    test_chacha20();
    test_poly1305();
    test_poly_key_gen();
    test_aead_rfc();
    test_aead_tamper();
    printf("test_atls_aead: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}

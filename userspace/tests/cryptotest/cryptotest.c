/* cryptotest — in-guest smoke test for libatls (INTERNET_PLAN.md N1).
 *
 * The full RFC-vector batteries run host-side (tests/unit/test_atls_*),
 * compiled from the identical sources.  This program proves the library
 * also runs INSIDE the OS: same 64 KiB user stack, the guest toolchain,
 * no host libc.  It exercises one vector per primitive plus the negative
 * paths that matter for a handshake (tampered AEAD, low-order X25519,
 * forged Ed25519 signature), and prints machine-checkable PASS lines.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "atls/atls.h"

static int failures = 0;

static void hex2bin(const char *h, uint8_t *o) {
    size_t n = strlen(h);
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        char tmp[3] = { h[i * 2], h[i * 2 + 1], 0 };
        sscanf(tmp, "%2x", &v);
        o[i] = (uint8_t)v;
    }
}

static int hex_eq(const uint8_t *bin, size_t len, const char *hex) {
    if (strlen(hex) != len * 2) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned v;
        char tmp[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        sscanf(tmp, "%2x", &v);
        if (bin[i] != (uint8_t)v) return 0;
    }
    return 1;
}

#define CHECK(cond, name)                                    \
    do {                                                     \
        if (cond) {                                          \
            printf("[cryptotest] PASS: %s\n", name);         \
        } else {                                             \
            failures++;                                      \
            printf("[cryptotest] FAIL: %s\n", name);         \
        }                                                    \
    } while (0)

static void test_sha(void) {
    uint8_t out[32];
    atls_sha256("abc", 3, out);
    CHECK(hex_eq(out, 32,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        "SHA-256(\"abc\")");
}

static void test_hmac_hkdf(void) {
    uint8_t key[20], prk[32], okm[42];
    memset(key, 0x0b, sizeof(key));
    uint8_t mac[32];
    atls_hmac_sha256(key, 20, "Hi There", 8, mac);
    CHECK(hex_eq(mac, 32,
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
        "HMAC-SHA256 RFC 4231 TC1");

    uint8_t salt[13], info[10], ikm[22];
    hex2bin("000102030405060708090a0b0c", salt);
    hex2bin("f0f1f2f3f4f5f6f7f8f9", info);
    memset(ikm, 0x0b, sizeof(ikm));
    CHECK(atls_hkdf_extract(salt, 13, ikm, 22, prk) == ATLS_OK &&
          hex_eq(prk, 32,
              "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"),
          "HKDF RFC 5869 TC1 extract");
    CHECK(atls_hkdf_expand(prk, info, 10, okm, 42) == ATLS_OK &&
          hex_eq(okm, 42,
              "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
              "34007208d5b887185865"),
          "HKDF RFC 5869 TC1 expand");
}

static void test_aead(void) {
    uint8_t key[32], nonce[12], aad[12], pt[114], ct[114], tag[16], back[114];
    hex2bin("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key);
    hex2bin("070000004041424344454647", nonce);
    hex2bin("50515253c0c1c2c3c4c5c6c7", aad);
    memcpy(pt,
           "Ladies and Gentlemen of the class of '99: If I could offer you "
           "only one tip for the future, sunscreen would be it.", 114);

    CHECK(atls_aead_encrypt(key, nonce, aad, sizeof(aad), pt, sizeof(pt),
                            ct, tag) == ATLS_OK &&
          hex_eq(tag, 16, "1ae10b594f09e26a7e902ecbd0600691"),
          "AEAD RFC 8439 §2.8.2 tag");
    CHECK(atls_aead_decrypt(key, nonce, aad, sizeof(aad), ct, sizeof(ct),
                            tag, back) == ATLS_OK &&
          memcmp(back, pt, sizeof(pt)) == 0,
          "AEAD round trip");

    uint8_t bad_tag[16];
    memcpy(bad_tag, tag, sizeof(bad_tag));
    bad_tag[0] ^= 1;
    CHECK(atls_aead_decrypt(key, nonce, aad, sizeof(aad), ct, sizeof(ct),
                            bad_tag, back) == ATLS_ERR_AUTH,
          "AEAD tampered tag refused");
}

static void test_x25519(void) {
    uint8_t a_priv[32], b_pub[32], shared[32];
    hex2bin("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",
            a_priv);
    hex2bin("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
            b_pub);
    CHECK(atls_x25519(shared, a_priv, b_pub) == ATLS_OK &&
          hex_eq(shared, 32,
              "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"),
          "X25519 RFC 7748 §5.2 shared secret");

    /* Low-order point u = 1 must be refused. */
    uint8_t one[32], out[32];
    memset(one, 0, sizeof(one));
    one[0] = 1;
    memset(out, 0xaa, sizeof(out));
    CHECK(atls_x25519(out, a_priv, one) == ATLS_ERR_LOW_ORDER,
          "X25519 low-order point refused");
}

static void test_ed25519(void) {
    uint8_t pk[32], sig[64];
    hex2bin("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", pk);
    hex2bin("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
            "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00", sig);
    uint8_t msg = 0x72;
    CHECK(atls_ed25519_verify(sig, pk, &msg, 1) == ATLS_OK,
          "Ed25519 RFC 8032 §7.1 TEST 2 verifies");

    uint8_t forged[64];
    memcpy(forged, sig, sizeof(forged));
    forged[40] ^= 0x08;
    CHECK(atls_ed25519_verify(forged, pk, &msg, 1) == ATLS_ERR_BAD_SIGNATURE,
          "Ed25519 forged signature refused");
}

int main(void) {
    printf("[cryptotest] libatls in-guest smoke test (INTERNET_PLAN N1)\n");
    test_sha();
    test_hmac_hkdf();
    test_aead();
    test_x25519();
    test_ed25519();
    if (failures == 0) {
        printf("[cryptotest] ALL PASS\n");
        return 0;
    }
    printf("[cryptotest] %d FAILURES\n", failures);
    return 1;
}

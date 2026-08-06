/* tests/unit/test_atls_x25519.c — N1 X25519 gates.
 *
 *   * RFC 7748 §5.2 Alice/Bob vector (shared secret both ways).
 *   * RFC 7748 §6.1 iterated test, 1 iteration AND 1000 iterations —
 *     the plan's explicit gate.
 *   * Wycheproof small/low-order public keys: every one must produce the
 *     all-zero shared secret AND be refused with ATLS_ERR_LOW_ORDER
 *     (ZeroSharedSecret flag).  A handshake that accepts a zero shared
 *     secret hands every passive observer the same session keys.
 *
 * The Wycheproof points are tcId 32, 33, 63-68 from x25519_test.json
 * (vectors_v1), including the NonCanonicalPublic encodings with bit 255
 * set — the RFC's decode masks that bit, so they land on the same
 * low-order points.
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

/* ---- RFC 7748 §5.2: Alice and Bob ---- */

static void test_rfc_alice_bob(void) {
    uint8_t a_priv[32], b_priv[32], a_pub[32], b_pub[32], s1[32], s2[32];

    hex2bin("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",
            a_priv);
    hex2bin("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb",
            b_priv);

    CHECK(atls_x25519(a_pub, a_priv, ATLS_X25519_BASEPOINT) == ATLS_OK,
          "Alice public key computation succeeds");
    CHECK(hex_eq(a_pub, 32,
        "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"),
        "Alice public key matches RFC 7748 §5.2");

    CHECK(atls_x25519(b_pub, b_priv, ATLS_X25519_BASEPOINT) == ATLS_OK,
          "Bob public key computation succeeds");
    CHECK(hex_eq(b_pub, 32,
        "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"),
        "Bob public key matches RFC 7748 §5.2");

    CHECK(atls_x25519(s1, a_priv, b_pub) == ATLS_OK, "Alice->Bob succeeds");
    CHECK(atls_x25519(s2, b_priv, a_pub) == ATLS_OK, "Bob->Alice succeeds");
    CHECK(atls_ct_eq(s1, s2, 32) == 1, "both directions agree");
    CHECK(hex_eq(s1, 32,
        "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"),
        "shared secret matches RFC 7748 §5.2");
}

/* ---- RFC 7748 §6.1: iterated ---- */

static void test_rfc_iterated(void) {
    uint8_t k[32], u[32], tmp[32];
    memset(k, 0, 32); k[0] = 9;
    memset(u, 0, 32); u[0] = 9;

    for (int i = 0; i < 1; i++) {
        CHECK(atls_x25519(tmp, k, u) == ATLS_OK, "iterated step runs");
        memcpy(u, k, 32);
        memcpy(k, tmp, 32);
    }
    CHECK(hex_eq(k, 32,
        "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"),
        "RFC 7748 §6.1 after 1 iteration");

    for (int i = 1; i < 1000; i++) {
        if (atls_x25519(tmp, k, u) != ATLS_OK) {
            CHECK(0, "iterated step failed mid-run");
            return;
        }
        memcpy(u, k, 32);
        memcpy(k, tmp, 32);
    }
    CHECK(hex_eq(k, 32,
        "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51"),
        "RFC 7748 §6.1 after 1000 iterations (the plan's gate)");
}

/* ---- Wycheproof low-order points ---- */

static void test_low_order(void) {
    /* (tcId, private, public, label) — the exact Wycheproof triples,
     * because a couple of these points have ODD small order (e.g. 5), so
     * the all-zero shared secret only appears under the matching scalar.
     * Every one carries the ZeroSharedSecret flag and result
     * "acceptable", i.e. a strict implementation refuses them. */
    static const struct {
        int tcid;
        const char *priv;
        const char *u;
        const char *what;
    } cases[] = {
        { 32, "88227494038f2bb811d47805bcdf04a2ac585ada7f2f23389bfd4658f9ddd45e",
              "0000000000000000000000000000000000000000000000000000000000000000",
              "u = 0 (identity)" },
        { 33, "48232e8972b61c7e61930eb9450b5070eae1c670475685541f0476217e48184f",
              "0100000000000000000000000000000000000000000000000000000000000000",
              "u = 1 (order 2)" },
        { 63, "e0f978dfcd3a8f1a5093418de54136a584c20b7b349afdf6c0520886f95b1272",
              "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
              "low order #1" },
        { 64, "387355d995616090503aafad49da01fb3dc3eda962704eaee6b86f9e20c92579",
              "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
              "low order #2" },
        { 65, "c8fe0df92ae68a03023fc0c9adb9557d31be7feed0d3ab36c558143daf4dbb40",
              "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
              "low order #3 (twist)" },
        { 66, "c8d74acde5934e64b9895d5ff7afbffd7f704f7dfccff7ac28fa62a1e6410347",
              "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b880",
              "low order #1, non-canonical (bit 255 set)" },
        { 67, "b85649d5120e01e8ccaf7b2fb8d81b62e8ad6f3d5c0553fdde1906cb9d79c050",
              "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f11d7",
              "low order #2, non-canonical (bit 255 set)" },
        { 68, "2064b2f4c9dc97ec7cf58932fdfa3265ba6ea4d11f0259b8efc8afb35db88c48",
              "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
              "low order #3, non-canonical (bit 255 set)" },
        { 74, "786a33a4f7af297a20e7642925932bf509e7070fa1bc36986af1eb13f4f50b55",
              "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
              "u = p (decodes to 0)" },
        { 75, "786a33a4f7af297a20e7642925932bf509e7070fa1bc36986af1eb13f4f50b55",
              "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
              "odd-order point" },
    };

    uint8_t scalar[32], u[32], out[32];

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hex2bin(cases[i].priv, scalar);
        hex2bin(cases[i].u, u);
        int rc = atls_x25519(out, scalar, u);
        char msg[160];
        int zero = 1;
        for (int j = 0; j < 32; j++) if (out[j]) zero = 0;
        snprintf(msg, sizeof(msg),
                 "Wycheproof tcId %d (%s): zero secret, refused as low-order",
                 cases[i].tcid, cases[i].what);
        CHECK(rc == ATLS_ERR_LOW_ORDER && zero, msg);
    }
}

/* ---- input validation and determinism ---- */

static void test_misc(void) {
    uint8_t scalar[32], u[32], a[32], b[32];
    for (int i = 0; i < 32; i++) { scalar[i] = (uint8_t)(i + 5); u[i] = (uint8_t)(i * 11 + 2); }

    int rc1 = atls_x25519(a, scalar, u);
    int rc2 = atls_x25519(b, scalar, u);
    CHECK(rc1 == ATLS_OK || rc1 == ATLS_ERR_LOW_ORDER, "random-ish point runs");
    CHECK(rc2 == rc1, "same inputs repeat");
    CHECK(rc1 != ATLS_OK || atls_ct_eq(a, b, 32) == 1, "X25519 is deterministic");

    CHECK(atls_x25519(NULL, scalar, u) == ATLS_ERR_INPUT, "NULL out refused");
    CHECK(atls_x25519(a, NULL, u) == ATLS_ERR_INPUT, "NULL scalar refused");
    CHECK(atls_x25519(a, scalar, NULL) == ATLS_ERR_INPUT, "NULL point refused");
}

int main(void) {
    printf("test_atls_x25519: RFC 7748 vectors + Wycheproof low-order gate\n");
    test_rfc_alice_bob();
    test_rfc_iterated();
    test_low_order();
    test_misc();
    printf("test_atls_x25519: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}

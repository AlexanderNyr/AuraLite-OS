/* tests/unit/test_atls_hash.c — N1 hash/MAC/KDF gates.
 *
 * Links the REAL libatls sources (the tree's "never test a copy" rule).
 * Vectors are the published ones — RFC 6234 (SHA), RFC 4231 (HMAC),
 * RFC 5869 (HKDF) — because "it produces consistent output" proves
 * nothing (INTERNET_PLAN.md N1 test gate).
 *
 * Also enforces decision D7: no memcmp/strncmp/bcmp/strcmp anywhere in
 * lib/libatls/src — the sources are grepped by THIS test, right here.
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

/* ---- SHA-256 (FIPS 180 / RFC 6234 vectors) ---- */

static void test_sha256(void) {
    uint8_t out[32];

    atls_sha256("", 0, out);
    CHECK(hex_eq(out, 32,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
        "SHA-256(\"\")");

    atls_sha256("abc", 3, out);
    CHECK(hex_eq(out, 32,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        "SHA-256(\"abc\")");

    atls_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
    CHECK(hex_eq(out, 32,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
        "SHA-256 two-block NIST vector");

    /* The million-'a' vector, streamed one chunk at a time. */
    atls_sha256_ctx c;
    atls_sha256_init(&c);
    for (int i = 0; i < 1000000 / 1000; i++) {
        char buf[1000];
        memset(buf, 'a', sizeof(buf));
        atls_sha256_update(&c, buf, sizeof(buf));
    }
    atls_sha256_final(&c, out);
    CHECK(hex_eq(out, 32,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
        "SHA-256 of 1,000,000 x 'a' (streamed)");
}

/* ---- SHA-512 ---- */

static void test_sha512(void) {
    uint8_t out[64];

    atls_sha512("", 0, out);
    CHECK(hex_eq(out, 64,
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"),
        "SHA-512(\"\")");

    atls_sha512("abc", 3, out);
    CHECK(hex_eq(out, 64,
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"),
        "SHA-512(\"abc\")");

    atls_sha512("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 112, out);
    CHECK(hex_eq(out, 64,
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
        "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"),
        "SHA-512 896-bit two-block vector");
}

/* ---- HMAC-SHA256 (RFC 4231) ---- */

static void hmac_tc(const char *name, const uint8_t *key, size_t keylen,
                    const uint8_t *data, size_t datalen,
                    const char *expect, size_t expect_len) {
    uint8_t out[32];
    atls_hmac_sha256(key, keylen, data, datalen, out);
    CHECK(hex_eq(out, expect_len, expect), name);
}

static void test_hmac_sha256(void) {
    uint8_t key[131], data[150];

    memset(key, 0x0b, 20);
    hmac_tc("RFC 4231 TC1", key, 20, (const uint8_t *)"Hi There", 8,
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32);

    hmac_tc("RFC 4231 TC2", (const uint8_t *)"Jefe", 4,
            (const uint8_t *)"what do ya want for nothing?", 28,
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32);

    memset(key, 0xaa, 20);
    memset(data, 0xdd, 50);
    hmac_tc("RFC 4231 TC3", key, 20, data, 50,
            "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", 32);

    for (int i = 0; i < 25; i++) key[i] = (uint8_t)(i + 1);
    memset(data, 0xcd, 50);
    hmac_tc("RFC 4231 TC4", key, 25, data, 50,
            "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b", 32);

    memset(key, 0x0c, 20);
    hmac_tc("RFC 4231 TC5 (truncated to 16)", key, 20,
            (const uint8_t *)"Test With Truncation", 20,
            "a3b6167473100ee06e0c796c2955552b", 16);

    memset(key, 0xaa, 131);
    hmac_tc("RFC 4231 TC6 (key > block size)", key, 131,
            (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First", 54,
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", 32);

    hmac_tc("RFC 4231 TC7 (key and data > block size)", key, 131,
            (const uint8_t *)"This is a test using a larger than block-size key and a "
                              "larger than block-size data. The key needs to be hashed "
                              "before being used by the HMAC algorithm.", 152,
            "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2", 32);
}

/* ---- HKDF (RFC 5869) ---- */

static void test_hkdf(void) {
    uint8_t ikm[80], salt[80], info[80], prk[32], okm[82];

    /* TC1 */
    memset(ikm, 0x0b, 22);
    hex2bin("000102030405060708090a0b0c", salt);
    hex2bin("f0f1f2f3f4f5f6f7f8f9", info);
    CHECK(atls_hkdf_extract(salt, 13, ikm, 22, prk) == ATLS_OK, "HKDF TC1 extract runs");
    CHECK(hex_eq(prk, 32,
        "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"),
        "HKDF TC1 PRK");
    CHECK(atls_hkdf_expand(prk, info, 10, okm, 42) == ATLS_OK, "HKDF TC1 expand runs");
    CHECK(hex_eq(okm, 42,
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
        "34007208d5b887185865"),
        "HKDF TC1 OKM");

    /* TC2 (long inputs) */
    for (int i = 0; i < 80; i++) { ikm[i] = (uint8_t)i; salt[i] = (uint8_t)(0x60 + i); info[i] = (uint8_t)(0xb0 + i); }
    CHECK(atls_hkdf_extract(salt, 80, ikm, 80, prk) == ATLS_OK, "HKDF TC2 extract runs");
    CHECK(hex_eq(prk, 32,
        "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244"),
        "HKDF TC2 PRK");
    CHECK(atls_hkdf_expand(prk, info, 80, okm, 82) == ATLS_OK, "HKDF TC2 expand runs");
    CHECK(hex_eq(okm, 82,
        "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
        "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
        "cc30c58179ec3e87c14c01d5c1f3434f1d87"),
        "HKDF TC2 OKM (82 bytes, three T-blocks)");

    /* TC3 (zero-length salt and info) */
    memset(ikm, 0x0b, 22);
    CHECK(atls_hkdf_extract(NULL, 0, ikm, 22, prk) == ATLS_OK, "HKDF TC3 extract runs");
    CHECK(hex_eq(prk, 32,
        "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04"),
        "HKDF TC3 PRK (salt defaults to zeros)");
    CHECK(atls_hkdf_expand(prk, NULL, 0, okm, 42) == ATLS_OK, "HKDF TC3 expand runs");
    CHECK(hex_eq(okm, 42,
        "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
        "9d201395faa4b61a96c8"),
        "HKDF TC3 OKM (empty info)");

    /* Over-long output is refused (RFC 5869: L <= 255*HashLen). */
    CHECK(atls_hkdf_expand(prk, NULL, 0, okm, 255 * 32 + 1) == ATLS_ERR_INPUT,
          "HKDF refuses L > 255*HashLen");
}

/* ---- atls_ct_eq behaviour ---- */

static void test_ct_eq(void) {
    uint8_t a[64], b[64];
    for (int i = 0; i < 64; i++) { a[i] = (uint8_t)(i * 3); b[i] = (uint8_t)(i * 3); }
    CHECK(atls_ct_eq(a, b, 64) == 1, "ct_eq: equal buffers compare equal");
    CHECK(atls_ct_eq(a, b, 0) == 1, "ct_eq: zero length is equal");
    b[63] ^= 1;
    CHECK(atls_ct_eq(a, b, 64) == 0, "ct_eq: last-byte difference detected");
    b[63] ^= 1;
    b[0] ^= 0x80;
    CHECK(atls_ct_eq(a, b, 64) == 0, "ct_eq: first-byte difference detected");
}

/* ---- D7: no memcmp on anything in libatls ---- */

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    /* Whole-file scan: atls_mlkem.c is >8 KiB, so a cap would miss
     * a banned token past the first page (Y5 catch, named). */
    int hit = 0;
    size_t nlen = strlen(needle);
    char buf[4096];
    size_t keep = 0;
    for (;;) {
        size_t n = fread(buf + keep, 1, sizeof(buf) - 1 - keep, f);
        size_t total = keep + n;
        buf[total] = 0;
        if (strstr(buf, needle)) { hit = 1; break; }
        if (n == 0) break;
        if (total > nlen) {
            memmove(buf, buf + total - nlen, nlen);
            keep = nlen;
        } else {
            keep = total;
        }
    }
    fclose(f);
    return hit;
}

static void test_d7_no_memcmp(void) {
    /* The audited translation units.  Keep in sync with LIBATLS_OBJS. */
    static const char *srcs[] = {
        "lib/libatls/src/atls_common.c",
        "lib/libatls/src/atls_sha256.c",
        "lib/libatls/src/atls_sha512.c",
        "lib/libatls/src/atls_hmac.c",
        "lib/libatls/src/atls_hkdf.c",
        "lib/libatls/src/atls_chacha20.c",
        "lib/libatls/src/atls_poly1305.c",
        "lib/libatls/src/atls_aead.c",
        "lib/libatls/src/atls_fe.c",
        "lib/libatls/src/atls_x25519.c",
        "lib/libatls/src/atls_ed25519.c",
        "lib/libatls/src/atls_sha3.c",
        "lib/libatls/src/atls_mlkem.c",
    };
    static const char *banned[] = {
        "memcmp(", "strncmp(", "bcmp(", "strcmp(",
    };
    int clean = 1;
    for (size_t i = 0; i < sizeof(srcs) / sizeof(srcs[0]); i++) {
        int present = file_contains(srcs[i], "atls_ct_eq");
        (void)present;
        for (size_t j = 0; j < sizeof(banned) / sizeof(banned[0]); j++) {
            int hit = file_contains(srcs[i], banned[j]);
            if (hit != 0) {
                if (hit > 0) {
                    printf("      %s contains %s\n", srcs[i], banned[j]);
                } else {
                    printf("      cannot open %s (run from repo root)\n", srcs[i]);
                }
                clean = 0;
            }
        }
    }
    CHECK(clean, "D7: no memcmp/strncmp/bcmp/strcmp in any libatls source");
}

int main(void) {
    printf("test_atls_hash: SHA/HMAC/HKDF RFC vectors + D7 audit\n");
    test_sha256();
    test_sha512();
    test_hmac_sha256();
    test_hkdf();
    test_ct_eq();
    test_d7_no_memcmp();
    printf("test_atls_hash: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}

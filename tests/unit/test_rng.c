/* tests/unit/test_rng.c — host-side tests for the N0 CSPRNG core.
 *
 * Links the SAME kernel/rng_core.h the guest kernel uses (the tree's
 * pure-core pattern), so these bytes are the guest's bytes.
 *
 * What is checked (INTERNET_PLAN.md N0 test gate):
 *
 *   1. The ChaCha20 block function against the RFC 8439 §2.3.2 vector.
 *   2. The keystream against RFC 8439 §2.4.2 (the "Sunscreen" example,
 *      both keystream blocks AND the full 114-byte ciphertext).
 *   3. DRBG determinism: identical seed material -> identical stream.
 *   4. Two different seeds -> different output (the "two boots differ"
 *      property, proven at the core level; the integration test proves it
 *      across real QEMU boots).
 *   5. Reseeding changes the stream.
 *   6. Statistical smoke over 1 MiB: per-byte frequency and bit-run
 *      counts within wide-but-meaningful bounds.  Catches stuck or
 *      counter-like generators; not a claim of cryptographic quality.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>   /* before rng_core.h: the core itself stays freestanding */
#include <stdint.h>

#include "kernel/rng_core.h"

static int pass_count = 0, fail_count = 0;

#define CHECK(cond, msg)                                          \
    do {                                                          \
        if (cond) {                                               \
            pass_count++;                                         \
            printf("PASS: %s\n", msg);                            \
        } else {                                                  \
            fail_count++;                                         \
            printf("FAIL: %s  (line %d)\n", msg, __LINE__);       \
        }                                                         \
    } while (0)

/* ---- RFC 8439 test-vector material ---- */

static const uint8_t rfc_key_bytes[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

/* §2.3.2: Nonce 00:00:00:09 00:00:00:4a 00:00:00:00, counter 1 */
static const uint32_t rfc232_nonce[3] = { 0x09000000u, 0x4a000000u, 0x00000000u };
static const uint32_t rfc232_expected[16] = {
    0xe4e7f110u, 0x15593bd1u, 0x1fdd0f50u, 0xc47120a3u,
    0xc7f4d1c7u, 0x0368c033u, 0x9aaa2204u, 0x4e6cd4c3u,
    0x466482d2u, 0x09aa9f07u, 0x05d7c214u, 0xa2028bd9u,
    0xd19c12b5u, 0xb94e16deu, 0xe883d0cbu, 0x4e3c50a2u,
};
static const uint8_t rfc232_serialized[64] = {
    0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
    0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
    0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03,
    0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
    0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09,
    0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
    0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9,
    0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e,
};

/* §2.4.2: Nonce 00:00:00:00 00:00:00:4a 00:00:00:00, initial counter 1 */
static const uint32_t rfc242_nonce[3] = { 0x00000000u, 0x4a000000u, 0x00000000u };
static const uint32_t rfc242_block1[16] = {
    0xf3514f22u, 0xe1d91b40u, 0x6f27de2fu, 0xed1d63b8u,
    0x821f138cu, 0xe2062c3du, 0xecca4f7eu, 0x78cff39eu,
    0xa30a3b8au, 0x920a6072u, 0xcd7479b5u, 0x34932bedu,
    0x40ba4c79u, 0xcd343ec6u, 0x4c2c21eau, 0xb7417df0u,
};
static const uint32_t rfc242_block2[16] = {
    0x9f74a669u, 0x410f633fu, 0x28feca22u, 0x7ec44decu,
    0x6d34d426u, 0x738cb970u, 0x3ac5e9f3u, 0x45590cc4u,
    0xda6e8b39u, 0x892c831au, 0xcdea67c1u, 0x2b7e1d90u,
    0x037463f3u, 0xa11a2073u, 0xe8bcfb88u, 0xedc49139u,
};

static const char rfc242_plaintext[] =
    "Ladies and Gentlemen of the class of '99: If I could offer you only "
    "one tip for the future, sunscreen would be it.";

static const uint8_t rfc242_ciphertext[114] = {
    0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80,
    0x41, 0xba, 0x07, 0x28, 0xdd, 0x0d, 0x69, 0x81,
    0xe9, 0x7e, 0x7a, 0xec, 0x1d, 0x43, 0x60, 0xc2,
    0x0a, 0x27, 0xaf, 0xcc, 0xfd, 0x9f, 0xae, 0x0b,
    0xf9, 0x1b, 0x65, 0xc5, 0x52, 0x47, 0x33, 0xab,
    0x8f, 0x59, 0x3d, 0xab, 0xcd, 0x62, 0xb3, 0x57,
    0x16, 0x39, 0xd6, 0x24, 0xe6, 0x51, 0x52, 0xab,
    0x8f, 0x53, 0x0c, 0x35, 0x9f, 0x08, 0x61, 0xd8,
    0x07, 0xca, 0x0d, 0xbf, 0x50, 0x0d, 0x6a, 0x61,
    0x56, 0xa3, 0x8e, 0x08, 0x8a, 0x22, 0xb6, 0x5e,
    0x52, 0xbc, 0x51, 0x4d, 0x16, 0xcc, 0xf8, 0x06,
    0x81, 0x8c, 0xe9, 0x1a, 0xb7, 0x79, 0x37, 0x36,
    0x5a, 0xf9, 0x0b, 0xbf, 0x74, 0xa3, 0x5b, 0xe6,
    0xb4, 0x0b, 0x8e, 0xed, 0xf2, 0x78, 0x5e, 0x42,
    0x87, 0x4d,
};

static void load_rfc_key(uint32_t key[8]) {
    for (int i = 0; i < 8; i++) key[i] = rngc_le32(rfc_key_bytes + i * 4);
}

/* ---- 1. RFC 8439 §2.3.2: block function ---- */

static void test_rfc232_block(void) {
    uint32_t key[8], out[16];
    load_rfc_key(key);
    rngc_chacha20_block(key, 1, rfc232_nonce, out);
    CHECK(memcmp(out, rfc232_expected, sizeof(out)) == 0,
          "RFC 8439 §2.3.2 block function vector");

    uint8_t bytes[64];
    rngc_chacha20_block_bytes(key, 1, rfc232_nonce, bytes);
    CHECK(memcmp(bytes, rfc232_serialized, 64) == 0,
          "RFC 8439 §2.3.2 little-endian serialization");
}

/* ---- 2. RFC 8439 §2.4.2: keystream and Sunscreen encryption ---- */

static void test_rfc242_cipher(void) {
    uint32_t key[8], out[16];
    load_rfc_key(key);

    rngc_chacha20_block(key, 1, rfc242_nonce, out);
    CHECK(memcmp(out, rfc242_block1, sizeof(out)) == 0,
          "RFC 8439 §2.4.2 first keystream block");

    rngc_chacha20_block(key, 2, rfc242_nonce, out);
    CHECK(memcmp(out, rfc242_block2, sizeof(out)) == 0,
          "RFC 8439 §2.4.2 second keystream block");

    /* Full 114-byte encryption: generate keystream bytes counter=1,2 and
     * XOR.  Exercises the byte-order path the DRBG uses. */
    uint8_t ks[128];
    rngc_chacha20_block_bytes(key, 1, rfc242_nonce, ks);
    rngc_chacha20_block_bytes(key, 2, rfc242_nonce, ks + 64);

    size_t pt_len = strlen(rfc242_plaintext);
    CHECK(pt_len == 114, "Sunscreen plaintext is 114 bytes");

    uint8_t ct[114];
    for (size_t i = 0; i < pt_len; i++) {
        ct[i] = (uint8_t)((uint8_t)rfc242_plaintext[i] ^ ks[i]);
    }
    CHECK(memcmp(ct, rfc242_ciphertext, 114) == 0,
          "RFC 8439 §2.4.2 Sunscreen ciphertext (114 bytes)");
}

/* ---- helpers for DRBG tests ---- */

static void make_material(uint8_t m[RNGC_SEED_LEN], uint8_t seed_byte) {
    for (int i = 0; i < RNGC_SEED_LEN; i++) {
        m[i] = (uint8_t)(seed_byte + i * 31 + (i / 7) * 17);
    }
}

/* ---- 3/4. determinism and seed sensitivity ---- */

static void test_drbg_determinism(void) {
    uint8_t m1[RNGC_SEED_LEN], m2[RNGC_SEED_LEN];
    uint8_t a[1024], b[1024];

    make_material(m1, 0x11);
    make_material(m2, 0x11);          /* identical */
    struct rngc_drbg d1, d2;
    rngc_drbg_seed(&d1, m1);
    rngc_drbg_seed(&d2, m2);
    rngc_drbg_fill(&d1, a, sizeof(a));
    rngc_drbg_fill(&d2, b, sizeof(b));
    CHECK(memcmp(a, b, sizeof(a)) == 0,
          "same seed material -> identical 1 KiB stream");

    make_material(m2, 0x12);          /* one byte's recipe differs */
    rngc_drbg_seed(&d2, m2);
    rngc_drbg_fill(&d2, b, sizeof(b));
    CHECK(memcmp(a, b, sizeof(a)) != 0,
          "different seed material -> different stream");

    /* Not just "different somewhere": overwhelmingly different.  A good
     * stream cipher flips ~half the bits under a new key. */
    unsigned differing = 0;
    for (size_t i = 0; i < sizeof(a); i++) {
        if (a[i] != b[i]) differing++;
    }
    CHECK(differing > sizeof(a) * 3 / 4,
          "different seeds differ in >75% of bytes (avalanche)");
}

/* ---- 5. reseed changes the stream ---- */

static void test_drbg_reseed(void) {
    uint8_t m1[RNGC_SEED_LEN], m2[RNGC_SEED_LEN];
    uint8_t a[256], b[256], c[256];
    struct rngc_drbg d1, d2;

    make_material(m1, 0x42);
    make_material(m2, 0x99);
    rngc_drbg_seed(&d1, m1);
    rngc_drbg_seed(&d2, m1);

    rngc_drbg_fill(&d1, a, sizeof(a));
    rngc_drbg_fill(&d2, b, sizeof(b));
    CHECK(memcmp(a, b, sizeof(a)) == 0, "pre-reseed streams agree");

    rngc_drbg_reseed(&d2, m2);
    rngc_drbg_fill(&d2, c, sizeof(c));
    CHECK(memcmp(b, c, sizeof(c)) != 0,
          "reseed with fresh material changes the stream");

    /* A reseeded DRBG must not coincide with a never-seeded-with-m2 one's
     * FIRST request either (XOR-fold lands in a different place than a
     * direct seed would). */
    struct rngc_drbg d3;
    rngc_drbg_seed(&d3, m2);
    uint8_t e[256];
    rngc_drbg_fill(&d3, e, sizeof(e));
    CHECK(memcmp(c, e, sizeof(e)) != 0,
          "reseeded state != directly-seeded state");
}

/* ---- 6. statistical smoke over 1 MiB ---- */

static void test_statistical_1mb(void) {
    enum { N = 1024 * 1024 };
    uint8_t *buf = malloc(N);
    if (!buf) {
        printf("FAIL: could not allocate 1 MiB for statistics\n");
        fail_count++;
        return;
    }

    uint8_t m[RNGC_SEED_LEN];
    make_material(m, 0x5A);
    struct rngc_drbg d;
    rngc_drbg_seed(&d, m);

    /* Fill in several requests so the backtracking re-key path runs too. */
    size_t off = 0;
    while (off < (size_t)N) {
        size_t chunk = 4096;
        if (off + chunk > (size_t)N) chunk = (size_t)N - off;
        rngc_drbg_fill(&d, buf + off, chunk);
        off += chunk;
    }

    /* Byte frequency: expected 4096 per value; allow +-15% (~9.6 sigma). */
    static uint32_t freq[256];
    memset(freq, 0, sizeof(freq));
    for (size_t i = 0; i < (size_t)N; i++) freq[buf[i]]++;
    uint32_t expected = N / 256;
    int freq_ok = 1;
    uint32_t lo = expected, hi = expected;
    for (int i = 0; i < 256; i++) {
        if (freq[i] < lo) lo = freq[i];
        if (freq[i] > hi) hi = freq[i];
        if (freq[i] < expected - expected * 15 / 100 ||
            freq[i] > expected + expected * 15 / 100) {
            freq_ok = 0;
        }
    }
    CHECK(freq_ok, "1 MiB byte frequency within +-15% per value");
    printf("      (min count %u, max count %u, expected %u)\n", lo, hi, expected);

    /* Bit runs: expected ~N*4 runs; allow +-2% (~58 sigma). */
    uint64_t nbits = (uint64_t)N * 8;
    uint64_t runs = 1, longest = 1, cur = 1;
    int prev = buf[0] & 1;
    for (size_t i = 0; i < (size_t)N; i++) {
        for (int bit = (i == 0 ? 1 : 0); bit < 8; bit++) {
            int b = (buf[i] >> bit) & 1;
            if (b == prev) {
                cur++;
                if (cur > longest) longest = cur;
            } else {
                runs++;
                cur = 1;
                prev = b;
            }
        }
    }
    uint64_t exp_runs = nbits / 2;
    CHECK(runs > exp_runs - exp_runs / 50 && runs < exp_runs + exp_runs / 50,
          "1 MiB bit-run count within +-2% of N/2");
    printf("      (runs %llu, expected ~%llu, longest run %llu)\n",
           (unsigned long long)runs, (unsigned long long)exp_runs,
           (unsigned long long)longest);
    CHECK(longest < 40, "1 MiB longest bit run < 40");

    /* A stuck/counter generator produces huge bias; check the mean too. */
    unsigned long sum = 0;
    for (int i = 0; i < 256; i++) sum += (unsigned long)freq[i] * i;
    double mean = (double)sum / N;
    CHECK(mean > 126.5 && mean < 129.0, "1 MiB byte mean near 127.5");

    free(buf);
}

int main(void) {
    printf("test_rng: N0 ChaCha20 CSPRNG core (RFC 8439 vectors + statistics)\n");
    test_rfc232_block();
    test_rfc242_cipher();
    test_drbg_determinism();
    test_drbg_reseed();
    test_statistical_1mb();

    printf("test_rng: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count == 0 ? 0 : 1;
}

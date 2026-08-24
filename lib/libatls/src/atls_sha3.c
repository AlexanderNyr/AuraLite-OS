/* atls_sha3.c — Keccak-f[1600], SHA3-256/512, SHAKE128/256 (FIPS 202).
 *
 * REALINTERNET2 Y5: the FIPS 203 hashes G/H/J and the XOF/PRF are
 * SHA3-512, SHA3-256, SHAKE256 and SHAKE128.  One permutation, four
 * paddings.  Freestanding C11, no secret-dependent branches (the
 * permutation is public; absorb/squeeze walk public lengths).
 */

#include "atls/atls.h"
#include <string.h>

#define ROTL64(x, n) (((x) << (n)) | ((x) >> (64 - (n))))

static const uint64_t k_rc[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL,
    0x800000000000808AULL, 0x8000000080008000ULL,
    0x000000000000808BULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL,
    0x000000000000008AULL, 0x0000000000000088ULL,
    0x0000000080008009ULL, 0x000000008000000AULL,
    0x000000008000808BULL, 0x800000000000008BULL,
    0x8000000000008089ULL, 0x8000000000008003ULL,
    0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800AULL, 0x800000008000000AULL,
    0x8000000080008081ULL, 0x8000000000008080ULL,
    0x0000000080000001ULL, 0x8000000080008008ULL
};

static void keccakf(uint64_t st[25]) {
    for (int round = 0; round < 24; round++) {
        uint64_t C[5], D[5];
        for (int x = 0; x < 5; x++)
            C[x] = st[x] ^ st[x + 5] ^ st[x + 10] ^ st[x + 15] ^ st[x + 20];
        for (int x = 0; x < 5; x++)
            D[x] = C[(x + 4) % 5] ^ ROTL64(C[(x + 1) % 5], 1);
        for (int i = 0; i < 25; i++) st[i] ^= D[i % 5];

        /* rho + pi */
        uint64_t t = st[1];
        static const int rho[24] = {
            1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
            27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44
        };
        static const int pi[24] = {
            10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
            15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1
        };
        for (int i = 0; i < 24; i++) {
            uint64_t tmp = st[pi[i]];
            st[pi[i]] = ROTL64(t, rho[i]);
            t = tmp;
        }

        for (int y = 0; y < 5; y++) {
            uint64_t a0 = st[5 * y], a1 = st[5 * y + 1], a2 = st[5 * y + 2];
            uint64_t a3 = st[5 * y + 3], a4 = st[5 * y + 4];
            st[5 * y]     = a0 ^ ((~a1) & a2);
            st[5 * y + 1] = a1 ^ ((~a2) & a3);
            st[5 * y + 2] = a2 ^ ((~a3) & a4);
            st[5 * y + 3] = a3 ^ ((~a4) & a0);
            st[5 * y + 4] = a4 ^ ((~a0) & a1);
        }
        st[0] ^= k_rc[round];
    }
}

static uint64_t load64(const uint8_t *p) {
    uint64_t x = 0;
    for (int i = 0; i < 8; i++) x |= (uint64_t)p[i] << (8 * i);
    return x;
}

static void store64(uint8_t *p, uint64_t x) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(x >> (8 * i));
}

static void keccak_init(atls_keccak_ctx *c, size_t rate) {
    memset(c, 0, sizeof(*c));
    c->rate = rate;
}

static void keccak_absorb(atls_keccak_ctx *c, const uint8_t *in, size_t n) {
    size_t rate = c->rate;
    while (n > 0) {
        size_t take = rate - c->nbuf;
        if (take > n) take = n;
        for (size_t i = 0; i < take; i++) c->buf[c->nbuf + i] = in[i];
        c->nbuf += take;
        in += take;
        n -= take;
        if (c->nbuf == rate) {
            for (size_t i = 0; i < rate / 8; i++)
                c->s[i] ^= load64(c->buf + 8 * i);
            keccakf(c->s);
            c->nbuf = 0;
        }
    }
}

static void keccak_finalize(atls_keccak_ctx *c, uint8_t pad) {
    size_t rate = c->rate;
    c->buf[c->nbuf++] = pad;
    while (c->nbuf < rate) c->buf[c->nbuf++] = 0;
    c->buf[rate - 1] |= 0x80;
    for (size_t i = 0; i < rate / 8; i++)
        c->s[i] ^= load64(c->buf + 8 * i);
    keccakf(c->s);
    c->nbuf = 0;
    c->squeezing = 1;
}

static void keccak_squeeze(atls_keccak_ctx *c, uint8_t *out, size_t n) {
    size_t rate = c->rate;
    while (n > 0) {
        if (c->nbuf == 0) {
            for (size_t i = 0; i < rate / 8; i++)
                store64(c->buf + 8 * i, c->s[i]);
            c->nbuf = rate;
        }
        size_t take = c->nbuf;
        if (take > n) take = n;
        size_t off = rate - c->nbuf;
        for (size_t i = 0; i < take; i++) out[i] = c->buf[off + i];
        c->nbuf -= take;
        out += take;
        n -= take;
        if (c->nbuf == 0) keccakf(c->s);
    }
}

void atls_sha3_256(const void *data, size_t len, uint8_t out[32]) {
    atls_keccak_ctx c;
    keccak_init(&c, 136);
    keccak_absorb(&c, (const uint8_t *)data, len);
    keccak_finalize(&c, 0x06);
    keccak_squeeze(&c, out, 32);
}

void atls_sha3_512(const void *data, size_t len, uint8_t out[64]) {
    atls_keccak_ctx c;
    keccak_init(&c, 72);
    keccak_absorb(&c, (const uint8_t *)data, len);
    keccak_finalize(&c, 0x06);
    keccak_squeeze(&c, out, 64);
}

void atls_shake128(const void *data, size_t len, uint8_t *out, size_t outlen) {
    atls_keccak_ctx c;
    keccak_init(&c, 168);
    keccak_absorb(&c, (const uint8_t *)data, len);
    keccak_finalize(&c, 0x1F);
    keccak_squeeze(&c, out, outlen);
}

void atls_shake256(const void *data, size_t len, uint8_t *out, size_t outlen) {
    atls_keccak_ctx c;
    keccak_init(&c, 136);
    keccak_absorb(&c, (const uint8_t *)data, len);
    keccak_finalize(&c, 0x1F);
    keccak_squeeze(&c, out, outlen);
}

void atls_shake128_init(atls_keccak_ctx *c) { keccak_init(c, 168); }
void atls_shake256_init(atls_keccak_ctx *c) { keccak_init(c, 136); }

void atls_keccak_absorb(atls_keccak_ctx *c, const void *data, size_t len) {
    keccak_absorb(c, (const uint8_t *)data, len);
}

void atls_keccak_finalize(atls_keccak_ctx *c, uint8_t pad) {
    keccak_finalize(c, pad);
}

void atls_keccak_squeeze(atls_keccak_ctx *c, uint8_t *out, size_t len) {
    keccak_squeeze(c, out, len);
}

/* atls_sha256.c — SHA-256 (FIPS 180-4), streaming + one-shot.
 *
 * Standard Merkle-Damgard implementation.  Big-endian message schedule;
 * all multi-byte I/O is serialised byte-by-byte so there are no host
 * endianness assumptions.
 */

#include "atls/atls.h"

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t rotr32(uint32_t v, int n) {
    return (v >> n) | (v << (32 - n));
}

static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static inline void be32_put(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v >> 0);
}

static void sha256_block(atls_sha256_ctx *c, const uint8_t p[64]) {
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h;

    for (int i = 0; i < 16; i++) w[i] = be32(p + i * 4);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g  = c->h[6]; h = c->h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1  = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch  = (e & f) ^ ((~e) & g);
        uint32_t t1  = h + S1 + ch + K[i] + w[i];
        uint32_t S0  = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2  = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g;  c->h[7] += h;
}

void atls_sha256_init(atls_sha256_ctx *c) {
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->nbytes = 0;
    c->buflen = 0;
}

void atls_sha256_update(atls_sha256_ctx *c, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->nbytes += len;
    while (len > 0) {
        size_t take = 64 - c->buflen;
        if (take > len) take = len;
        for (size_t i = 0; i < take; i++) c->buf[c->buflen + i] = p[i];
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen == 64) {
            sha256_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

void atls_sha256_final(atls_sha256_ctx *c, uint8_t out[32]) {
    uint64_t bitlen = c->nbytes * 8;
    uint8_t pad = 0x80;
    atls_sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buflen != 56) atls_sha256_update(c, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) {
        lenb[i] = (uint8_t)(bitlen >> (56 - i * 8));
    }
    atls_sha256_update(c, lenb, 8);   /* fills 56..64 and flushes the block */
    for (int i = 0; i < 8; i++) be32_put(out + i * 4, c->h[i]);
    atls_wipe(c, sizeof(*c));
}

void atls_sha256(const void *data, size_t len, uint8_t out[32]) {
    atls_sha256_ctx c;
    atls_sha256_init(&c);
    atls_sha256_update(&c, data, len);
    atls_sha256_final(&c, out);
}

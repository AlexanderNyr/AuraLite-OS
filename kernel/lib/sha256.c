/* kernel/lib/sha256.c — freestanding FIPS 180-4 SHA-256 (RESIDUE2 T3).
 *
 * See sha256.h for WHY this exists in the kernel at all (D2 keeps the
 * kernel off libatls; btrfs needs a checksum).  The implementation is
 * the textbook big-endian 64-round compression function, written plain
 * on purpose: no tables beyond the round constants, no platform
 * intrinsics, identical bytes on every width this tree compiles.
 *
 * Verified by tests/unit/test_ksha256.c against the RFC 6234 vectors,
 * run both host-side (make test-unit) and compiled into the kernel as
 * the very object this file builds.
 */

#include "kernel/lib/sha256.h"
#include "kernel/lib/string.h"

/* FIPS 180-4 §4.2.2: the first 32 bits of the fractional parts of the
 * cube roots of the first 64 primes. */
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

static inline uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static inline void be32_put(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Compress one 64-byte block into the state. */
static void ksha256_block(struct ksha256_ctx *ctx, const uint8_t *p) {
    uint32_t w[64];
    for (int t = 0; t < 16; t++)
        w[t] = be32(p + t * 4);
    for (int t = 16; t < 64; t++) {
        uint32_t s0 = rotr32(w[t - 15], 7) ^ rotr32(w[t - 15], 18) ^
                      (w[t - 15] >> 3);
        uint32_t s1 = rotr32(w[t - 2], 17) ^ rotr32(w[t - 2], 19) ^
                      (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];

    for (int t = 0; t < 64; t++) {
        uint32_t S1    = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch    = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + K[t] + w[t];
        uint32_t S0    = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void ksha256_init(struct ksha256_ctx *ctx) {
    /* FIPS 180-4 §5.3.3: the initial hash value. */
    static const uint32_t H0[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    for (int i = 0; i < 8; i++)
        ctx->h[i] = H0[i];
    ctx->total  = 0;
    ctx->buflen = 0;
}

void ksha256_update(struct ksha256_ctx *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    ctx->total += len;
    while (len > 0) {
        size_t take = 64 - ctx->buflen;
        if (take > len)
            take = len;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p   += take;
        len -= take;
        if (ctx->buflen == 64) {
            ksha256_block(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

void ksha256_final(struct ksha256_ctx *ctx, uint8_t out[KSHA256_DIGEST_SIZE]) {
    /* Snapshot BEFORE padding: the length field counts message bytes
     * only. */
    uint64_t bitlen = ctx->total * 8u;

    /* FIPS 180-4 §5.1.1: append 0x80, then zeros until 56 mod 64, then
     * the 64-bit big-endian bit count.  When the tail has no room for
     * the nine padding bytes, the zeros run out a second block. */
    ctx->buf[ctx->buflen++] = 0x80;
    if (ctx->buflen > 56) {
        while (ctx->buflen < 64)
            ctx->buf[ctx->buflen++] = 0;
        ksha256_block(ctx, ctx->buf);
        ctx->buflen = 0;
    }
    while (ctx->buflen < 56)
        ctx->buf[ctx->buflen++] = 0;
    for (int i = 0; i < 8; i++)
        ctx->buf[56 + i] = (uint8_t)(bitlen >> (56 - i * 8));
    ksha256_block(ctx, ctx->buf);

    for (int i = 0; i < 8; i++)
        be32_put(out + i * 4, ctx->h[i]);
}

void ksha256(const void *data, size_t len, uint8_t out[KSHA256_DIGEST_SIZE]) {
    struct ksha256_ctx ctx;
    ksha256_init(&ctx);
    ksha256_update(&ctx, data, len);
    ksha256_final(&ctx, out);
}

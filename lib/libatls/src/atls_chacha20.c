/* atls_chacha20.c — ChaCha20 stream cipher (RFC 8439 section 2.4).
 *
 * Userspace copy, independent of the kernel's rng_core.h (decision D2:
 * crypto lives in userspace).  Same RFC 8439 block function, verified
 * against the §2.3.2 / §2.4.2 vectors by tests/unit/test_atls_aead.c.
 */

#include "atls/atls.h"

static inline uint32_t rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

#define QR(a, b, c, d)                       \
    do {                                     \
        a += b; d ^= a; d = rotl32(d, 16);   \
        c += d; b ^= c; b = rotl32(b, 12);   \
        a += b; d ^= a; d = rotl32(d, 8);    \
        c += d; b ^= c; b = rotl32(b, 7);    \
    } while (0)

static inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void chacha20_block(const uint8_t key[32], uint32_t counter,
                           const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t st[16], x[16];

    st[0] = 0x61707865u; st[1] = 0x3320646eu;
    st[2] = 0x79622d32u; st[3] = 0x6b206574u;
    for (int i = 0; i < 8; i++) st[4 + i] = le32(key + i * 4);
    st[12] = counter;
    st[13] = le32(nonce);
    st[14] = le32(nonce + 4);
    st[15] = le32(nonce + 8);

    for (int i = 0; i < 16; i++) x[i] = st[i];
    for (int r = 0; r < 10; r++) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + st[i];
        out[i * 4 + 0] = (uint8_t)(v >> 0);
        out[i * 4 + 1] = (uint8_t)(v >> 8);
        out[i * 4 + 2] = (uint8_t)(v >> 16);
        out[i * 4 + 3] = (uint8_t)(v >> 24);
    }
}

void atls_chacha20_xor(const uint8_t key[32], uint32_t counter,
                       const uint8_t nonce[12],
                       const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t block[64];
    size_t off = 0;

    while (off < len) {
        chacha20_block(key, counter++, nonce, block);
        size_t take = len - off;
        if (take > 64) take = 64;
        for (size_t i = 0; i < take; i++) {
            out[off + i] = (uint8_t)(in[off + i] ^ block[i]);
        }
        off += take;
    }
    atls_wipe(block, sizeof(block));
}

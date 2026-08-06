/* atls_poly1305.c — Poly1305 one-time authenticator (RFC 8439 §2.5).
 *
 * Radix-2^26 limbs with 64-bit products (the classic "donna" shape),
 * portable C.  The arithmetic runs on public accumulator state derived
 * from the message; the key-dependent part is a fixed sequence of
 * multiply-accumulates, so there are no secret-dependent branches.
 */

#include "atls/atls.h"

static inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void le32_put(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 0);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void atls_poly1305(const uint8_t key[32],
                   const uint8_t *msg, size_t msglen,
                   uint8_t tag[16]) {
    /* r, clamped per RFC 8439 §2.5.1 (top four bits of r[3],r[7],r[11],
     * r[15] clear; bottom two bits of r[4],r[8],r[12] clear). */
    uint32_t r0 =  (le32(key +  0))       & 0x3ffffffu;
    uint32_t r1 =  (le32(key +  3) >> 2)  & 0x3ffff03u;
    uint32_t r2 =  (le32(key +  6) >> 4)  & 0x3ffc0ffu;
    uint32_t r3 =  (le32(key +  9) >> 6)  & 0x3f03fffu;
    uint32_t r4 =  (le32(key + 12) >> 8)  & 0x00fffffu;

    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;

    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;

    size_t remaining = msglen;
    while (remaining > 0) {
        uint8_t block[17];
        size_t take = remaining < 16 ? remaining : 16;
        for (size_t i = 0; i < take; i++) block[i] = msg[i];
        block[take] = 0x01;                     /* 2^(8*take) pad bit */
        for (size_t i = take + 1; i < 17; i++) block[i] = 0;
        /* A full block contributes 2^128 as the 17th "byte" bit; a
         * partial block has already been padded with 0x01 at its end and
         * must NOT get the high bit. */
        uint32_t hibit = (take == 16) ? (1u << 24) : 0u;

        h0 +=  le32(block +  0)        & 0x3ffffffu;
        h1 += (le32(block +  3) >> 2)  & 0x3ffffffu;
        h2 += (le32(block +  6) >> 4)  & 0x3ffffffu;
        h3 += (le32(block +  9) >> 6)  & 0x3ffffffu;
        h4 += (le32(block + 12) >> 8)  | hibit;

        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 +
                      (uint64_t)h2 * s3 + (uint64_t)h3 * s2 +
                      (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 +
                      (uint64_t)h2 * s4 + (uint64_t)h3 * s3 +
                      (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 +
                      (uint64_t)h2 * r0 + (uint64_t)h3 * s4 +
                      (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 +
                      (uint64_t)h2 * r1 + (uint64_t)h3 * r0 +
                      (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 +
                      (uint64_t)h2 * r2 + (uint64_t)h3 * r1 +
                      (uint64_t)h4 * r0;

        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffffu; d1 += c;
        c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffffu; d2 += c;
        c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffffu; d3 += c;
        c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffffu; d4 += c;
        c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffffu;
        h0 += c * 5;
        c = h0 >> 26; h0 &= 0x3ffffffu; h1 += c;

        msg += take;
        remaining -= take;
    }

    /* Full carry chain. */
    uint32_t c;
    c = h1 >> 26; h1 &= 0x3ffffffu; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffffu; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffffu; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffffu; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffffu; h1 += c;

    /* Compute h + -(2^130 - 5) and select it if it did not borrow. */
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffffu;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffffu;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffffu;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffffu;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;      /* all-ones if no borrow */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    /* Reassemble into four 32-bit words and add s = key[16..31]. */
    uint64_t f;
    f = (uint64_t)h0 | ((uint64_t)h1 << 26);
    uint32_t w0 = (uint32_t)f;
    f = ((uint64_t)h1 >> 6) | ((uint64_t)h2 << 20);
    uint32_t w1 = (uint32_t)f;
    f = ((uint64_t)h2 >> 12) | ((uint64_t)h3 << 14);
    uint32_t w2 = (uint32_t)f;
    f = ((uint64_t)h3 >> 18) | ((uint64_t)h4 << 8);
    uint32_t w3 = (uint32_t)f;

    f = (uint64_t)w0 + le32(key + 16);             w0 = (uint32_t)f;
    f = (uint64_t)w1 + le32(key + 20) + (f >> 32); w1 = (uint32_t)f;
    f = (uint64_t)w2 + le32(key + 24) + (f >> 32); w2 = (uint32_t)f;
    f = (uint64_t)w3 + le32(key + 28) + (f >> 32); w3 = (uint32_t)f;

    le32_put(tag +  0, w0);
    le32_put(tag +  4, w1);
    le32_put(tag +  8, w2);
    le32_put(tag + 12, w3);
}

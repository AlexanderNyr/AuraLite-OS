/* atls_fe.c — field arithmetic mod p = 2^255 - 19.
 *
 * Five limbs in radix 2^51.  After mul/sq each limb is bounded by about
 * 2^52, which keeps every __int128 accumulator far below 2^127.
 */

#include "atls_fe.h"
#include "atls/atls.h"

#define MASK51 0x7ffffffffffffULL

typedef unsigned __int128 u128;

void atls_fe_0(atls_fe *r) {
    for (int i = 0; i < 5; i++) r->v[i] = 0;
}

void atls_fe_1(atls_fe *r) {
    r->v[0] = 1;
    for (int i = 1; i < 5; i++) r->v[i] = 0;
}

void atls_fe_copy(atls_fe *r, const atls_fe *a) {
    for (int i = 0; i < 5; i++) r->v[i] = a->v[i];
}

void atls_fe_add(atls_fe *r, const atls_fe *a, const atls_fe *b) {
    for (int i = 0; i < 5; i++) r->v[i] = a->v[i] + b->v[i];
}

/* Subtract with a pre-added 2p so intermediate limbs never underflow
 * (inputs are each < ~2^52 per limb, and 2p's limbs are ~2^51). */
void atls_fe_sub(atls_fe *r, const atls_fe *a, const atls_fe *b) {
    static const uint64_t two_p[5] = {
        0xfffffffffffdaULL, 0xffffffffffffeULL, 0xffffffffffffeULL,
        0xffffffffffffeULL, 0xffffffffffffeULL,
    };
    for (int i = 0; i < 5; i++) {
        r->v[i] = a->v[i] + two_p[i] - b->v[i];
    }
}

void atls_fe_neg(atls_fe *r, const atls_fe *a) {
    static const uint64_t two_p[5] = {
        0xfffffffffffdaULL, 0xffffffffffffeULL, 0xffffffffffffeULL,
        0xffffffffffffeULL, 0xffffffffffffeULL,
    };
    for (int i = 0; i < 5; i++) r->v[i] = two_p[i] - a->v[i];
}

/* Carry-propagate five 128-bit accumulators down to bounded 51-bit limbs.
 * The sums BEFORE this chain are up to ~107 bits (five products of two
 * ~52-bit limbs), so the propagation must run in 128 bits — truncating to
 * uint64_t first silently loses the top half of the value. */
static inline void carry_chain(u128 t[5], uint64_t out[5]) {
    u128 c;
    c = t[0] >> 51; t[1] += c; out[0] = (uint64_t)t[0] & MASK51;
    c = t[1] >> 51; t[2] += c; out[1] = (uint64_t)t[1] & MASK51;
    c = t[2] >> 51; t[3] += c; out[2] = (uint64_t)t[2] & MASK51;
    c = t[3] >> 51; t[4] += c; out[3] = (uint64_t)t[3] & MASK51;
    c = t[4] >> 51; out[4] = (uint64_t)t[4] & MASK51;
    out[0] += (uint64_t)c * 19;
    c = out[0] >> 51; out[0] &= MASK51; out[1] += (uint64_t)c;
}

void atls_fe_mul(atls_fe *r, const atls_fe *a, const atls_fe *b) {
    const uint64_t *av = a->v, *bv = b->v;
    uint64_t b19_1 = bv[1] * 19, b19_2 = bv[2] * 19, b19_3 = bv[3] * 19,
             b19_4 = bv[4] * 19;
    u128 t[5];
    uint64_t out[5];

    t[0] = (u128)av[0] * bv[0] + (u128)av[1] * b19_4 +
           (u128)av[2] * b19_3 + (u128)av[3] * b19_2 +
           (u128)av[4] * b19_1;
    t[1] = (u128)av[0] * bv[1] + (u128)av[1] * bv[0] +
           (u128)av[2] * b19_4 + (u128)av[3] * b19_3 +
           (u128)av[4] * b19_2;
    t[2] = (u128)av[0] * bv[2] + (u128)av[1] * bv[1] +
           (u128)av[2] * bv[0] + (u128)av[3] * b19_4 +
           (u128)av[4] * b19_3;
    t[3] = (u128)av[0] * bv[3] + (u128)av[1] * bv[2] +
           (u128)av[2] * bv[1] + (u128)av[3] * bv[0] +
           (u128)av[4] * b19_4;
    t[4] = (u128)av[0] * bv[4] + (u128)av[1] * bv[3] +
           (u128)av[2] * bv[2] + (u128)av[3] * bv[1] +
           (u128)av[4] * bv[0];

    carry_chain(t, out);
    for (int i = 0; i < 5; i++) r->v[i] = out[i];
}

void atls_fe_mul_small(atls_fe *r, const atls_fe *a, uint64_t s) {
    u128 t[5];
    uint64_t out[5];
    for (int i = 0; i < 5; i++) t[i] = (u128)a->v[i] * s;
    carry_chain(t, out);
    for (int i = 0; i < 5; i++) r->v[i] = out[i];
}

/* Squaring, specialised from mul:
 *   t0 = a0^2 + 19*(2 a1 a4 + 2 a2 a3)
 *   t1 = 2 a0 a1 + 19*(2 a2 a4 + a3^2)
 *   t2 = 2 a0 a2 + a1^2 + 19*(2 a3 a4)
 *   t3 = 2 a0 a3 + 2 a1 a2 + 19*(a4^2)
 *   t4 = 2 a0 a4 + 2 a1 a3 + a2^2
 * written with the precomputed a19_i = 19*a_i to keep single multiplies. */
void atls_fe_sq(atls_fe *r, const atls_fe *a) {
    const uint64_t *av = a->v;
    uint64_t a19_3 = av[3] * 19, a19_4 = av[4] * 19;
    u128 t[5];
    uint64_t out[5];

    t[0] = (u128)av[0] * av[0] +
           (u128)(2 * av[1]) * a19_4 +
           (u128)(2 * av[2]) * a19_3;
    t[1] = (u128)(2 * av[0]) * av[1] +
           (u128)(2 * av[2]) * a19_4 +
           (u128)av[3] * a19_3;
    t[2] = (u128)(2 * av[0]) * av[2] +
           (u128)av[1] * av[1] +
           (u128)(2 * av[3]) * a19_4;
    t[3] = (u128)(2 * av[0]) * av[3] +
           (u128)(2 * av[1]) * av[2] +
           (u128)av[4] * a19_4;
    t[4] = (u128)(2 * av[0]) * av[4] +
           (u128)(2 * av[1]) * av[3] +
           (u128)av[2] * av[2];

    carry_chain(t, out);
    for (int i = 0; i < 5; i++) r->v[i] = out[i];
}

void atls_fe_cswap(atls_fe *a, atls_fe *b, uint64_t swap) {
    uint64_t mask = (uint64_t)0 - swap;   /* swap must be 0 or 1 */
    for (int i = 0; i < 5; i++) {
        uint64_t x = (a->v[i] ^ b->v[i]) & mask;
        a->v[i] ^= x;
        b->v[i] ^= x;
    }
}

void atls_fe_frombytes(atls_fe *r, const uint8_t b[32]) {
    uint64_t w[4];
    for (int i = 0; i < 4; i++) {
        w[i] = 0;
        for (int j = 0; j < 8; j++) {
            w[i] |= (uint64_t)b[i * 8 + j] << (j * 8);
        }
    }
    r->v[0] = w[0] & MASK51;
    r->v[1] = ((w[0] >> 51) | (w[1] << 13)) & MASK51;
    r->v[2] = ((w[1] >> 38) | (w[2] << 26)) & MASK51;
    r->v[3] = ((w[2] >> 25) | (w[3] << 39)) & MASK51;
    r->v[4] = (w[3] >> 12) & MASK51;        /* bit 255 dropped here */
}

void atls_fe_tobytes(uint8_t out[32], const atls_fe *a) {
    uint64_t t[5];
    for (int i = 0; i < 5; i++) t[i] = a->v[i];

    /* Carry to fully bounded limbs. */
    uint64_t c;
    c = t[0] >> 51; t[1] += c; t[0] &= MASK51;
    c = t[1] >> 51; t[2] += c; t[1] &= MASK51;
    c = t[2] >> 51; t[3] += c; t[2] &= MASK51;
    c = t[3] >> 51; t[4] += c; t[3] &= MASK51;
    c = t[4] >> 51; t[0] += c * 19; t[4] &= MASK51;
    c = t[0] >> 51; t[1] += c; t[0] &= MASK51;

    /* Conditional subtraction of p: compute t + 19 - 2^255 and keep it
     * iff it did not borrow. */
    uint64_t g[5];
    g[0] = t[0] + 19; c = g[0] >> 51; g[0] &= MASK51;
    g[1] = t[1] + c;  c = g[1] >> 51; g[1] &= MASK51;
    g[2] = t[2] + c;  c = g[2] >> 51; g[2] &= MASK51;
    g[3] = t[3] + c;  c = g[3] >> 51; g[3] &= MASK51;
    g[4] = t[4] + c;
    uint64_t overflow = g[4] >> 51;          /* 1 iff t >= p */
    g[4] &= MASK51;
    uint64_t mask = (uint64_t)0 - overflow;  /* all-ones iff subtract */
    for (int i = 0; i < 5; i++) {
        t[i] ^= (t[i] ^ g[i]) & mask;
    }

    /* Pack 5x51 bits into 32 little-endian bytes. */
    uint64_t w[4];
    w[0] = t[0] | (t[1] << 51);
    w[1] = (t[1] >> 13) | (t[2] << 38);
    w[2] = (t[2] >> 26) | (t[3] << 25);
    w[3] = (t[3] >> 39) | (t[4] << 12);
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) {
            out[i * 8 + j] = (uint8_t)(w[i] >> (j * 8));
        }
    }
}

/* Generic square-and-multiply over a little-endian 32-byte exponent.
 * Variable-time over the exponent — acceptable: both exponents below
 * (p-2, 2^252-3) are public constants. */
static void fe_pow(atls_fe *r, const atls_fe *base, const uint8_t e[32]) {
    atls_fe acc, b;
    atls_fe_1(&acc);
    atls_fe_copy(&b, base);

    for (int bit = 255; bit >= 0; bit--) {
        atls_fe_sq(&acc, &acc);
        if ((e[bit >> 3] >> (bit & 7)) & 1) {
            atls_fe_mul(&acc, &acc, &b);
        }
    }
    atls_fe_copy(r, &acc);
}

void atls_fe_invert(atls_fe *r, const atls_fe *a) {
    /* p - 2 = 2^255 - 21, little-endian. */
    static const uint8_t e[32] = {
        0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f,
    };
    fe_pow(r, a, e);
}

void atls_fe_pow2523(atls_fe *r, const atls_fe *a) {
    /* 2^252 - 3, little-endian. */
    static const uint8_t e[32] = {
        0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f,
    };
    fe_pow(r, a, e);
}

int atls_fe_iszero(const atls_fe *a) {
    uint8_t b[32];
    atls_fe_tobytes(b, a);
    uint8_t zero = 0;
    for (int i = 0; i < 32; i++) zero |= b[i];
    return (int)(((uint32_t)zero - 1u) >> 31);
}

int atls_fe_is_negative(const atls_fe *a) {
    uint8_t b[32];
    atls_fe_tobytes(b, a);
    return (int)(b[0] & 1);
}

int atls_fe_ct_eq(const atls_fe *a, const atls_fe *b) {
    uint8_t x[32], y[32];
    atls_fe_tobytes(x, a);
    atls_fe_tobytes(y, b);
    return atls_ct_eq(x, y, 32);
}

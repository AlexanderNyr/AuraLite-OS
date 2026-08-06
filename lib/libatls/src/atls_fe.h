#ifndef LIBATLS_ATLS_FE_H
#define LIBATLS_ATLS_FE_H

/* atls_fe.h — field arithmetic modulo p = 2^255 - 19, shared by X25519
 * and Ed25519 (INTERNET_PLAN.md phase N1).  Internal header.
 *
 * Representation: five 64-bit limbs in radix 2^51 (curve25519-donna-64
 * shape), multiplied with unsigned __int128 accumulators.  This is
 * userspace code (the guest userspace build is plain 64-bit C), so
 * __int128 is available; the kernel never compiles this file.
 *
 * Everything here is straight-line except atls_fe_cswap's callers and the
 * final conditional subtraction in tobytes (both branch-free masked
 * selects).  The only variable-time operations are the two generic
 * exponentiations, which run on public exponents (p-2, 2^252-3).
 */

#include <stdint.h>

typedef struct {
    uint64_t v[5];
} atls_fe;

void atls_fe_0(atls_fe *r);
void atls_fe_1(atls_fe *r);
void atls_fe_copy(atls_fe *r, const atls_fe *a);
void atls_fe_add(atls_fe *r, const atls_fe *a, const atls_fe *b);
void atls_fe_sub(atls_fe *r, const atls_fe *a, const atls_fe *b);
void atls_fe_neg(atls_fe *r, const atls_fe *a);
void atls_fe_mul(atls_fe *r, const atls_fe *a, const atls_fe *b);
void atls_fe_mul_small(atls_fe *r, const atls_fe *a, uint64_t s);
void atls_fe_sq(atls_fe *r, const atls_fe *a);

/* Constant-time conditional swap: swap a and b iff swap == 1. */
void atls_fe_cswap(atls_fe *a, atls_fe *b, uint64_t swap);

/* Decode 32 little-endian bytes, masking bit 255 (RFC 7748 decodeUCoordinate
 * behaviour; also what Ed25519 wants for the y coordinate before the sign
 * bit is split off by the caller). */
void atls_fe_frombytes(atls_fe *r, const uint8_t b[32]);

/* Fully reduce to [0, p) and serialise 32 little-endian bytes. */
void atls_fe_tobytes(uint8_t out[32], const atls_fe *a);

/* r = a^(p-2)  (field inverse, 0 -> 0). */
void atls_fe_invert(atls_fe *r, const atls_fe *a);

/* r = a^(2^252-3)  (the sqrt candidate exponent (p-5)/8). */
void atls_fe_pow2523(atls_fe *r, const atls_fe *a);

/* Constant-time comparisons on CANONICAL encodings. */
int atls_fe_iszero(const atls_fe *a);
int atls_fe_is_negative(const atls_fe *a);   /* low bit of canonical form */
int atls_fe_ct_eq(const atls_fe *a, const atls_fe *b);

#endif /* LIBATLS_ATLS_FE_H */

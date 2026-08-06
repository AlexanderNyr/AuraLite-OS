/* atls_x25519.c — X25519 scalar multiplication (RFC 7748).
 *
 * The Montgomery ladder exactly as the RFC's pseudocode, over the 5x51
 * field module.  a24 = 121665 = (486662 - 2) / 4, matching the RFC's
 * z_2 = E * (AA + a24 * E) form.
 *
 * Constant-time with respect to the scalar: the ladder is straight-line,
 * swaps are masked cswaps, and clamping is arithmetic, not branching.
 * The final all-zero check rejects low-order points (RFC 7748 §6.1
 * "implementations MUST abort ... if the shared secret is all zeros" is
 * the TLS 1.3 reading; we surface it as ATLS_ERR_LOW_ORDER so the
 * handshake layer can fail loudly).
 */

#include "atls_fe.h"
#include "atls/atls.h"

const uint8_t ATLS_X25519_BASEPOINT[32] = {
    9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

void atls_x25519_clamp(uint8_t scalar[32]) {
    scalar[0] &= 248;
    scalar[31] &= 127;
    scalar[31] |= 64;
}

int atls_x25519(uint8_t out[32], const uint8_t scalar[32],
                const uint8_t point[32]) {
    if (!out || !scalar || !point) return ATLS_ERR_INPUT;

    uint8_t k[32];
    for (int i = 0; i < 32; i++) k[i] = scalar[i];
    atls_x25519_clamp(k);

    atls_fe x_1, x_2, z_2, x_3, z_3;
    atls_fe_frombytes(&x_1, point);      /* masks bit 255 per the RFC */
    atls_fe_1(&x_2);
    atls_fe_0(&z_2);
    atls_fe_copy(&x_3, &x_1);
    atls_fe_1(&z_3);

    uint64_t swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (k[pos >> 3] >> (pos & 7)) & 1u;
        swap ^= bit;
        atls_fe_cswap(&x_2, &x_3, swap);
        atls_fe_cswap(&z_2, &z_3, swap);
        swap = bit;

        atls_fe A, AA, B, BB, E, C, D, DA, CB, t;

        atls_fe_add(&A, &x_2, &z_2);
        atls_fe_sq(&AA, &A);
        atls_fe_sub(&B, &x_2, &z_2);
        atls_fe_sq(&BB, &B);
        atls_fe_sub(&E, &AA, &BB);
        atls_fe_add(&C, &x_3, &z_3);
        atls_fe_sub(&D, &x_3, &z_3);
        atls_fe_mul(&DA, &D, &A);
        atls_fe_mul(&CB, &C, &B);

        atls_fe_add(&t, &DA, &CB);
        atls_fe_sq(&x_3, &t);
        atls_fe_sub(&t, &DA, &CB);
        atls_fe_sq(&t, &t);
        atls_fe_mul(&z_3, &x_1, &t);

        atls_fe_mul(&x_2, &AA, &BB);
        /* z_2 = E * (AA + a24 * E), a24 = 121665 */
        atls_fe_mul_small(&t, &E, 121665);
        atls_fe_add(&t, &AA, &t);
        atls_fe_mul(&z_2, &E, &t);
    }
    atls_fe_cswap(&x_2, &x_3, swap);
    atls_fe_cswap(&z_2, &z_3, swap);

    atls_fe zinv;
    atls_fe_invert(&zinv, &z_2);
    atls_fe_mul(&x_2, &x_2, &zinv);
    atls_fe_tobytes(out, &x_2);

    atls_wipe(k, sizeof(k));

    /* All-zero shared secret: low-order input point. */
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= out[i];
    if (((uint32_t)acc - 1u) >> 31) {      /* acc == 0 */
        return ATLS_ERR_LOW_ORDER;
    }
    return ATLS_OK;
}

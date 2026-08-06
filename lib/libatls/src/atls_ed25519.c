/* atls_ed25519.c — Ed25519 signature VERIFICATION (RFC 8032).
 *
 * Verification only, per INTERNET_PLAN.md D4: certificate chains need
 * signature checks, and nothing in this OS signs.  No key generation,
 * no signing.
 *
 * The curve is the twisted Edwards curve -x^2 + y^2 = 1 + d x^2 y^2
 * over GF(2^255-19) with the extended homogeneous coordinates
 * (X:Y:Z:T), x=X/Z, y=Y/Z, xy=T/Z, and the RFC's own §5.1.7 equation:
 *
 *     [S]B = R + [h]A,    h = SHA-512(R || A || M) mod L
 *
 * Points are compared with the cross-multiplication of the RFC's
 * reference implementation (x1 z2 == x2 z1, y1 z2 == y2 z1), which
 * needs no inversion.
 *
 * Stack discipline: this file is on the 64 KiB guest stack, so there are
 * no large locals; every scratch ge (4 fields * 5 limbs * 8 bytes = 160
 * bytes) is function-scoped and small.  The scalar multiplications are
 * plain double-and-add — two of them per verification, which is slow by
 * ref10 standards and honest about it; certificate chains carry three
 * signatures at most, and N3 measures the handshake cost, not hides it.
 */

#include "atls_fe.h"
#include "atls/atls.h"

typedef struct {
    atls_fe X, Y, Z, T;
} ge;

/* ---- curve constants (generated and sanity-checked: the base point is
 * on the curve; L is 253 bits) ---- */

static const uint8_t ED25519_D[32] = {
    0xa3, 0x78, 0x59, 0x13, 0xca, 0x4d, 0xeb, 0x75,
    0xab, 0xd8, 0x41, 0x41, 0x4d, 0x0a, 0x70, 0x00,
    0x98, 0xe8, 0x79, 0x77, 0x79, 0x40, 0xc7, 0x8c,
    0x73, 0xfe, 0x6f, 0x2b, 0xee, 0x6c, 0x03, 0x52,
};
static const uint8_t ED25519_D2[32] = {
    0x59, 0xf1, 0xb2, 0x26, 0x94, 0x9b, 0xd6, 0xeb,
    0x56, 0xb1, 0x83, 0x82, 0x9a, 0x14, 0xe0, 0x00,
    0x30, 0xd1, 0xf3, 0xee, 0xf2, 0x80, 0x8e, 0x19,
    0xe7, 0xfc, 0xdf, 0x56, 0xdc, 0xd9, 0x06, 0x24,
};
static const uint8_t ED25519_SQRT_M1[32] = {
    0xb0, 0xa0, 0x0e, 0x4a, 0x27, 0x1b, 0xee, 0xc4,
    0x78, 0xe4, 0x2f, 0xad, 0x06, 0x18, 0x43, 0x2f,
    0xa7, 0xd7, 0xfb, 0x3d, 0x99, 0x00, 0x4d, 0x2b,
    0x0b, 0xdf, 0xc1, 0x4f, 0x80, 0x24, 0x83, 0x2b,
};
static const uint8_t ED25519_BASE_X[32] = {
    0x1a, 0xd5, 0x25, 0x8f, 0x60, 0x2d, 0x56, 0xc9,
    0xb2, 0xa7, 0x25, 0x95, 0x60, 0xc7, 0x2c, 0x69,
    0x5c, 0xdc, 0xd6, 0xfd, 0x31, 0xe2, 0xa4, 0xc0,
    0xfe, 0x53, 0x6e, 0xcd, 0xd3, 0x36, 0x69, 0x21,
};
static const uint8_t ED25519_BASE_Y[32] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
};
static const uint8_t ED25519_L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};

static void load_const(atls_fe *r, const uint8_t b[32]) {
    atls_fe_frombytes(r, b);
}

/* ---- group law (extended coordinates, a = -1) ---- */

static void ge_neutral(ge *p) {
    atls_fe_0(&p->X);
    atls_fe_1(&p->Y);
    atls_fe_1(&p->Z);
    atls_fe_0(&p->T);
}

/* dbl-2008-hwcd. */
static void ge_double(ge *r, const ge *p) {
    atls_fe A, B, C, D, E, G, F, H, t;

    atls_fe_sq(&A, &p->X);
    atls_fe_sq(&B, &p->Y);
    atls_fe_sq(&C, &p->Z);
    atls_fe_add(&C, &C, &C);
    atls_fe_neg(&D, &A);                          /* D = a*A = -A */
    atls_fe_add(&t, &p->X, &p->Y);
    atls_fe_sq(&E, &t);
    atls_fe_sub(&E, &E, &A);
    atls_fe_sub(&E, &E, &B);
    atls_fe_add(&G, &D, &B);
    atls_fe_sub(&F, &G, &C);
    atls_fe_sub(&H, &D, &B);

    atls_fe_mul(&r->X, &E, &F);
    atls_fe_mul(&r->Y, &G, &H);
    atls_fe_mul(&r->T, &E, &H);
    atls_fe_mul(&r->Z, &F, &G);
}

/* add-2008-hwcd-3. */
static void ge_add(ge *r, const ge *p, const ge *q) {
    static atls_fe d2;
    static int d2_ready = 0;
    if (!d2_ready) { load_const(&d2, ED25519_D2); d2_ready = 1; }

    atls_fe A, B, C, D, E, F, G, H, t, u;

    atls_fe_sub(&t, &p->Y, &p->X);
    atls_fe_sub(&u, &q->Y, &q->X);
    atls_fe_mul(&A, &t, &u);
    atls_fe_add(&t, &p->Y, &p->X);
    atls_fe_add(&u, &q->Y, &q->X);
    atls_fe_mul(&B, &t, &u);
    atls_fe_mul(&C, &p->T, &d2);
    atls_fe_mul(&C, &C, &q->T);
    atls_fe_mul(&D, &p->Z, &q->Z);
    atls_fe_add(&D, &D, &D);
    atls_fe_sub(&E, &B, &A);
    atls_fe_sub(&F, &D, &C);
    atls_fe_add(&G, &D, &C);
    atls_fe_add(&H, &B, &A);

    atls_fe_mul(&r->X, &E, &F);
    atls_fe_mul(&r->Y, &G, &H);
    atls_fe_mul(&r->T, &E, &H);
    atls_fe_mul(&r->Z, &F, &G);
}

/* r = [scalar]p, MSB-first double-and-add.  Not constant-time in the
 * addition pattern — acceptable: in verify() the scalars are S (public
 * in the signature) and h (a public hash). */
static void ge_scalarmult(ge *r, const ge *p, const uint8_t scalar[32]) {
    ge q, acc;
    ge_neutral(&acc);
    for (int bit = 255; bit >= 0; bit--) {
        ge_double(&acc, &acc);
        if ((scalar[bit >> 3] >> (bit & 7)) & 1) {
            ge tmp;
            ge_add(&tmp, &acc, p);
            acc = tmp;
        }
    }
    q = acc;
    *r = q;
}

/* ---- encoding ---- */

/* Decode a 32-byte compressed point.  Returns 0 or -1.  Per RFC 8032
 * §5.1.3, including the canonical-encoding check (the decoded y must
 * re-serialise to exactly the input bytes). */
static int ge_frombytes(ge *p, const uint8_t s[32]) {
    static atls_fe d_fe;
    static int d_ready = 0;
    if (!d_ready) { load_const(&d_fe, ED25519_D); d_ready = 1; }

    uint8_t yb[32];
    for (int i = 0; i < 32; i++) yb[i] = s[i];
    int sign = (yb[31] >> 7) & 1;
    yb[31] &= 0x7f;

    atls_fe y;
    atls_fe_frombytes(&y, yb);

    uint8_t chk[32];
    atls_fe_tobytes(chk, &y);
    if (!atls_ct_eq(chk, yb, 32)) return -1;     /* non-canonical y */

    atls_fe one, y2, dy2, u, v;
    atls_fe_1(&one);
    atls_fe_sq(&y2, &y);
    atls_fe_mul(&dy2, &d_fe, &y2);
    atls_fe_sub(&u, &y2, &one);                  /* u = y^2 - 1 */
    atls_fe_add(&v, &dy2, &one);                 /* v = d y^2 + 1 */

    /* x = u v^3 (u v^7)^((p-5)/8) */
    atls_fe v3, v7, uv7, x, vx2;
    atls_fe_sq(&v3, &v);
    atls_fe_mul(&v3, &v3, &v);
    atls_fe_sq(&v7, &v3);
    atls_fe_mul(&v7, &v7, &v);
    atls_fe_mul(&uv7, &u, &v7);
    atls_fe_pow2523(&x, &uv7);
    atls_fe_mul(&x, &x, &u);
    atls_fe_mul(&x, &x, &v3);

    atls_fe_sq(&vx2, &x);
    atls_fe_mul(&vx2, &vx2, &v);
    if (!atls_fe_ct_eq(&vx2, &u)) {
        static atls_fe sqrt_m1;
        static int sm1_ready = 0;
        if (!sm1_ready) { load_const(&sqrt_m1, ED25519_SQRT_M1); sm1_ready = 1; }
        atls_fe_mul(&x, &x, &sqrt_m1);
        atls_fe_sq(&vx2, &x);
        atls_fe_mul(&vx2, &vx2, &v);
        if (!atls_fe_ct_eq(&vx2, &u)) return -1;   /* not on curve */
    }

    if (atls_fe_iszero(&x) && sign) return -1;     /* x=0 wants sign 0 */
    if (atls_fe_is_negative(&x) != sign) {
        atls_fe_neg(&x, &x);
    }

    p->X = x;
    p->Y = y;
    atls_fe_1(&p->Z);
    atls_fe_mul(&p->T, &x, &y);
    return 0;
}

/* Cross-multiplication point equality (RFC 8032 reference's
 * point_equal): x1 z2 == x2 z1 and y1 z2 == y2 z1. */
static int ge_equal(const ge *p, const ge *q) {
    atls_fe a, b;
    atls_fe_mul(&a, &p->X, &q->Z);
    atls_fe_mul(&b, &q->X, &p->Z);
    if (!atls_fe_ct_eq(&a, &b)) return 0;
    atls_fe_mul(&a, &p->Y, &q->Z);
    atls_fe_mul(&b, &q->Y, &p->Z);
    return atls_fe_ct_eq(&a, &b);
}

/* ---- scalar arithmetic mod L ---- */

/* s >= L ?  (both 32-byte little-endian) */
static int sc_ge_L(const uint8_t s[32]) {
    for (int i = 31; i >= 0; i--) {
        if (s[i] > ED25519_L[i]) return 1;
        if (s[i] < ED25519_L[i]) return 0;
    }
    return 1;    /* equal: not canonical */
}

/* Reduce a 64-byte little-endian integer modulo L by repeated
 * conditional subtraction of shifted L.  h is public data (a hash), so
 * the data-dependent branch leaks nothing secret.  At most 260
 * subtractions; each is a handful of limb ops. */
static void sc_reduce64(const uint8_t in[64], uint8_t out[32]) {
    uint64_t x[9];
    uint64_t Lw[4];

    for (int i = 0; i < 8; i++) {
        x[i] = 0;
        for (int j = 0; j < 8; j++) {
            x[i] |= (uint64_t)in[i * 8 + j] << (j * 8);
        }
    }
    x[8] = 0;
    for (int i = 0; i < 4; i++) {
        Lw[i] = 0;
        for (int j = 0; j < 8; j++) {
            Lw[i] |= (uint64_t)ED25519_L[i * 8 + j] << (j * 8);
        }
    }

    for (int shift = 259; shift >= 0; shift--) {
        /* y = L << shift, as nine 64-bit limbs */
        uint64_t y[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        int ws = shift / 64, bs = shift % 64;
        for (int i = 0; i < 4; i++) {
            y[ws + i] |= Lw[i] << bs;
            if (bs && ws + i + 1 < 9) y[ws + i + 1] |= Lw[i] >> (64 - bs);
        }
        /* compare x >= y */
        int cmp = 0;   /* 1: x>y, -1: x<y, 0: equal */
        for (int i = 8; i >= 0; i--) {
            if (x[i] > y[i]) { cmp = 1; break; }
            if (x[i] < y[i]) { cmp = -1; break; }
        }
        if (cmp >= 0) {
            uint64_t borrow = 0;
            for (int i = 0; i < 9; i++) {
                uint64_t sub = y[i] + borrow;
                borrow = (x[i] < sub) ? 1 : 0;
                x[i] -= sub;
            }
        }
    }

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) {
            out[i * 8 + j] = (uint8_t)(x[i] >> (j * 8));
        }
    }
}

/* ---- public API ---- */

int atls_ed25519_verify(const uint8_t sig[64], const uint8_t pk[32],
                        const void *msg, size_t msglen) {
    if (!sig || !pk || (!msg && msglen)) return ATLS_ERR_INPUT;

    ge A, R;
    if (ge_frombytes(&A, pk) != 0) return ATLS_ERR_BAD_ENCODING;
    if (ge_frombytes(&R, sig) != 0) return ATLS_ERR_BAD_ENCODING;
    if (sc_ge_L(sig + 32)) return ATLS_ERR_BAD_SIGNATURE;

    /* h = SHA-512(R || A || M) mod L */
    uint8_t h64[64];
    atls_sha512_ctx c;
    atls_sha512_init(&c);
    atls_sha512_update(&c, sig, 32);
    atls_sha512_update(&c, pk, 32);
    atls_sha512_update(&c, msg, msglen);
    atls_sha512_final(&c, h64);
    uint8_t h[32];
    sc_reduce64(h64, h);

    /* [S]B == R + [h]A */
    static ge B;
    static int B_ready = 0;
    if (!B_ready) {
        load_const(&B.X, ED25519_BASE_X);
        load_const(&B.Y, ED25519_BASE_Y);
        atls_fe_1(&B.Z);
        atls_fe_mul(&B.T, &B.X, &B.Y);
        B_ready = 1;
    }

    ge sB, hA, rhs;
    ge_scalarmult(&sB, &B, sig + 32);
    ge_scalarmult(&hA, &A, h);
    ge_add(&rhs, &R, &hA);

    return ge_equal(&sB, &rhs) ? ATLS_OK : ATLS_ERR_BAD_SIGNATURE;
}

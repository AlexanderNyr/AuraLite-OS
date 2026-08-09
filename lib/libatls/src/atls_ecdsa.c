/* atls_ecdsa.c — ECDSA P-256 (secp256r1) verification (REALINTERNET_PLAN.md
 * phase X1).
 *
 * Verification only.  The curve is the one that now dominates the public web
 * (see the X1 rationale); without it, roughly half of real HTTPS chains abort
 * with unsupported_certificate.
 *
 * Representation: big unsigned integers as 4 x uint64 little-endian limbs
 * (256-bit), products as 8 limbs (512-bit).  Modular reduction is a generic
 * bit-by-bit shift-and-subtract over the 512-bit product, correct for ANY
 * modulus — deliberately simpler than the NIST P-256 fast reduction so the
 * two moduli used here (field prime p and group order n) share one auditable
 * code path.  Point arithmetic uses Jacobian projective coordinates with
 * a = -3 (the standard short-Weierstrass formulas).
 *
 * Everything runs on public data (a certificate chain / TLS CertificateVerify
 * is not secret), so nothing here is constant-time — same stance as
 * atls_rsa.c.  D7 applies to secret comparisons; there are none here.
 */

#include "atls/ecdsa.h"

/* ------------------------------------------------------------------ */
/* Constants (little-endian uint64 limbs).                             */
/* ------------------------------------------------------------------ */

typedef uint64_t atls_limb;
#define P256_LIMBS 4

/* 256-bit integer, little-endian limbs. */
typedef struct { atls_limb d[P256_LIMBS]; } p256n;
/* 512-bit product, little-endian limbs. */
typedef struct { atls_limb d[8]; } p256w;

/* The secp256r1 field prime  p = 2^256 - 2^224 + 2^192 + 2^96 - 1. */
static const p256n P256_P = {
    { UINT64_C(0xffffffffffffffff), UINT64_C(0x00000000ffffffff),
      UINT64_C(0x0000000000000000), UINT64_C(0xffffffff00000001) }
};
/* The secp256r1 group order  n. */
static const p256n P256_N = {
    { UINT64_C(0xf3b9cac2fc632551), UINT64_C(0xbce6faada7179e84),
      UINT64_C(0xffffffffffffffff), UINT64_C(0xffffffff00000000) }
};
/* Curve coefficient  b  (Weierstrass y^2 = x^3 - 3x + b). */
static const p256n P256_B = {
    { UINT64_C(0x3bce3c3e27d2604b), UINT64_C(0x651d06b0cc53b0f6),
      UINT64_C(0xb3ebbd55769886bc), UINT64_C(0x5ac635d8aa3a93e7) }
};
/* Base point G (affine). */
static const p256n P256_GX = {
    { UINT64_C(0xf4a13945d898c296), UINT64_C(0x77037d812deb33a0),
      UINT64_C(0xf8bce6e563a440f2), UINT64_C(0x6b17d1f2e12c4247) }
};
static const p256n P256_GY = {
    { UINT64_C(0xcbb6406837bf51f5), UINT64_C(0x2bce33576b315ece),
      UINT64_C(0x8ee7eb4a7c0f9e16), UINT64_C(0x4fe342e2fe1a7f9b) }
};

/* ------------------------------------------------------------------ */
/* Raw 256-bit arithmetic (mod 2^256).                                 */
/* ------------------------------------------------------------------ */

static void nz(p256n *r) {
    r->d[0] = r->d[1] = r->d[2] = r->d[3] = 0;
}
static void nset1(p256n *r) {
    r->d[0] = 1; r->d[1] = r->d[2] = r->d[3] = 0;
}
static void ncopy(p256n *r, const p256n *a) {
    r->d[0] = a->d[0]; r->d[1] = a->d[1];
    r->d[2] = a->d[2]; r->d[3] = a->d[3];
}
static int niszero(const p256n *a) {
    return (a->d[0] | a->d[1] | a->d[2] | a->d[3]) == 0;
}

/* Compare two 256-bit integers as unsigned.  -1, 0 or 1. */
static int ncmp(const p256n *a, const p256n *b) {
    for (int i = P256_LIMBS - 1; i >= 0; i--) {
        if (a->d[i] != b->d[i])
            return a->d[i] < b->d[i] ? -1 : 1;
    }
    return 0;
}

/* r = a + b (mod 2^256); returns carry out of bit 255. */
static atls_limb nadd(p256n *r, const p256n *a, const p256n *b) {
    atls_limb carry = 0;
    for (int i = 0; i < P256_LIMBS; i++) {
        atls_limb x = a->d[i], y = b->d[i];
        atls_limb s = x + y;
        atls_limb b1 = (s < x) ? 1 : 0;        /* overflow of x + y */
        atls_limb s2 = s + carry;
        atls_limb b2 = (s2 < s) ? 1 : 0;       /* overflow adding carry */
        r->d[i] = s2;
        carry = b1 | b2;
    }
    return carry;
}

/* r = a - b (mod 2^256); returns borrow (1 if a < b). */
static atls_limb nsub(p256n *r, const p256n *a, const p256n *b) {
    atls_limb borrow = 0;
    for (int i = 0; i < P256_LIMBS; i++) {
        atls_limb t = a->d[i] - b->d[i];
        atls_limb b1 = (a->d[i] < b->d[i]) ? 1 : 0;
        atls_limb diff = t - borrow;
        atls_limb b2 = (t < borrow) ? 1 : 0;
        r->d[i] = diff;
        borrow = b1 | b2;
    }
    return borrow;
}

/* 512-bit product: r = a * b (4 limbs -> 8 limbs). */
static void nmul(p256w *r, const p256n *a, const p256n *b) {
    for (int i = 0; i < 8; i++) r->d[i] = 0;
    for (int i = 0; i < P256_LIMBS; i++) {
        atls_limb carry = 0;
        for (int j = 0; j < P256_LIMBS; j++) {
            unsigned __int128 t =
                (unsigned __int128)r->d[i + j] +
                (unsigned __int128)a->d[i] * (unsigned __int128)b->d[j] +
                (unsigned __int128)carry;
            r->d[i + j] = (atls_limb)t;
            carry = (atls_limb)(t >> 64);
        }
        int k = i + P256_LIMBS;
        while (carry) {
            unsigned __int128 t = (unsigned __int128)r->d[k] + carry;
            r->d[k] = (atls_limb)t;
            carry = (atls_limb)(t >> 64);
            k++;
        }
    }
}

static int wcmp(const p256w *a, const p256w *b) {
    for (int i = 7; i >= 0; i--) {
        if (a->d[i] != b->d[i])
            return a->d[i] < b->d[i] ? -1 : 1;
    }
    return 0;
}
static void wsub(p256w *r, const p256w *a, const p256w *b) {
    atls_limb borrow = 0;
    for (int i = 0; i < 8; i++) {
        atls_limb t = a->d[i] - b->d[i];
        atls_limb b1 = (a->d[i] < b->d[i]) ? 1 : 0;
        atls_limb diff = t - borrow;
        atls_limb b2 = (t < borrow) ? 1 : 0;
        r->d[i] = diff;
        borrow = b1 | b2;
    }
}

/* ------------------------------------------------------------------ */
/* Modular arithmetic mod an arbitrary 256-bit modulus M (p or n).     */
/* ------------------------------------------------------------------ */

/* r = A mod M, where A is a 512-bit value (8 limbs).  Bit-by-bit binary
 * reduction; correct for any M in [1, 2^256). */
static void nmod_reduce(p256n *r, const p256w *A, const p256n *M) {
    p256w R;
    for (int i = 0; i < 8; i++) R.d[i] = 0;
    p256w Mp;
    for (int i = 0; i < 4; i++) Mp.d[i] = M->d[i];
    for (int i = 4; i < 8; i++) Mp.d[i] = 0;

    for (int bit = 511; bit >= 0; bit--) {
        /* R = R << 1 | A.bit(bit) */
        atls_limb carry = 0;
        for (int i = 0; i < 8; i++) {
            atls_limb nr = (R.d[i] << 1) | carry;
            carry = R.d[i] >> 63;
            R.d[i] = nr;
        }
        R.d[0] |= (A->d[bit >> 6] >> (bit & 63)) & 1;
        if (wcmp(&R, &Mp) >= 0) wsub(&R, &R, &Mp);
    }
    for (int i = 0; i < 4; i++) r->d[i] = R.d[i];
}

/* r = a + b mod M */
static void nmod_add(p256n *r, const p256n *a, const p256n *b,
                     const p256n *M) {
    p256n t;
    if (nadd(&t, a, b)) {
        /* t = a+b-2^256; fold the 2^256 term via c = 2^256 - M. */
        p256n c2m;
        nz(&c2m);
        nsub(&c2m, &c2m, M);                 /* c2m = 0 - M = 2^256 - M */
        if (nadd(&t, &t, &c2m)) {
            /* wrapped again: t = (a+b-2^256) + c2m - 2^256 = a+b-2*2^256+c2m.
             * Since a,b < M and c2m < 2^256, a+b+c2m < 3*2^256 so one more
             * wrap is impossible; this branch is dead but harmless. */
            nsub(&t, &t, M);
        } else if (ncmp(&t, M) >= 0) {
            nsub(&t, &t, M);
        }
    } else if (ncmp(&t, M) >= 0) {
        nsub(&t, &t, M);
    }
    ncopy(r, &t);
}

/* r = a - b mod M */
static void nmod_sub(p256n *r, const p256n *a, const p256n *b,
                     const p256n *M) {
    p256n t;
    if (nsub(&t, a, b)) {
        (void)nadd(&t, &t, M);               /* result in [0, M) */
    }
    ncopy(r, &t);
}

/* r = a * b mod M */
static void nmod_mul(p256n *r, const p256n *a, const p256n *b,
                     const p256n *M) {
    p256w prod;
    nmul(&prod, a, b);
    nmod_reduce(r, &prod, M);
}

/* r = a mod M  (a is already a 256-bit value) */
static void nmod_self(p256n *r, const p256n *a, const p256n *M) {
    if (ncmp(a, M) >= 0) {
        p256w w;
        for (int i = 0; i < 4; i++) w.d[i] = a->d[i];
        for (int i = 4; i < 8; i++) w.d[i] = 0;
        nmod_reduce(r, &w, M);
    } else {
        ncopy(r, a);
    }
}

/* r = a^(M-2) mod M  (modular inverse via Fermat's little theorem). */
static void nmod_inv(p256n *r, const p256n *a, const p256n *M) {
    p256n e;
    ncopy(&e, M);
    nsub(&e, &e, &(p256n){ {2,0,0,0} });

    p256n res, base;
    nset1(&res);
    ncopy(&base, a);
    for (int bit = 255; bit >= 0; bit--) {
        nmod_mul(&res, &res, &res, M);       /* res = res^2 */
        if ((e.d[bit >> 6] >> (bit & 63)) & 1)
            nmod_mul(&res, &res, &base, M);
    }
    ncopy(r, &res);
}

/* Parse a big-endian unsigned integer (up to 33 bytes, dropping a single
 * leading 0x00 sign byte) into a 256-bit value.  0 on success, -1 if it
 * does not fit in 256 bits. */
static int nfrom_be(p256n *r, const uint8_t *p, size_t len) {
    nz(r);
    if (len == 0) return -1;
    if (len > 33) return -1;
    if (len == 33 && p[0] != 0x00) return -1;  /* > 256 bits */
    if (len == 33) { p++; len--; }
    for (size_t i = 0; i < len; i++) {
        atls_limb carry = 0;
        for (int j = 0; j < P256_LIMBS; j++) {
            atls_limb v = (r->d[j] << 8) | carry;
            carry = r->d[j] >> 56;
            r->d[j] = v;
        }
        r->d[0] |= p[i];
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* secp256r1 point arithmetic (Jacobian coordinates, a = -3).          */
/* ------------------------------------------------------------------ */

typedef struct {
    p256n X, Y, Z;   /* Z == 0 denotes the point at infinity */
} p256pt;

static void pt_inf(p256pt *p) { nz(&p->X); nz(&p->Y); nz(&p->Z); }
static int pt_is_inf(const p256pt *p) { return niszero(&p->Z); }

static void pt_affine(p256pt *p, const p256n *x, const p256n *y) {
    ncopy(&p->X, x); ncopy(&p->Y, y); nset1(&p->Z);
}

/* Jacobian double: 2P.  Handles P = infinity.
 * All reads of p->* happen before any write to r->*, so it is safe to call
 * with r == p (in-place), as pt_mul does. */
static void pt_double(p256pt *r, const p256pt *p, const p256n *M) {
    if (pt_is_inf(p)) { pt_inf(r); return; }

    p256n XX, YY, YYYY, ZZ, ZZ2, S, Md, E8, t, tZ, X3;
    nmod_mul(&XX, &p->X, &p->X, M);          /* XX = X^2 */
    nmod_mul(&YY, &p->Y, &p->Y, M);          /* YY = Y^2 */
    nmod_mul(&YYYY, &YY, &YY, M);            /* YYYY = Y^4 */
    nmod_mul(&ZZ, &p->Z, &p->Z, M);          /* ZZ = Z^2 */
    nmod_mul(&ZZ2, &ZZ, &ZZ, M);             /* ZZ2 = Z^4 */

    nmod_add(&t, &p->X, &YY, M);
    nmod_mul(&t, &t, &t, M);
    nmod_sub(&t, &t, &XX, M);
    nmod_sub(&t, &t, &YYYY, M);
    nmod_add(&S, &t, &t, M);                 /* S = 2*((X+YY)^2-XX-YYYY) */

    nmod_sub(&t, &XX, &ZZ2, M);
    nmod_add(&Md, &t, &t, M);
    nmod_add(&Md, &Md, &t, M);               /* M = 3*(XX - Z^4) */

    nmod_mul(&X3, &Md, &Md, M);              /* M^2 */
    nmod_sub(&X3, &X3, &S, M);
    nmod_sub(&X3, &X3, &S, M);               /* X3 = M^2 - 2S */

    /* Z3 temp computed now, while p->Y and p->Z are still intact. */
    nmod_add(&tZ, &p->Y, &p->Z, M);
    nmod_mul(&tZ, &tZ, &tZ, M);
    nmod_sub(&tZ, &tZ, &YY, M);
    nmod_sub(&tZ, &tZ, &ZZ, M);              /* Z3 = (Y+Z)^2 - YY - ZZ */

    nmod_sub(&t, &S, &X3, M);
    nmod_mul(&t, &t, &Md, M);                /* M*(S - X3) */
    nmod_add(&E8, &YYYY, &YYYY, M);
    nmod_add(&E8, &E8, &E8, M);
    nmod_add(&E8, &E8, &E8, M);              /* 8*YYYY */
    nmod_sub(&r->Y, &t, &E8, M);             /* Y3 */

    ncopy(&r->X, &X3);
    ncopy(&r->Z, &tZ);
}

/* Jacobian add: r = P + Q.  Handles infinity and P == Q (-> double). */
static void pt_add(p256pt *r, const p256pt *p, const p256pt *q,
                   const p256n *M) {
    if (pt_is_inf(p)) { *r = *q; return; }
    if (pt_is_inf(q)) { *r = *p; return; }

    p256n Z1Z1, Z2Z2, U1, U2, S1, S2, H, Rt, HH, HHH, V;
    nmod_mul(&Z1Z1, &p->Z, &p->Z, M);
    nmod_mul(&Z2Z2, &q->Z, &q->Z, M);

    nmod_mul(&U1, &p->X, &Z2Z2, M);
    nmod_mul(&U2, &q->X, &Z1Z1, M);

    nmod_mul(&S1, &p->Y, &Z2Z2, M);
    nmod_mul(&S1, &S1, &q->Z, M);            /* Y1 * Z2^3 */
    nmod_mul(&S2, &q->Y, &Z1Z1, M);
    nmod_mul(&S2, &S2, &p->Z, M);            /* Y2 * Z1^3 */

    nmod_sub(&H, &U2, &U1, M);
    nmod_sub(&Rt, &S2, &S1, M);

    if (niszero(&H)) {
        if (niszero(&Rt)) { pt_double(r, p, M); return; }
        pt_inf(r); return;
    }

    nmod_mul(&HH, &H, &H, M);
    nmod_mul(&HHH, &HH, &H, M);
    nmod_mul(&V, &U1, &HH, M);

    /* X3 = R^2 - H^3 - 2V */
    nmod_mul(&r->X, &Rt, &Rt, M);
    nmod_sub(&r->X, &r->X, &HHH, M);
    nmod_sub(&r->X, &r->X, &V, M);
    nmod_sub(&r->X, &r->X, &V, M);

    /* Y3 = R*(V - X3) - S1*H^3 */
    {
        p256n a, b;
        nmod_sub(&a, &V, &r->X, M);          /* V - X3 */
        nmod_mul(&a, &a, &Rt, M);            /* R*(V-X3) */
        nmod_mul(&b, &S1, &HHH, M);          /* S1*H^3 */
        nmod_sub(&r->Y, &a, &b, M);
    }

    /* Z3 = Z1 * Z2 * H */
    nmod_mul(&r->Z, &p->Z, &q->Z, M);
    nmod_mul(&r->Z, &r->Z, &H, M);
}

/* r = k * P (double-and-add, left to right). */
static void pt_mul(p256pt *r, const p256n *k, const p256pt *p,
                   const p256n *M) {
    p256pt acc;
    pt_inf(&acc);
    for (int bit = 255; bit >= 0; bit--) {
        if (!pt_is_inf(&acc)) pt_double(&acc, &acc, M);
        if ((k->d[bit >> 6] >> (bit & 63)) & 1)
            pt_add(&acc, &acc, p, M);
    }
    *r = acc;
}

/* Affine x-coordinate of a Jacobian point; -1 if point at infinity. */
static int pt_x_affine(p256n *x, const p256pt *p, const p256n *M) {
    if (pt_is_inf(p)) return -1;
    p256n Z2, Z2inv;
    nmod_mul(&Z2, &p->Z, &p->Z, M);
    nmod_inv(&Z2inv, &Z2, M);                /* Z^-2 */
    nmod_mul(x, &p->X, &Z2inv, M);
    return 0;
}

/* ------------------------------------------------------------------ */
/* DER ECDSA-Sig-Value parsing.                                        */
/* ------------------------------------------------------------------ */

static int parse_der_sig(const uint8_t *der, size_t len,
                         p256n *r, p256n *s) {
    if (!der || len < 8) return -1;
    if (der[0] != 0x30) return -1;
    size_t pos = 1;
    if (der[pos] >= 0x80) return -1;         /* short-form length only */
    size_t seq_len = der[pos];
    pos += 1;
    if (pos + seq_len != len) return -1;

    if (pos + 2 > len || der[pos] != 0x02) return -1;
    size_t rl = der[pos + 1];
    if (rl == 0 || pos + 2 + rl > len) return -1;
    if (nfrom_be(r, der + pos + 2, rl) != 0) return -1;
    pos += 2 + rl;

    if (pos + 2 > len || der[pos] != 0x02) return -1;
    size_t sl = der[pos + 1];
    if (sl == 0 || pos + 2 + sl > len) return -1;
    if (nfrom_be(s, der + pos + 2, sl) != 0) return -1;
    pos += 2 + sl;

    return (pos == len) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */
/* ------------------------------------------------------------------ */

int atls_ecdsa_p256_verify(const uint8_t *sig_der, size_t sig_der_len,
                           const uint8_t pubkey[65],
                           const uint8_t *msg, size_t msg_len) {
    if (!sig_der || !pubkey || !msg || pubkey[0] != 0x04)
        return ATLS_ERR_BAD_ENCODING;

    /* Parse the public key point (0x04 || X || Y). */
    p256n QX, QY;
    if (nfrom_be(&QX, pubkey + 1, 32) != 0) return ATLS_ERR_BAD_ENCODING;
    if (nfrom_be(&QY, pubkey + 33, 32) != 0) return ATLS_ERR_BAD_ENCODING;
    if (ncmp(&QX, &P256_P) >= 0 || ncmp(&QY, &P256_P) >= 0)
        return ATLS_ERR_BAD_ENCODING;

    /* The point must lie on the curve: y^2 == x^3 - 3x + b  (mod p). */
    {
        p256n lhs, rhs, threex;
        nmod_mul(&lhs, &QY, &QY, &P256_P);           /* y^2 */
        nmod_mul(&rhs, &QX, &QX, &P256_P);
        nmod_mul(&rhs, &rhs, &QX, &P256_P);          /* x^3 */
        nmod_add(&threex, &QX, &QX, &P256_P);
        nmod_add(&threex, &threex, &QX, &P256_P);    /* 3x */
        nmod_sub(&rhs, &rhs, &threex, &P256_P);      /* x^3 - 3x */
        nmod_add(&rhs, &rhs, &P256_B, &P256_P);      /* + b */
        if (ncmp(&lhs, &rhs) != 0) return ATLS_ERR_BAD_ENCODING;
    }

    /* Parse r, s and apply the standard range checks. */
    p256n r, s;
    if (parse_der_sig(sig_der, sig_der_len, &r, &s) != 0)
        return ATLS_ERR_BAD_ENCODING;
    if (niszero(&r) || niszero(&s)) return ATLS_ERR_BAD_SIGNATURE;
    if (ncmp(&r, &P256_N) >= 0 || ncmp(&s, &P256_N) >= 0)
        return ATLS_ERR_BAD_SIGNATURE;

    /* e = SHA-256(msg) as a big-endian integer. */
    uint8_t h[32];
    atls_sha256(msg, msg_len, h);
    p256n e;
    if (nfrom_be(&e, h, 32) != 0) return ATLS_ERR_INPUT;

    /* w = s^-1 mod n;  u1 = e*w;  u2 = r*w. */
    p256n winv, u1, u2;
    nmod_inv(&winv, &s, &P256_N);
    nmod_mul(&u1, &e, &winv, &P256_N);
    nmod_mul(&u2, &r, &winv, &P256_N);

    /* R = u1*G + u2*Q */
    p256pt G, Q, R1, R2, R;
    pt_affine(&G, &P256_GX, &P256_GY);
    pt_affine(&Q, &QX, &QY);
    pt_mul(&R1, &u1, &G, &P256_P);
    pt_mul(&R2, &u2, &Q, &P256_P);
    pt_add(&R, &R1, &R2, &P256_P);

    /* v = x(R) mod n; accept iff v == r. */
    p256n xR;
    if (pt_x_affine(&xR, &R, &P256_P) != 0) return ATLS_ERR_BAD_SIGNATURE;
    nmod_self(&xR, &xR, &P256_N);
    return (ncmp(&xR, &r) == 0) ? ATLS_OK : ATLS_ERR_BAD_SIGNATURE;
}

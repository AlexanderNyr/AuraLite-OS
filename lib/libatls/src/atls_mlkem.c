/* atls_mlkem.c — ML-KEM-768 (FIPS 203), REALINTERNET2 Y5.
 *
 * K-PKE + the FO transform, 768-parameter set only (k=3, η1=η2=2,
 * du=10, dv=4).  NTT/invNTT over Z_q[X]/(X^256+1), q=3329, ζ=17.
 * Centered binomial sampling; SHA3/SHAKE via atls_sha3.c.
 *
 * Constant-time on secrets (D7): no secret-dependent branches or
 * table lookups.  The zetas table is public (indexed by the public
 * loop counter).  FO decaps always re-encrypts and cmov's between
 * K' and J(z||c); a corrupted ciphertext is implicit rejection,
 * never an error return.
 */

#include "atls/mlkem.h"
#include <string.h>

#define Q       3329
#define N       256
#define K       3
#define ETA1    2
#define ETA2    2
#define DU      10
#define DV      4
#define POLY_BYTES      384
#define POLYVEC_BYTES   (POLY_BYTES * K)          /* 1152 */
#define C1_BYTES        (32 * DU * K)             /* 960 */
#define C2_BYTES        (32 * DV)                 /* 128 */

/* ζ^{BitRev7(i)} for i=0..127 — public twiddles (FIPS 203 §4.3). */
static const int16_t k_zetas[128] = {
        1,  1729,  2580,  3289,  2642,   630,  1897,   848,
     1062,  1919,   193,   797,  2786,  3260,   569,  1746,
      296,  2447,  1339,  1476,  3046,    56,  2240,  1333,
     1426,  2094,   535,  2882,  2393,  2879,  1974,   821,
      289,   331,  3253,  1756,  1197,  2304,  2277,  2055,
      650,  1977,  2513,   632,  2865,    33,  1320,  1915,
     2319,  1435,   807,   452,  1438,  2868,  1534,  2402,
     2647,  2617,  1481,   648,  2474,  3110,  1227,   910,
       17,  2761,   583,  2649,  1637,   723,  2288,  1100,
     1409,  2662,  3281,   233,   756,  2156,  3015,  3050,
     1703,  1651,  2789,  1789,  1847,   952,  1461,  2687,
      939,  2308,  2437,  2388,   733,  2337,   268,   641,
     1584,  2298,  2037,  3220,   375,  2549,  2090,  1645,
     1063,   319,  2773,   757,  2099,   561,  2466,  2594,
     2804,  1092,   403,  1026,  1143,  2150,  2775,   886,
     1722,  1212,  1874,  1029,  2110,  2935,   885,  2154,
};

/* ζ^{2·BitRev7(i)+1} — BaseCaseMultiply twiddles. */
static const int16_t k_basemul[128] = {
       17,  3312,  2761,   568,   583,  2746,  2649,   680,
     1637,  1692,   723,  2606,  2288,  1041,  1100,  2229,
     1409,  1920,  2662,   667,  3281,    48,   233,  3096,
      756,  2573,  2156,  1173,  3015,   314,  3050,   279,
     1703,  1626,  1651,  1678,  2789,   540,  1789,  1540,
     1847,  1482,   952,  2377,  1461,  1868,  2687,   642,
      939,  2390,  2308,  1021,  2437,   892,  2388,   941,
      733,  2596,  2337,   992,   268,  3061,   641,  2688,
     1584,  1745,  2298,  1031,  2037,  1292,  3220,   109,
      375,  2954,  2549,   780,  2090,  1239,  1645,  1684,
     1063,  2266,   319,  3010,  2773,   556,   757,  2572,
     2099,  1230,   561,  2768,  2466,   863,  2594,   735,
     2804,   525,  1092,  2237,   403,  2926,  1026,  2303,
     1143,  2186,  2150,  1179,  2775,   554,   886,  2443,
     1722,  1607,  1212,  2117,  1874,  1455,  1029,  2300,
     2110,  1219,  2935,   394,   885,  2444,  2154,  1175,
};

typedef int16_t poly[N];
typedef poly polyvec[K];

/* Reduce a product (or a wider sum) into [0, q).  The compiler
 * lowers the constant remainder to a multiply; the sign fix is a
 * mask, not a branch. */
static int16_t fqred(int32_t a) {
    int32_t r = a % Q;
    r += (r >> 31) & Q;
    return (int16_t)r;
}

static int16_t fqadd(int16_t a, int16_t b) {
    return fqred((int32_t)a + (int32_t)b);
}

static int16_t fqsub(int16_t a, int16_t b) {
    return fqred((int32_t)a - (int32_t)b);
}

static int16_t fqmul(int16_t a, int16_t b) {
    return fqred((int32_t)a * (int32_t)b);
}

static void poly_zero(poly r) {
    for (int i = 0; i < N; i++) r[i] = 0;
}

/* FIPS 203 Algorithm 9. */
static void ntt(poly f) {
    int i = 1;
    for (int len = 128; len >= 2; len >>= 1) {
        for (int start = 0; start < N; start += 2 * len) {
            int16_t zeta = k_zetas[i++];
            for (int j = start; j < start + len; j++) {
                int16_t t = fqmul(zeta, f[j + len]);
                f[j + len] = fqsub(f[j], t);
                f[j] = fqadd(f[j], t);
            }
        }
    }
}

/* FIPS 203 Algorithm 10.  3303 = 128^{-1} mod q. */
static void invntt(poly f) {
    int i = 127;
    for (int len = 2; len <= 128; len <<= 1) {
        for (int start = 0; start < N; start += 2 * len) {
            int16_t zeta = k_zetas[i--];
            for (int j = start; j < start + len; j++) {
                int16_t t = f[j];
                f[j] = fqadd(t, f[j + len]);
                f[j + len] = fqmul(zeta, fqsub(f[j + len], t));
            }
        }
    }
    for (int j = 0; j < N; j++) f[j] = fqmul(f[j], 3303);
}

/* FIPS 203 Algorithm 11 / 12. */
static void basemul(int16_t r[2], const int16_t a[2], const int16_t b[2],
                    int16_t gamma) {
    r[0] = fqadd(fqmul(a[0], b[0]), fqmul(fqmul(a[1], b[1]), gamma));
    r[1] = fqadd(fqmul(a[0], b[1]), fqmul(a[1], b[0]));
}

static void poly_basemul(poly r, const poly a, const poly b) {
    for (int i = 0; i < N / 2; i++) {
        int16_t t[2];
        basemul(t, &a[2 * i], &b[2 * i], k_basemul[i]);
        r[2 * i] = t[0];
        r[2 * i + 1] = t[1];
    }
}

static void poly_add(poly r, const poly a, const poly b) {
    for (int i = 0; i < N; i++) r[i] = fqadd(a[i], b[i]);
}

static void poly_sub(poly r, const poly a, const poly b) {
    for (int i = 0; i < N; i++) r[i] = fqsub(a[i], b[i]);
}

/* Compress_d / Decompress_d (FIPS 203 §4.2.1). */
static uint16_t compress_d(int16_t x, int d) {
    uint32_t u = (uint16_t)x;
    uint32_t t = (uint32_t)((((uint64_t)u << d) + (Q / 2)) / Q);
    return (uint16_t)(t & ((1u << d) - 1u));
}

static int16_t decompress_d(uint16_t y, int d) {
    return (int16_t)(((uint32_t)y * Q + (1u << (d - 1))) >> d);
}

/* ByteEncode_d / ByteDecode_d (FIPS 203 Algorithms 5–6). */
static void byte_encode(uint8_t *out, const poly f, int d) {
    int bit = 0;
    memset(out, 0, (size_t)(N * d / 8));
    for (int i = 0; i < N; i++) {
        uint32_t a = (uint16_t)f[i];
        for (int j = 0; j < d; j++) {
            out[bit / 8] |= (uint8_t)((a & 1u) << (bit % 8));
            a >>= 1;
            bit++;
        }
    }
}

static void byte_decode(poly f, const uint8_t *in, int d) {
    int bit = 0;
    for (int i = 0; i < N; i++) {
        uint32_t a = 0;
        for (int j = 0; j < d; j++) {
            a |= (uint32_t)((in[bit / 8] >> (bit % 8)) & 1u) << j;
            bit++;
        }
        f[i] = (int16_t)a;
    }
}

static void poly_compress(uint8_t *out, const poly f, int d) {
    poly t;
    for (int i = 0; i < N; i++) t[i] = (int16_t)compress_d(f[i], d);
    byte_encode(out, t, d);
}

static void poly_decompress(poly f, const uint8_t *in, int d) {
    poly t;
    byte_decode(t, in, d);
    for (int i = 0; i < N; i++) f[i] = decompress_d((uint16_t)t[i], d);
}

static void polyvec_ntt(polyvec v) {
    for (int i = 0; i < K; i++) ntt(v[i]);
}

static void polyvec_invntt(polyvec v) {
    for (int i = 0; i < K; i++) invntt(v[i]);
}

static void polyvec_add(polyvec r, const polyvec a, const polyvec b) {
    for (int i = 0; i < K; i++) poly_add(r[i], a[i], b[i]);
}

static void polyvec_encode12(uint8_t *out, const polyvec v) {
    for (int i = 0; i < K; i++) byte_encode(out + i * POLY_BYTES, v[i], 12);
}

static int polyvec_decode12(polyvec v, const uint8_t *in) {
    int bad = 0;
    for (int i = 0; i < K; i++) {
        byte_decode(v[i], in + i * POLY_BYTES, 12);
        for (int j = 0; j < N; j++) {
            /* Secret-independent: the bit is accumulated, not branched. */
            bad |= (uint16_t)(Q - 1 - v[i][j]) >> 15;
        }
    }
    return bad; /* 0 = all coeffs in 0..q-1 */
}

/* SampleNTT (FIPS 203 Algorithm 7) — public matrix A, rejection OK. */
static void sample_ntt(poly f, const uint8_t rho[32], uint8_t j, uint8_t i) {
    uint8_t seed[34];
    memcpy(seed, rho, 32);
    seed[32] = j;
    seed[33] = i;
    atls_keccak_ctx xof;
    atls_shake128_init(&xof);
    atls_keccak_absorb(&xof, seed, 34);
    atls_keccak_finalize(&xof, 0x1F);
    int off = 0;
    while (off < N) {
        uint8_t buf[3];
        atls_keccak_squeeze(&xof, buf, 3);
        int d1 = buf[0] | ((int)(buf[1] & 0x0F) << 8);
        int d2 = (buf[1] >> 4) | ((int)buf[2] << 4);
        if (d1 < Q) f[off++] = (int16_t)d1;
        if (off < N && d2 < Q) f[off++] = (int16_t)d2;
    }
}

/* SamplePolyCBD_η (FIPS 203 Algorithm 8).  η is 2 for both 768 η's. */
static void sample_cbd2(poly f, const uint8_t *buf /* 128 bytes */) {
    for (int i = 0; i < N; i++) {
        int byte = buf[i / 2];
        int nibble = (i & 1) ? (byte >> 4) : (byte & 0x0F);
        int x = (nibble & 1) + ((nibble >> 1) & 1);
        int y = ((nibble >> 2) & 1) + ((nibble >> 3) & 1);
        f[i] = fqred(x - y);
    }
}

static void prf_cbd2(poly f, const uint8_t s[32], uint8_t nonce) {
    uint8_t in[33], out[128];
    memcpy(in, s, 32);
    in[32] = nonce;
    atls_shake256(in, 33, out, 128);
    sample_cbd2(f, out);
    atls_wipe(out, sizeof out);
}

static void matrix_A(polyvec a[K], const uint8_t rho[32], int transposed) {
    for (int i = 0; i < K; i++) {
        for (int j = 0; j < K; j++) {
            if (transposed) sample_ntt(a[i][j], rho, (uint8_t)i, (uint8_t)j);
            else            sample_ntt(a[i][j], rho, (uint8_t)j, (uint8_t)i);
        }
    }
}

/* Â ◦ ŝ  (NTT-domain matrix-vector). */
static void matvec(polyvec r, const polyvec a[K], const polyvec s) {
    for (int i = 0; i < K; i++) {
        poly acc;
        poly_zero(acc);
        for (int j = 0; j < K; j++) {
            poly t;
            poly_basemul(t, a[i][j], s[j]);
            poly_add(acc, acc, t);
        }
        memcpy(r[i], acc, sizeof(poly));
    }
}

/* t̂ᵀ ◦ ŷ  (row vector × vector, NTT domain) → one poly. */
static void vecdot(poly r, const polyvec a, const polyvec b) {
    poly_zero(r);
    for (int i = 0; i < K; i++) {
        poly t;
        poly_basemul(t, a[i], b[i]);
        poly_add(r, r, t);
    }
}

/* Message ↔ polynomial (ByteDecode_1 / Decompress_1 and inverse). */
static void poly_frommsg(poly f, const uint8_t m[32]) {
    for (int i = 0; i < N; i++) {
        uint16_t bit = (m[i / 8] >> (i % 8)) & 1u;
        f[i] = decompress_d(bit, 1);
    }
}

static void poly_tomsg(uint8_t m[32], const poly f) {
    memset(m, 0, 32);
    for (int i = 0; i < N; i++) {
        uint16_t b = compress_d(f[i], 1);
        m[i / 8] |= (uint8_t)(b << (i % 8));
    }
}

/* ---- K-PKE (FIPS 203 Algorithms 13–15) ---- */

static void kpke_keygen(uint8_t ek[ATLS_MLKEM768_EK_BYTES],
                        uint8_t dk[POLYVEC_BYTES],
                        const uint8_t d[32]) {
    uint8_t seed[33], g[64];
    memcpy(seed, d, 32);
    seed[32] = (uint8_t)K;                 /* domain-separate by k */
    atls_sha3_512(seed, 33, g);
    uint8_t *rho = g;
    uint8_t *sigma = g + 32;

    polyvec a[K], s, e, t;
    matrix_A(a, rho, 0);
    uint8_t n = 0;
    for (int i = 0; i < K; i++) prf_cbd2(s[i], sigma, n++);
    for (int i = 0; i < K; i++) prf_cbd2(e[i], sigma, n++);
    polyvec_ntt(s);
    polyvec_ntt(e);
    matvec(t, a, s);
    polyvec_add(t, t, e);
    polyvec_encode12(ek, t);
    memcpy(ek + POLYVEC_BYTES, rho, 32);
    polyvec_encode12(dk, s);

    atls_wipe(g, sizeof g);
    atls_wipe(seed, sizeof seed);
    atls_wipe(s, sizeof s);
    atls_wipe(e, sizeof e);
}

static void kpke_encrypt(uint8_t c[ATLS_MLKEM768_CT_BYTES],
                         const uint8_t ek[ATLS_MLKEM768_EK_BYTES],
                         const uint8_t m[32],
                         const uint8_t r[32]) {
    polyvec t, y, e1, u, a[K];
    poly e2, v, mu;
    uint8_t rho[32];
    polyvec_decode12(t, ek);               /* KAT keys are valid */
    memcpy(rho, ek + POLYVEC_BYTES, 32);
    matrix_A(a, rho, 1);                   /* Âᵀ */
    uint8_t n = 0;
    for (int i = 0; i < K; i++) prf_cbd2(y[i], r, n++);
    for (int i = 0; i < K; i++) prf_cbd2(e1[i], r, n++);
    prf_cbd2(e2, r, n);
    polyvec_ntt(y);
    matvec(u, a, y);
    polyvec_invntt(u);
    polyvec_add(u, u, e1);
    vecdot(v, t, y);
    invntt(v);
    poly_frommsg(mu, m);
    poly_add(v, v, e2);
    poly_add(v, v, mu);
    for (int i = 0; i < K; i++)
        poly_compress(c + i * (32 * DU), u[i], DU);
    poly_compress(c + C1_BYTES, v, DV);
    atls_wipe(y, sizeof y);
}

static void kpke_decrypt(uint8_t m[32],
                         const uint8_t dk[POLYVEC_BYTES],
                         const uint8_t c[ATLS_MLKEM768_CT_BYTES]) {
    polyvec u, s;
    poly v, w;
    for (int i = 0; i < K; i++)
        poly_decompress(u[i], c + i * (32 * DU), DU);
    poly_decompress(v, c + C1_BYTES, DV);
    polyvec_decode12(s, dk);
    polyvec_ntt(u);
    vecdot(w, s, u);
    invntt(w);
    poly_sub(w, v, w);
    poly_tomsg(m, w);
}

static void cmov(uint8_t *r, const uint8_t *x, size_t n, uint8_t b) {
    uint8_t mask = (uint8_t)(- (int)(b & 1u));
    for (size_t i = 0; i < n; i++)
        r[i] ^= (uint8_t)(mask & (r[i] ^ x[i]));
}

/* ---- ML-KEM.KeyGen / Encaps / Decaps_internal (FIPS 203 §6) ---- */

int atls_mlkem768_keygen(uint8_t ek[ATLS_MLKEM768_EK_BYTES],
                         uint8_t dk[ATLS_MLKEM768_DK_BYTES],
                         const uint8_t d[ATLS_MLKEM768_SEED_BYTES],
                         const uint8_t z[ATLS_MLKEM768_SEED_BYTES]) {
    if (!ek || !dk || !d || !z) return ATLS_ERR_INPUT;
    uint8_t dk_pke[POLYVEC_BYTES];
    kpke_keygen(ek, dk_pke, d);
    memcpy(dk, dk_pke, POLYVEC_BYTES);
    memcpy(dk + POLYVEC_BYTES, ek, ATLS_MLKEM768_EK_BYTES);
    atls_sha3_256(ek, ATLS_MLKEM768_EK_BYTES, dk + POLYVEC_BYTES + ATLS_MLKEM768_EK_BYTES);
    memcpy(dk + POLYVEC_BYTES + ATLS_MLKEM768_EK_BYTES + 32, z, 32);
    atls_wipe(dk_pke, sizeof dk_pke);
    return ATLS_OK;
}

int atls_mlkem768_encaps(uint8_t ct[ATLS_MLKEM768_CT_BYTES],
                         uint8_t ss[ATLS_MLKEM768_SS_BYTES],
                         const uint8_t ek[ATLS_MLKEM768_EK_BYTES],
                         const uint8_t m[ATLS_MLKEM768_SEED_BYTES]) {
    if (!ct || !ss || !ek || !m) return ATLS_ERR_INPUT;
    polyvec t;
    if (polyvec_decode12(t, ek) != 0) return ATLS_ERR_BAD_ENCODING;
    uint8_t hek[32], g_in[64], g_out[64];
    atls_sha3_256(ek, ATLS_MLKEM768_EK_BYTES, hek);
    memcpy(g_in, m, 32);
    memcpy(g_in + 32, hek, 32);
    atls_sha3_512(g_in, 64, g_out);        /* (K, r) ← G(m || H(ek)) */
    memcpy(ss, g_out, 32);
    kpke_encrypt(ct, ek, m, g_out + 32);
    atls_wipe(g_in, sizeof g_in);
    atls_wipe(g_out, sizeof g_out);
    return ATLS_OK;
}

int atls_mlkem768_decaps(uint8_t ss[ATLS_MLKEM768_SS_BYTES],
                         const uint8_t ct[ATLS_MLKEM768_CT_BYTES],
                         const uint8_t dk[ATLS_MLKEM768_DK_BYTES]) {
    if (!ss || !ct || !dk) return ATLS_ERR_INPUT;
    const uint8_t *dk_pke = dk;
    const uint8_t *ek     = dk + POLYVEC_BYTES;
    const uint8_t *h      = dk + POLYVEC_BYTES + ATLS_MLKEM768_EK_BYTES;
    const uint8_t *z      = h + 32;

    uint8_t mp[32], g_in[64], g_out[64], c2[ATLS_MLKEM768_CT_BYTES];
    uint8_t j_in[32 + ATLS_MLKEM768_CT_BYTES], kbar[32];
    kpke_decrypt(mp, dk_pke, ct);
    memcpy(g_in, mp, 32);
    memcpy(g_in + 32, h, 32);
    atls_sha3_512(g_in, 64, g_out);        /* (K', r') ← G(m' || h) */
    memcpy(j_in, z, 32);
    memcpy(j_in + 32, ct, ATLS_MLKEM768_CT_BYTES);
    atls_shake256(j_in, sizeof j_in, kbar, 32);  /* K̄ ← J(z || c) */
    kpke_encrypt(c2, ek, mp, g_out + 32);
    /* b = 1 if ct == c2, else 0.  Always copy K' then cmov K̄. */
    int eq = atls_ct_eq(ct, c2, ATLS_MLKEM768_CT_BYTES);
    memcpy(ss, g_out, 32);
    cmov(ss, kbar, 32, (uint8_t)(1 - eq));
    atls_wipe(mp, sizeof mp);
    atls_wipe(g_in, sizeof g_in);
    atls_wipe(g_out, sizeof g_out);
    atls_wipe(kbar, sizeof kbar);
    atls_wipe(c2, sizeof c2);
    return ATLS_OK;
}

/* atls_rsa.c — RSA PKCS#1v1.5 verification (INTERNET_PLAN.md N5).
 *
 * Verification only.  Bignum arithmetic with 32-bit limbs and 64-bit
 * intermediates.  Not constant-time (public-key operations on public
 * data — D7 applies to secret comparisons, not RSA verify).
 *
 * The implementation is deliberately simple and correct rather than
 * fast: modular exponentiation uses binary square-and-multiply, and
 * modular reduction uses schoolbook division.  2048-bit RSA verify
 * takes ~2ms on the host — acceptable for a hobby OS.
 */

#include "atls_rsa.h"
#include "atls/atls.h"
#include <string.h>

/* ---- Bignum primitives ---- */

void atls_bn_zero(atls_bignum *a) {
    for (int i = 0; i < ATLS_RSA_MAX_LIMBS; i++) a->v[i] = 0;
    a->used = 0;
}

void atls_bn_from_bytes(atls_bignum *a, const uint8_t *data, size_t len) {
    atls_bn_zero(a);
    /* Big-endian to little-endian limbs. */
    int limb = 0;
    for (int i = (int)len - 1; i >= 0; i -= 4) {
        uint32_t val = 0;
        for (int j = 0; j < 4 && i - j >= 0; j++) {
            val |= (uint32_t)data[i - j] << (j * 8);
        }
        a->v[limb++] = val;
    }
    a->used = limb;
    /* Trim leading zeros. */
    while (a->used > 0 && a->v[a->used - 1] == 0) a->used--;
}

int atls_bn_cmp(const atls_bignum *a, const atls_bignum *b) {
    int ua = a->used, ub = b->used;
    while (ua > 0 && a->v[ua - 1] == 0) ua--;
    while (ub > 0 && b->v[ub - 1] == 0) ub--;
    if (ua != ub) return ua < ub ? -1 : 1;
    for (int i = ua - 1; i >= 0; i--) {
        if (a->v[i] != b->v[i])
            return a->v[i] < b->v[i] ? -1 : 1;
    }
    return 0;
}

int atls_bn_is_zero(const atls_bignum *a) {
    for (int i = 0; i < a->used; i++)
        if (a->v[i]) return 0;
    return 1;
}

/* r = a - b (assumes a >= b). */
static void bn_sub(atls_bignum *r, const atls_bignum *a, const atls_bignum *b) {
    int64_t borrow = 0;
    for (int i = 0; i < a->used; i++) {
        int64_t diff = (int64_t)a->v[i] - (i < b->used ? b->v[i] : 0) - borrow;
        if (diff < 0) { diff += ((int64_t)1 << 32); borrow = 1; }
        else borrow = 0;
        r->v[i] = (uint32_t)diff;
    }
    r->used = a->used;
    while (r->used > 0 && r->v[r->used - 1] == 0) r->used--;
}

/* r = a * b (schoolbook, result up to a->used + b->used limbs). */
static void bn_mul(atls_bignum *r, const atls_bignum *a, const atls_bignum *b) {
    atls_bn_zero(r);
    for (int i = 0; i < a->used; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < b->used || carry; j++) {
            uint64_t prod = (uint64_t)r->v[i + j]
                          + (uint64_t)a->v[i] * (j < b->used ? b->v[j] : 0)
                          + carry;
            r->v[i + j] = (uint32_t)prod;
            carry = prod >> 32;
        }
    }
    r->used = a->used + b->used;
    while (r->used > 0 && r->v[r->used - 1] == 0) r->used--;
}

/* r = a mod m (shift-and-subtract from MSB of a to LSB).
 * The loop processes ALL bits of a, not just m_bits bits. */
static void bn_mod(atls_bignum *r, const atls_bignum *a, const atls_bignum *m) {
    if (atls_bn_cmp(a, m) < 0) {
        *r = *a;
        return;
    }

    /* Find the highest set bit of a (the dividend). */
    int a_bits = 0;
    for (int i = a->used - 1; i >= 0; i--) {
        if (a->v[i]) {
            for (int b = 31; b >= 0; b--) {
                if (a->v[i] & (1u << b)) {
                    a_bits = i * 32 + b + 1;
                    goto found_a;
                }
            }
        }
    }
found_a:

    atls_bn_zero(r);

    for (int bit = a_bits - 1; bit >= 0; bit--) {
        /* Shift r left by 1, OR in the current bit of a. */
        uint64_t carry = 0;
        for (int j = 0; j <= r->used; j++) {
            uint64_t val = (uint64_t)r->v[j] * 2 + carry;
            r->v[j] = (uint32_t)val;
            carry = val >> 32;
        }
        if (r->used == 0 && carry == 0) { /* keep used */ }
        r->used++;
        while (r->used > 0 && r->v[r->used - 1] == 0) r->used--;

        /* Add the current bit from a. */
        int word = bit / 32;
        int bit_in_word = bit % 32;
        if (word < a->used && (a->v[word] & (1u << bit_in_word))) {
            uint64_t c = r->v[0] + 1;
            r->v[0] = (uint32_t)c;
            c >>= 32;
            for (int j = 1; c && j <= r->used; j++) {
                uint64_t val = (uint64_t)r->v[j] + c;
                r->v[j] = (uint32_t)val;
                c = val >> 32;
            }
            if (c) { r->v[r->used] = (uint32_t)c; r->used++; }
        }

        /* If r >= m, subtract m. */
        if (atls_bn_cmp(r, m) >= 0) {
            bn_sub(r, r, m);
        }
    }
}

/* r = (a * b) mod m. */
static void bn_mul_mod(atls_bignum *r, const atls_bignum *a,
                       const atls_bignum *b, const atls_bignum *m) {
    atls_bignum prod;
    bn_mul(&prod, a, b);
    bn_mod(r, &prod, m);
}

/* r = base^exp mod m (binary square-and-multiply, left-to-right). */
void atls_bn_mod_exp(atls_bignum *r, const atls_bignum *base,
                     const atls_bignum *exp, const atls_bignum *m) {
    atls_bignum result;
    atls_bn_zero(&result);
    result.v[0] = 1;
    result.used = 1;

    atls_bignum base_mod;
    bn_mod(&base_mod, base, m);

    /* Find highest bit of exp. */
    int max_bit = 0;
    for (int i = exp->used - 1; i >= 0; i--) {
        if (exp->v[i]) {
            for (int b = 31; b >= 0; b--) {
                if (exp->v[i] & (1u << b)) {
                    max_bit = i * 32 + b;
                    goto found;
                }
            }
        }
    }
found:

    for (int bit = max_bit; bit >= 0; bit--) {
        /* Square. */
        bn_mul_mod(&result, &result, &result, m);
        /* Multiply if bit is set. */
        int word = bit / 32;
        int bit_in_word = bit % 32;
        if (word < exp->used && (exp->v[word] & (1u << bit_in_word))) {
            bn_mul_mod(&result, &result, &base_mod, m);
        }
    }
    *r = result;
}

/* ---- RSA PKCS#1v1.5 verification ---- */

/* SHA-256 DigestInfo prefix (DER-encoded OID + NULL). */
static const uint8_t sha256_digestinfo[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
    0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
    0x00, 0x04, 0x20
};

int atls_rsa_verify_pkcs1v15(const uint8_t *sig, size_t sig_len,
                             const uint8_t *msg, size_t msg_len,
                             const uint8_t *n_bytes, size_t n_len,
                             const uint8_t *e_bytes, size_t e_len) {
    if (!sig || !msg || !n_bytes || !e_bytes) return ATLS_ERR_INPUT;
    if (sig_len == 0 || n_len == 0 || e_len == 0) return ATLS_ERR_INPUT;

    /* Parse modulus and exponent. */
    atls_bignum n, e, s;
    atls_bn_from_bytes(&n, n_bytes, n_len);
    atls_bn_from_bytes(&e, e_bytes, e_len);
    atls_bn_from_bytes(&s, sig, sig_len);

    if (atls_bn_is_zero(&n) || atls_bn_is_zero(&e))
        return ATLS_ERR_INPUT;
    if (atls_bn_cmp(&s, &n) >= 0)
        return ATLS_ERR_BAD_SIGNATURE; /* s must be < n */

    /* s^e mod n = the PKCS#1v1.5 encoded message. */
    atls_bignum em;
    atls_bn_mod_exp(&em, &s, &e, &n);

    /* Convert to bytes for comparison (big-endian). */
    uint8_t em_bytes[ATLS_RSA_MAX_BITS / 8];
    memset(em_bytes, 0, sizeof(em_bytes));
    int em_bytes_len = n_len; /* same length as modulus */
    /* Write limbs in big-endian order: limb[used-1] is the most
     * significant, limb[0] is the least significant.
     * Each limb is 4 bytes, little-endian internally. */
    for (int i = 0; i < em.used; i++) {
        /* limb i goes to bytes [em_bytes_len - 4*(i+1) .. em_bytes_len - 4*i - 1] */
        int start = em_bytes_len - 4 * (i + 1);
        if (start < 0) break; /* should not happen if n_len >= em size */
        em_bytes[start + 0] = (em.v[i] >> 24) & 0xFF;
        em_bytes[start + 1] = (em.v[i] >> 16) & 0xFF;
        em_bytes[start + 2] = (em.v[i] >>  8) & 0xFF;
        em_bytes[start + 3] = (em.v[i] >>  0) & 0xFF;
    }

    /* Verify PKCS#1v1.5 padding: 0x00 0x01 [0xff...] 0x00 DigestInfo hash */
    size_t pos = 0;
    if (pos >= (size_t)em_bytes_len || em_bytes[pos] != 0x00) return ATLS_ERR_BAD_SIGNATURE;
    pos++;
    if (pos >= (size_t)em_bytes_len || em_bytes[pos] != 0x01) return ATLS_ERR_BAD_SIGNATURE;
    pos++;
    /* Skip 0xff padding bytes. */
    size_t ff_start = pos;
    while (pos < (size_t)em_bytes_len && em_bytes[pos] == 0xFF) pos++;
    if (pos - ff_start < 8) {
        return ATLS_ERR_BAD_SIGNATURE; /* minimum 8 bytes of 0xff */
    }
    if (pos >= (size_t)em_bytes_len || em_bytes[pos] != 0x00) {
        return ATLS_ERR_BAD_SIGNATURE;
    }
    pos++;
    /* DigestInfo prefix. */
    size_t di_len = sizeof(sha256_digestinfo);
    if (pos + di_len + 32 > (size_t)em_bytes_len) return ATLS_ERR_BAD_SIGNATURE;
    for (size_t i = 0; i < di_len; i++) {
        if (em_bytes[pos + i] != sha256_digestinfo[i]) return ATLS_ERR_BAD_SIGNATURE;
    }
    pos += di_len;
    /* Compare the hash. */
    uint8_t hash[32];
    atls_sha256(msg, msg_len, hash);
    for (int i = 0; i < 32; i++) {
        if (em_bytes[pos + i] != hash[i]) return ATLS_ERR_BAD_SIGNATURE;
    }

    return ATLS_OK;
}

/* ---- SPKI / RSAPublicKey parse ---- */

static int der_take_len(const uint8_t *p, size_t len, size_t *pos, size_t *out) {
    if (*pos >= len) return -1;
    uint8_t b = p[(*pos)++];
    if (b < 0x80) {
        *out = b;
        return 0;
    }
    int n = b & 0x7f;
    if (n == 0 || n > 3 || *pos + (size_t)n > len) return -1;
    size_t L = 0;
    for (int i = 0; i < n; i++) L = (L << 8) | p[(*pos)++];
    *out = L;
    return 0;
}

int atls_rsa_parse_spki(const uint8_t *key, size_t key_len,
                        const uint8_t **n_out, size_t *n_len,
                        const uint8_t **e_out, size_t *e_len) {
    if (!key || !n_out || !n_len || !e_out || !e_len) return ATLS_ERR_INPUT;
    if (key_len < 4 || key[0] != 0x30) return ATLS_ERR_BAD_ENCODING;
    size_t pos = 1, seq_len = 0;
    if (der_take_len(key, key_len, &pos, &seq_len) != 0) return ATLS_ERR_BAD_ENCODING;
    if (pos + seq_len > key_len) return ATLS_ERR_BAD_ENCODING;
    size_t end = pos + seq_len;

    if (pos >= end || key[pos] != 0x02) return ATLS_ERR_BAD_ENCODING;
    pos++;
    size_t nl = 0;
    if (der_take_len(key, end, &pos, &nl) != 0) return ATLS_ERR_BAD_ENCODING;
    if (pos + nl > end || nl == 0) return ATLS_ERR_BAD_ENCODING;
    const uint8_t *n = key + pos;
    pos += nl;
    if (n[0] == 0x00 && nl > 1) { n++; nl--; }

    if (pos >= end || key[pos] != 0x02) return ATLS_ERR_BAD_ENCODING;
    pos++;
    size_t el = 0;
    if (der_take_len(key, end, &pos, &el) != 0) return ATLS_ERR_BAD_ENCODING;
    if (pos + el > end || el == 0) return ATLS_ERR_BAD_ENCODING;
    const uint8_t *e = key + pos;
    if (e[0] == 0x00 && el > 1) { e++; el--; }

    *n_out = n; *n_len = nl;
    *e_out = e; *e_len = el;
    return ATLS_OK;
}

/* ---- RSA-PSS-SHA256 (RFC 8017 EMSA-PSS-VERIFY, TLS 1.3 0x0804) ---- */

static void mgf1_sha256(const uint8_t *seed, size_t seed_len,
                        uint8_t *out, size_t out_len) {
    uint8_t hash[32], ctr[4];
    size_t off = 0;
    uint32_t c = 0;
    while (off < out_len) {
        ctr[0] = (uint8_t)(c >> 24);
        ctr[1] = (uint8_t)(c >> 16);
        ctr[2] = (uint8_t)(c >> 8);
        ctr[3] = (uint8_t)c;
        atls_sha256_ctx ctx;
        atls_sha256_init(&ctx);
        atls_sha256_update(&ctx, seed, seed_len);
        atls_sha256_update(&ctx, ctr, 4);
        atls_sha256_final(&ctx, hash);
        size_t take = out_len - off;
        if (take > 32) take = 32;
        for (size_t i = 0; i < take; i++) out[off + i] = hash[i];
        off += take;
        c++;
    }
}

static int n_bitlen(const uint8_t *n, size_t n_len) {
    size_t i = 0;
    while (i < n_len && n[i] == 0) i++;
    if (i == n_len) return 0;
    int bits = (int)((n_len - i) * 8);
    uint8_t b = n[i];
    while (b && (b & 0x80) == 0) { bits--; b = (uint8_t)(b << 1); }
    return bits;
}

static int rsa_public_em(uint8_t *em, size_t em_len,
                         const uint8_t *sig, size_t sig_len,
                         const uint8_t *n_bytes, size_t n_len,
                         const uint8_t *e_bytes, size_t e_len) {
    atls_bignum n, e, s, m;
    atls_bn_from_bytes(&n, n_bytes, n_len);
    atls_bn_from_bytes(&e, e_bytes, e_len);
    atls_bn_from_bytes(&s, sig, sig_len);
    if (atls_bn_is_zero(&n) || atls_bn_is_zero(&e)) return ATLS_ERR_INPUT;
    if (atls_bn_cmp(&s, &n) >= 0) return ATLS_ERR_BAD_SIGNATURE;
    atls_bn_mod_exp(&m, &s, &e, &n);
    for (size_t i = 0; i < em_len; i++) em[i] = 0;
    for (int i = 0; i < m.used; i++) {
        int start = (int)em_len - 4 * (i + 1);
        if (start < 0) break;
        em[start + 0] = (uint8_t)(m.v[i] >> 24);
        em[start + 1] = (uint8_t)(m.v[i] >> 16);
        em[start + 2] = (uint8_t)(m.v[i] >> 8);
        em[start + 3] = (uint8_t)(m.v[i]);
    }
    return ATLS_OK;
}

int atls_rsa_verify_pss_sha256(const uint8_t *sig, size_t sig_len,
                               const uint8_t *msg, size_t msg_len,
                               const uint8_t *n_bytes, size_t n_len,
                               const uint8_t *e_bytes, size_t e_len) {
    if (!sig || !msg || !n_bytes || !e_bytes) return ATLS_ERR_INPUT;
    if (sig_len == 0 || n_len == 0 || e_len == 0) return ATLS_ERR_INPUT;
    if (n_len > ATLS_RSA_MAX_BITS / 8) return ATLS_ERR_INPUT;

    uint8_t em[ATLS_RSA_MAX_BITS / 8];
    int rc = rsa_public_em(em, n_len, sig, sig_len, n_bytes, n_len,
                           e_bytes, e_len);
    if (rc != ATLS_OK) return rc;

    int modBits = n_bitlen(n_bytes, n_len);
    if (modBits < 8 * 32 + 16) return ATLS_ERR_BAD_SIGNATURE;
    int emBits = modBits - 1;
    size_t emLen = (size_t)((emBits + 7) / 8);
    const uint8_t *EM = em;
    if (emLen == n_len) {
        EM = em;
    } else if (emLen + 1 == n_len) {
        if (em[0] != 0) return ATLS_ERR_BAD_SIGNATURE;
        EM = em + 1;
    } else {
        return ATLS_ERR_BAD_SIGNATURE;
    }

    const size_t hLen = 32, sLen = 32;
    if (emLen < hLen + sLen + 2) return ATLS_ERR_BAD_SIGNATURE;
    if (EM[emLen - 1] != 0xbc) return ATLS_ERR_BAD_SIGNATURE;

    size_t dbLen = emLen - hLen - 1;
    const uint8_t *maskedDB = EM;
    const uint8_t *H = EM + dbLen;

    int unused = 8 * (int)emLen - emBits;
    if (unused < 0 || unused > 7) return ATLS_ERR_BAD_SIGNATURE;
    if (unused && (maskedDB[0] & (uint8_t)(0xFFu << (8 - unused))))
        return ATLS_ERR_BAD_SIGNATURE;

    uint8_t dbMask[ATLS_RSA_MAX_BITS / 8];
    uint8_t DB[ATLS_RSA_MAX_BITS / 8];
    mgf1_sha256(H, hLen, dbMask, dbLen);
    for (size_t i = 0; i < dbLen; i++) DB[i] = (uint8_t)(maskedDB[i] ^ dbMask[i]);
    if (unused) DB[0] = (uint8_t)(DB[0] & (0xFFu >> unused));

    size_t ps_len = dbLen - sLen - 1;
    for (size_t i = 0; i < ps_len; i++) {
        if (DB[i] != 0x00) return ATLS_ERR_BAD_SIGNATURE;
    }
    if (DB[ps_len] != 0x01) return ATLS_ERR_BAD_SIGNATURE;
    const uint8_t *salt = DB + ps_len + 1;

    uint8_t mHash[32];
    atls_sha256(msg, msg_len, mHash);
    uint8_t Mp[8 + 32 + 32];
    for (int i = 0; i < 8; i++) Mp[i] = 0;
    for (int i = 0; i < 32; i++) Mp[8 + i] = mHash[i];
    for (int i = 0; i < 32; i++) Mp[40 + i] = salt[i];
    uint8_t Hp[32];
    atls_sha256(Mp, 72, Hp);
    if (!atls_ct_eq(Hp, H, 32)) return ATLS_ERR_BAD_SIGNATURE;
    return ATLS_OK;
}

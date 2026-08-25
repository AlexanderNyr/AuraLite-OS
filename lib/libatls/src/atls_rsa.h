#ifndef LIBATLS_ATLS_RSA_H
#define LIBATLS_ATLS_RSA_H

/* atls_rsa.h — RSA PKCS#1v1.5 verification (INTERNET_PLAN.md N5).
 *
 * Verification only — no signing, no key generation, no decryption.
 * Bignum arithmetic with 32-bit limbs and 64-bit intermediates.
 * Not constant-time (public-key operations on public data; D7 applies
 * to secret comparisons, not to RSA verify).
 */

#include <stdint.h>
#include <stddef.h>

/* Maximum RSA modulus size in bits (2048 is the common case;
 * 4096-bit support is required for ISRG Root YR / X1).  Limb count
 * is doubled so a 4096×4096 schoolbook product fits — the first
 * 4096-bit verify wrote past v[128] and #GP'd the guest. */
#define ATLS_RSA_MAX_BITS 4096
#define ATLS_RSA_MAX_LIMBS ((ATLS_RSA_MAX_BITS / 32) * 2)

/* Big unsigned integer: limbs[0] is least significant. */
typedef struct {
    uint32_t v[ATLS_RSA_MAX_LIMBS];
    int      used;  /* number of non-zero limbs */
} atls_bignum;

void atls_bn_zero(atls_bignum *a);
void atls_bn_from_bytes(atls_bignum *a, const uint8_t *data, size_t len);
int  atls_bn_cmp(const atls_bignum *a, const atls_bignum *b);
int  atls_bn_is_zero(const atls_bignum *a);

/* Modular operations. */
void atls_bn_mod_exp(atls_bignum *result,
                     const atls_bignum *base,
                     const atls_bignum *exp,
                     const atls_bignum *mod);

/* RSA PKCS#1v1.5 verification.
 * Returns 0 on success, negative on failure.
 * sig: raw signature bytes (big-endian, sig_len bytes)
 * msg: the signed message (typically TBS certificate DER)
 * msg_len: length of msg
 * n_bytes, n_len: RSA modulus (big-endian)
 * e_bytes, e_len: RSA public exponent (big-endian, usually 65537 = 3 bytes)
 */
int atls_rsa_verify_pkcs1v15(const uint8_t *sig, size_t sig_len,
                             const uint8_t *msg, size_t msg_len,
                             const uint8_t *n_bytes, size_t n_len,
                             const uint8_t *e_bytes, size_t e_len);

/* RSA-PSS-RSAE-SHA256 (RFC 8017 EMSA-PSS, MGF1-SHA-256, saltLen=32).
 * TLS 1.3 CertificateVerify scheme 0x0804. */
int atls_rsa_verify_pss_sha256(const uint8_t *sig, size_t sig_len,
                               const uint8_t *msg, size_t msg_len,
                               const uint8_t *n_bytes, size_t n_len,
                               const uint8_t *e_bytes, size_t e_len);

/* Parse RSAPublicKey (SPKI BIT STRING contents) into n and e
 * (pointers into `key`, leading 0x00 sign bytes stripped). */
int atls_rsa_parse_spki(const uint8_t *key, size_t key_len,
                        const uint8_t **n_out, size_t *n_len,
                        const uint8_t **e_out, size_t *e_len);

#endif /* LIBATLS_ATLS_RSA_H */

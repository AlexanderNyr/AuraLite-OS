#ifndef LIBATLS_ATLS_H
#define LIBATLS_ATLS_H

/* atls/atls.h — AuraLite TLS crypto primitives (INTERNET_PLAN.md phase N1).
 *
 * A freestanding C11 userspace library (decision D2: crypto lives in a
 * process the kernel can kill, never in the kernel itself).  Portable
 * 64-bit C, no libc beyond <stdint.h>/<stddef.h>, no SSE intrinsics, and
 * no memcmp on secret data anywhere (decision D7 — the only secret
 * comparison primitive is atls_ct_eq, and the test suite greps the sources
 * to prove nothing else is used).
 *
 * Implemented, each verified against its RFC test vectors by
 * tests/unit/test_atls_*.c (the host tests compile these very sources):
 *
 *   SHA-256            FIPS 180-4
 *   SHA-512            FIPS 180-4          (needed by Ed25519)
 *   HMAC-SHA256        RFC 2104 / 4231
 *   HKDF               RFC 5869 (SHA-256)
 *   ChaCha20           RFC 8439 section 2.4
 *   Poly1305           RFC 8439 section 2.5
 *   AEAD               RFC 8439 section 2.8 (ChaCha20-Poly1305)
 *   X25519             RFC 7748
 *   Ed25519 verify     RFC 8032 (verification only, per D4: no signing,
 *                      no key generation — certificate chains need verify)
 *
 * Not implemented here: RSA-PKCS#1v1.5 verification lands with certificate
 * validation (phase N5), and the TLS protocol itself is N3/N4.
 *
 * This is unaudited hobby cryptography.  N9 will say that in the docs
 * rather than implying a security guarantee.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- result codes ---- */

#define ATLS_OK                    0
#define ATLS_ERR_INPUT            -1   /* NULL/out-of-range argument */
#define ATLS_ERR_BAD_ENCODING     -2   /* point/integer decoding failed */
#define ATLS_ERR_BAD_SIGNATURE    -3   /* decoded fine, did not verify */
#define ATLS_ERR_LOW_ORDER        -4   /* X25519 shared secret is zero */
#define ATLS_ERR_AUTH             -5   /* AEAD tag mismatch */
/* ASN.1 / X.509 (phase N2): refusals are specific so a test can assert
 * the REASON, not just the fact. */
#define ATLS_ERR_TRUNCATED        -6   /* input ended mid-structure */
#define ATLS_ERR_BAD_LENGTH       -7   /* length not DER / exceeds buffer */
#define ATLS_ERR_DEPTH            -8   /* nesting beyond ATLS_DER_MAX_DEPTH */
#define ATLS_ERR_UNSUPPORTED      -9   /* version/extension/structure refused on purpose */

/* ---- constant-time utilities (D7) ---- */

/* Compare two buffers without an early exit.  Returns 1 if equal, 0 if
 * not.  This is the ONLY comparison primitive libatls may use on secret
 * or attacker-influenced MAC/signature material. */
int atls_ct_eq(const uint8_t *a, const uint8_t *b, size_t len);

/* Overwrite `len` bytes at `p` with zeros, defeat dead-store elimination. */
void atls_wipe(void *p, size_t len);

/* ---- SHA-256 (FIPS 180-4) ---- */

typedef struct {
    uint32_t h[8];
    uint64_t nbytes;          /* total bytes hashed so far */
    uint8_t  buf[64];
    size_t   buflen;
} atls_sha256_ctx;

void atls_sha256_init(atls_sha256_ctx *c);
void atls_sha256_update(atls_sha256_ctx *c, const void *data, size_t len);
void atls_sha256_final(atls_sha256_ctx *c, uint8_t out[32]);
void atls_sha256(const void *data, size_t len, uint8_t out[32]);

/* ---- SHA-512 (FIPS 180-4) ---- */

typedef struct {
    uint64_t h[8];
    uint64_t nbytes_lo;       /* total bytes hashed, low 64 bits */
    uint64_t nbytes_hi;
    uint8_t  buf[128];
    size_t   buflen;
} atls_sha512_ctx;

void atls_sha512_init(atls_sha512_ctx *c);
void atls_sha512_update(atls_sha512_ctx *c, const void *data, size_t len);
void atls_sha512_final(atls_sha512_ctx *c, uint8_t out[64]);
void atls_sha512(const void *data, size_t len, uint8_t out[64]);

/* ---- HMAC-SHA256 (RFC 2104) ---- */

typedef struct {
    atls_sha256_ctx inner;
    uint8_t opad_hash_prefix[64];   /* pre-padded outer key block */
} atls_hmac_sha256_ctx;

void atls_hmac_sha256_init(atls_hmac_sha256_ctx *c,
                           const uint8_t *key, size_t keylen);
void atls_hmac_sha256_update(atls_hmac_sha256_ctx *c,
                             const void *data, size_t len);
void atls_hmac_sha256_final(atls_hmac_sha256_ctx *c, uint8_t out[32]);
void atls_hmac_sha256(const uint8_t *key, size_t keylen,
                      const void *msg, size_t msglen, uint8_t out[32]);

/* ---- HKDF (RFC 5869, SHA-256) ---- */

/* PRK = HKDF-Extract(salt, IKM).  A NULL/0 salt means HashLen zeros. */
int atls_hkdf_extract(const uint8_t *salt, size_t saltlen,
                      const uint8_t *ikm, size_t ikmlen,
                      uint8_t prk[32]);

/* OKM = HKDF-Expand(PRK, info, L).  L must be <= 255 * 32. */
int atls_hkdf_expand(const uint8_t prk[32],
                     const uint8_t *info, size_t infolen,
                     uint8_t *okm, size_t okmlen);

/* ---- ChaCha20 (RFC 8439 section 2.4) ---- */

/* XOR `len` bytes of `in` with the keystream starting at 32-bit block
 * `counter` (encryption and decryption are the same call). */
void atls_chacha20_xor(const uint8_t key[32], uint32_t counter,
                       const uint8_t nonce[12],
                       const uint8_t *in, uint8_t *out, size_t len);

/* ---- Poly1305 (RFC 8439 section 2.5) ---- */

/* One-time authenticator.  The 32-byte key must never be reused. */
void atls_poly1305(const uint8_t key[32],
                   const uint8_t *msg, size_t msglen,
                   uint8_t tag[16]);

/* ---- AEAD_CHACHA20_POLY1305 (RFC 8439 section 2.8) ---- */

/* `ct` may equal `pt` (in place).  Writes ptlen ciphertext bytes and a
 * 16-byte tag.  Returns ATLS_OK or ATLS_ERR_INPUT. */
int atls_aead_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *aad, size_t aadlen,
                      const uint8_t *pt, size_t ptlen,
                      uint8_t *ct, uint8_t tag[16]);

/* Verifies the tag with atls_ct_eq BEFORE releasing plaintext.  On
 * ATLS_ERR_AUTH the plaintext buffer is wiped.  `pt` may equal `ct`. */
int atls_aead_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *aad, size_t aadlen,
                      const uint8_t *ct, size_t ctlen,
                      const uint8_t tag[16], uint8_t *pt);

/* ---- X25519 (RFC 7748) ---- */

extern const uint8_t ATLS_X25519_BASEPOINT[32];   /* u = 9 */

/* Clamp a private scalar in place (RFC 7748 section 5).  atls_x25519()
 * clamps internally too; this is exposed for key-generation code. */
void atls_x25519_clamp(uint8_t scalar[32]);

/* out = X25519(scalar, point).  The high bit of point[31] is masked per
 * the RFC.  Returns ATLS_OK, ATLS_ERR_INPUT, or ATLS_ERR_LOW_ORDER when
 * the shared secret is all zeros (low-order input point, Wycheproof
 * ZeroSharedSecret).  On ATLS_ERR_LOW_ORDER, out is written with zeros. */
int atls_x25519(uint8_t out[32], const uint8_t scalar[32],
                const uint8_t point[32]);

/* ---- Ed25519 (RFC 8032) — verification only (D4) ---- */

/* Verify `sig` over `msg` under `pk`.  Returns ATLS_OK,
 * ATLS_ERR_BAD_ENCODING (R or the public key does not decode as a point),
 * or ATLS_ERR_BAD_SIGNATURE (everything decoded but the equation fails,
 * including the "0 <= S < L" range check on the integer half). */
int atls_ed25519_verify(const uint8_t sig[64], const uint8_t pk[32],
                        const void *msg, size_t msglen);

#ifdef __cplusplus
}
#endif

#endif /* LIBATLS_ATLS_H */

#ifndef LIBATLS_ATLS_CERTVAL_H
#define LIBATLS_ATLS_CERTVAL_H

/* atls/certval.h — Certificate validation (INTERNET_PLAN.md phase N5).
 *
 * Chain building, signature verification, validity dates, hostname
 * matching, basic constraints, key usage.  This is what makes the
 * padlock mean something (D5).
 *
 * Usage:
 *   atls_certval_ctx ctx;
 *   atls_certval_init(&ctx, trust_roots, num_roots);
 *   int rc = atls_certval_verify(&ctx, chain_certs, chain_len,
 *                                 hostname, now);
 */

#include <stdint.h>
#include <stddef.h>
#include "atls/atls.h"
#include "atls/x509.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum chain depth (leaf + intermediates). */
#define ATLS_CERTVAL_MAX_CHAIN 8

/* A pinned trust root (DER-encoded certificate). */
typedef struct {
    const uint8_t *der;
    size_t         der_len;
} atls_trust_root;

/* Current time for validity checking. */
typedef struct {
    int year, month, day, hour, minute, second;
} atls_time_now;

/* Validation context (holds the trust store). */
typedef struct {
    const atls_trust_root *roots;
    int                    num_roots;
} atls_certval_ctx;

/* Validation error codes. */
#define ATLS_CERTVAL_OK                0
#define ATLS_CERTVAL_ERR_CHAIN        -20  /* chain broken or unknown root */
#define ATLS_CERTVAL_ERR_EXPIRED      -21  /* certificate not yet valid or expired */
#define ATLS_CERTVAL_ERR_HOSTNAME     -22  /* hostname doesn't match SAN */
#define ATLS_CERTVAL_ERR_CA           -23  /* basicConstraints violation */
#define ATLS_CERTVAL_ERR_KEYUSAGE     -24  /* key usage violation */
#define ATLS_CERTVAL_ERR_SIGNATURE    -25  /* signature verification failed */
#define ATLS_CERTVAL_ERR_UNSUPPORTED  -26  /* unsupported signature algorithm */
#define ATLS_CERTVAL_ERR_UNKNOWN_ROOT -27  /* X8: top of chain's issuer is not a
                                            * shipped trust-store root (distinct
                                            * from a broken/garbled chain, so a
                                            * "root not in trust store" diagnosis
                                            * can be surfaced instead of a generic
                                            * handshake failure) */

/* Initialize validation context with a trust store. */
void atls_certval_init(atls_certval_ctx *ctx,
                       const atls_trust_root *roots, int num_roots);

/* Verify a certificate chain against the trust store.
 *
 * chain: array of DER certificates, leaf first (chain[0] = leaf).
 * chain_len: number of certificates in the chain.
 * hostname: the expected hostname (SNI); NULL to skip hostname check.
 * now: current time; NULL to skip date checks.
 *
 * Returns ATLS_CERTVAL_OK or a negative error code.
 *
 * Checks performed (each failure is a specific error code):
 *   - Chain links: issuer[i] == subject[i+1], signature[i] verified
 *     by [i+1]'s public key (Ed25519 or RSA-PKCS#1v1.5-SHA256).
 *   - Root: issuer of the last cert matches a pinned root, and that
 *     root's signature is verified.
 *   - Validity: each cert's not_before <= now <= not_after.
 *   - Hostname: leaf's SAN dNSName matches (wildcard: *.example.com
 *     matches foo.example.com but not example.com or a.b.example.com).
 *   - Basic constraints: leaf must NOT have cA=TRUE; intermediate CAs
 *     must have cA=TRUE.
 *   - Key usage: CA certs must have keyCertSign (bit 5); leaf must
 *     have digitalSignature (bit 0) or keyEncipherment (bit 2).
 */
int atls_certval_verify(atls_certval_ctx *ctx,
                        const uint8_t **chain, const size_t *chain_lens,
                        int chain_len,
                        const char *hostname,
                        const atls_time_now *now);

/* Match a hostname against a dNSName (supports single-label wildcard).
 * Returns 1 if matched, 0 if not. */
int atls_certval_hostname_match(const char *hostname,
                                const uint8_t *dns_name, size_t dns_len);

/* Check if a time is within validity period.
 * Returns 1 if valid, 0 if expired/not-yet-valid. */
int atls_certval_time_valid(const atls_x509_time *not_before,
                            const atls_x509_time *not_after,
                            const atls_time_now *now);

#ifdef __cplusplus
}
#endif

#endif /* LIBATLS_ATLS_CERTVAL_H */

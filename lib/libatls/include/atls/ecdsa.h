#ifndef LIBATLS_ATLS_ECDSA_H
#define LIBATLS_ATLS_ECDSA_H

/* atls/ecdsa.h — ECDSA P-256 (secp256r1) verification (REALINTERNET_PLAN.md
 * phase X1).
 *
 * Verification only — no signing, no key generation.  Certificate chains
 * (and TLS 1.3 CertificateVerify) need to verify the ECDSA signatures that
 * dominate the public web; they never need to produce one.
 *
 * The public key is the uncompressed EC point `04 || X || Y` (65 bytes), the
 * form that appears in an X.509 SubjectPublicKeyInfo and in a TLS 1.3
 * CertificateVerify over an ECDSA leaf.  The signature is the DER-encoded
 * ECDSA-Sig-Value `SEQUENCE { INTEGER r, INTEGER s }` — the form used both by
 * X.509 certificate signatures and by TLS 1.3.
 *
 * This is unaudited hobby cryptography (decision D7 and the N9 statement in
 * tls.md apply).  It runs on public data, so it is not constant-time.
 */

#include <stdint.h>
#include <stddef.h>
#include "atls/atls.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Verify an ECDSA-SHA256 signature over `msg` under the P-256 public key.
 *
 *   sig_der, sig_der_len : the DER ECDSA-Sig-Value (SEQUENCE { r, s }).
 *   pubkey               : 65-byte uncompressed point (0x04 || X || Y).
 *   msg, msg_len         : the signed message (TBS certificate DER, or the
 *                          TLS 1.3 CertificateVerify input).
 *
 * Returns:
 *   ATLS_OK                signature verified
 *   ATLS_ERR_BAD_ENCODING  signature DER or public key did not decode as a
 *                          valid P-256 point / r,s (wrong shape, off-curve)
 *   ATLS_ERR_BAD_SIGNATURE decoded fine but did not verify (including
 *                          r or s out of [1, n-1], or R = infinity)
 */
int atls_ecdsa_p256_verify(const uint8_t *sig_der, size_t sig_der_len,
                           const uint8_t pubkey[65],
                           const uint8_t *msg, size_t msg_len);

#ifdef __cplusplus
}
#endif

#endif /* LIBATLS_ATLS_ECDSA_H */

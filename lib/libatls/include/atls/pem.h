#ifndef LIBATLS_ATLS_PEM_H
#define LIBATLS_ATLS_PEM_H

/* atls/pem.h — PEM decoding for trust-store roots (REALINTERNET_PLAN X2).
 *
 * X.509 certificates are shipped as base64 "-----BEGIN CERTIFICATE-----"
 * PEM blocks (etc/ssl/roots.pem).  This parses one PEM block into a DER
 * buffer so atls_certval can validate against it.  Verification only: we
 * decode certificates we already trust, never attacker input, so this is
 * not a security boundary — but it still bounds its work (no allocation,
 * caller-supplied output buffer, length checked).
 */

#include <stdint.h>
#include <stddef.h>
#include "atls/atls.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decode a single "BEGIN CERTIFICATE" ... "END CERTIFICATE" PEM block.
 *
 *   pem, pem_len : the whole PEM text (may contain surrounding data and
 *                  other blocks; the first CERTIFICATE block is used).
 *   der, der_cap : caller-owned output buffer for the DER bytes.
 *   der_len      : on success, receives the number of bytes written.
 *
 * Returns ATLS_OK, or ATLS_ERR_BAD_ENCODING (no/broken block, bad base64,
 * output too small), ATLS_ERR_INPUT (NULL args).
 */
int atls_pem_cert_to_der(const char *pem, size_t pem_len,
                         uint8_t *der, size_t der_cap, size_t *der_len);

#ifdef __cplusplus
}
#endif

#endif /* LIBATLS_ATLS_PEM_H */

/* atls_hkdf.c — HKDF-SHA256 (RFC 5869).
 *
 * Extract-then-Expand.  TLS 1.3 (RFC 8446) builds its entire key schedule
 * on HKDF-Expand with SHA-256, which is why this is here in N1 rather
 * than waiting for N3.
 */

#include "atls/atls.h"

int atls_hkdf_extract(const uint8_t *salt, size_t saltlen,
                      const uint8_t *ikm, size_t ikmlen,
                      uint8_t prk[32]) {
    if (!prk || (!ikm && ikmlen)) return ATLS_ERR_INPUT;
    if (!salt || saltlen == 0) {
        static const uint8_t zeros[32] = { 0 };
        atls_hmac_sha256(zeros, sizeof(zeros), ikm, ikmlen, prk);
    } else {
        atls_hmac_sha256(salt, saltlen, ikm, ikmlen, prk);
    }
    return ATLS_OK;
}

int atls_hkdf_expand(const uint8_t prk[32],
                     const uint8_t *info, size_t infolen,
                     uint8_t *okm, size_t okmlen) {
    if (!prk || (!okm && okmlen) || (!info && infolen)) return ATLS_ERR_INPUT;
    if (okmlen > 255u * 32u) return ATLS_ERR_INPUT;   /* RFC 5869 §2.3 */

    uint8_t t[32];
    size_t tlen = 0;
    size_t done = 0;
    uint8_t counter = 1;

    while (done < okmlen) {
        atls_hmac_sha256_ctx c;
        atls_hmac_sha256_init(&c, prk, 32);
        if (tlen) atls_hmac_sha256_update(&c, t, tlen);
        if (infolen) atls_hmac_sha256_update(&c, info, infolen);
        atls_hmac_sha256_update(&c, &counter, 1);
        atls_hmac_sha256_final(&c, t);
        tlen = 32;

        size_t take = okmlen - done;
        if (take > 32) take = 32;
        for (size_t i = 0; i < take; i++) okm[done + i] = t[i];
        done += take;
        counter++;
    }

    atls_wipe(t, sizeof(t));
    return ATLS_OK;
}

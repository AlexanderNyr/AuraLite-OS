#ifndef LIBATLS_ATLS_X509_H
#define LIBATLS_ATLS_X509_H

/* atls/x509.h — X.509 certificate parsing (INTERNET_PLAN.md phase N2).
 *
 * "Read a certificate without being read by it."
 *
 * The parser is ZERO-COPY and ZERO-ALLOCATION: every output field is a
 * span into the caller's DER buffer, so a hostile length field cannot
 * buy memory.  Nesting is bounded by ATLS_DER_MAX_DEPTH (32); a
 * 10 000-deep construction is refused with ATLS_ERR_DEPTH, never
 * followed.  Unknown CRITICAL extensions are fatal (D5: reject rather
 * than interpret); unknown non-critical ones are skipped with the
 * depth-bounded iterative walker.
 *
 * What this phase gives N5 (validation):
 *   - tbs + signature + both algorithm OIDs  -> signature verification
 *   - issuer / subject raw DER spans         -> chain building by byte
 *                                               comparison
 *   - not_before / not_after                 -> validity dates (N5 checks
 *                                               the RTC is sane first)
 *   - SAN dNSName list                       -> hostname matching
 *   - basic constraints / key usage          -> CA policy checks
 *   - SPKI span + algorithm OID              -> key decoding (RSA parsing
 *                                               arrives with N5)
 */

#include <stdint.h>
#include <stddef.h>
#include "atls/atls.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATLS_X509_MAX_DNS_NAMES 16

/* A raw slice of the input DER.  Valid for as long as the input buffer
 * the certificate was parsed from is valid. */
typedef struct {
    const uint8_t *data;
    size_t len;
} atls_span;

typedef struct {
    int year, month, day, hour, minute, second;
} atls_x509_time;

typedef struct {
    /* structure */
    int version;                    /* 3 for an X.509 v3 certificate */
    atls_span serial;               /* INTEGER contents (may lead with 0x00) */
    atls_span tbs;                  /* full TBSCertificate DER (sign this) */

    /* signatures */
    atls_span outer_sig_alg;        /* outer AlgorithmIdentifier DER */
    atls_span outer_sig_oid;        /* its OID contents */
    atls_span inner_sig_oid;        /* TBS signature OID (must equal outer) */
    atls_span signature;            /* signature BIT STRING contents */
    int signature_unused_bits;      /* must be 0 for a valid certificate */

    /* identity */
    atls_span issuer;               /* full Name DER */
    atls_span subject;              /* full Name DER */
    atls_x509_time not_before;
    atls_x509_time not_after;

    /* SubjectPublicKeyInfo */
    atls_span spki;                 /* full SPKI DER */
    atls_span spki_alg_oid;         /* algorithm OID inside SPKI */
    atls_span spki_key;             /* subjectPublicKey BIT STRING contents */
    int spki_key_unused_bits;

    /* extensions the stack understands */
    int is_ca;                      /* basicConstraints.cA (default FALSE) */
    int path_len_present;
    int path_len;                   /* meaningful when path_len_present */
    int bc_critical;
    int key_usage_present;
    int ku_critical;
    uint16_t key_usage;             /* bit i => 1<<i; bit 0 = digitalSignature,
                                       bit 5 = keyCertSign, bit 6 = cRLSign */
    int san_dns_count;              /* entries stored in san_dns */
    int san_dns_truncated;          /* more dNSNames than MAX: extras ignored */
    atls_span san_dns[ATLS_X509_MAX_DNS_NAMES];
} atls_x509_cert;

/* Parse one DER certificate.  Returns ATLS_OK or one of:
 *   ATLS_ERR_INPUT         NULL argument
 *   ATLS_ERR_TRUNCATED     input ends mid-structure
 *   ATLS_ERR_BAD_LENGTH    non-DER length, or length beyond the buffer
 *   ATLS_ERR_DEPTH         nesting deeper than ATLS_DER_MAX_DEPTH
 *   ATLS_ERR_BAD_ENCODING  wrong tags/shape for a certificate
 *   ATLS_ERR_UNSUPPORTED   v1/v2 cert, unknown critical extension,
 *                          high-tag-number forms, time shapes we refuse
 * On any error, *out is left wiped (all zeros). */
int atls_x509_parse(const uint8_t *der, size_t len, atls_x509_cert *out);

/* Compare an OID span against raw OID content octets. */
int atls_oid_eq(const atls_span *oid, const uint8_t *bytes, size_t len);

/* Well-known algorithm OIDs (content octets), for N5 and for tests. */
extern const uint8_t ATLS_OID_RSA_ENCRYPTION[9];        /* 1.2.840.113549.1.1.1 */
extern const uint8_t ATLS_OID_SHA256_RSA[9];            /* 1.2.840.113549.1.1.11 */
extern const uint8_t ATLS_OID_SHA384_RSA[9];            /* 1.2.840.113549.1.1.12 */
extern const uint8_t ATLS_OID_EC_PUBLIC_KEY[7];         /* 1.2.840.10045.2.1 */
extern const uint8_t ATLS_OID_ECDSA_SHA256[8];          /* 1.2.840.10045.4.3.2 */
extern const uint8_t ATLS_OID_ECDSA_SHA384[8];          /* 1.2.840.10045.4.3.3 */
extern const uint8_t ATLS_OID_ED25519[3];               /* 1.3.101.112 */

/* Extension OIDs (content octets). */
extern const uint8_t ATLS_OID_EXT_KEY_USAGE[3];         /* 2.5.29.15 */
extern const uint8_t ATLS_OID_EXT_BASIC_CONSTRAINTS[3]; /* 2.5.29.19 */
extern const uint8_t ATLS_OID_EXT_SAN[3];               /* 2.5.29.17 */

#ifdef __cplusplus
}
#endif

#endif /* LIBATLS_ATLS_X509_H */

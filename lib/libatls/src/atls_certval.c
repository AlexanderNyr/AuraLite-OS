/* atls_certval.c — Certificate validation (INTERNET_PLAN.md N5).
 *
 * Chain building, signature verification (Ed25519 + RSA-PKCS#1v1.5-SHA256),
 * validity dates, hostname matching, basic constraints, key usage.
 *
 * Every failure is a specific error code — the test gate requires each
 * refusal to be individually asserted for the right reason.
 */

#include "atls/certval.h"
#include "atls_rsa.h"
#include "atls/ecdsa.h"
#include <string.h>
#include <stdio.h>

void atls_certval_init(atls_certval_ctx *ctx,
                       const atls_trust_root *roots, int num_roots) {
    ctx->roots = roots;
    ctx->num_roots = num_roots;
}

/* ---- Hostname matching (RFC 6125 §6.4.3) ---- */

int atls_certval_hostname_match(const char *hostname,
                                const uint8_t *dns_name, size_t dns_len) {
    if (!hostname || !dns_name || dns_len == 0) return 0;

    size_t hn_len = strlen(hostname);
    if (hn_len == 0) return 0;

    /* Compare as strings. */
    if (dns_len == hn_len) {
        int match = 1;
        for (size_t i = 0; i < dns_len; i++) {
            char a = (char)dns_name[i], b = hostname[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = 0; break; }
        }
        if (match) return 1;
    }

    /* Wildcard: *.example.com matches foo.example.com but NOT
     * example.com or a.b.example.com (RFC 6125: only a single
     * leftmost label). */
    if (dns_len >= 2 && dns_name[0] == '*' && dns_name[1] == '.') {
        /* The wildcard must be the entire leftmost label. */
        const char *wild_suffix = (const char *)dns_name + 1; /* ".example.com" */
        size_t wild_suffix_len = dns_len - 1;

        /* hostname must have a leftmost label (no dots in the part
         * matching the wildcard). */
        const char *hn_suffix = hostname;
        /* Find the first dot in hostname — that's where the suffix starts. */
        for (size_t i = 0; i < hn_len; i++) {
            if (hostname[i] == '.') {
                hn_suffix = hostname + i;
                break;
            }
        }
        size_t hn_suffix_len = hn_len - (size_t)(hn_suffix - hostname);

        /* The suffix must match. */
        if (hn_suffix_len != wild_suffix_len) return 0;
        for (size_t i = 0; i < wild_suffix_len; i++) {
            char a = wild_suffix[i], b = hn_suffix[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) return 0;
        }
        return 1;
    }

    return 0;
}

/* ---- Time comparison ---- */

static int time_cmp(const atls_x509_time *a, const atls_x509_time *b) {
    if (a->year != b->year) return a->year < b->year ? -1 : 1;
    if (a->month != b->month) return a->month < b->month ? -1 : 1;
    if (a->day != b->day) return a->day < b->day ? -1 : 1;
    if (a->hour != b->hour) return a->hour < b->hour ? -1 : 1;
    if (a->minute != b->minute) return a->minute < b->minute ? -1 : 1;
    if (a->second != b->second) return a->second < b->second ? -1 : 1;
    return 0;
}

int atls_certval_time_valid(const atls_x509_time *not_before,
                            const atls_x509_time *not_after,
                            const atls_time_now *now) {
    if (!not_before || !not_after || !now) return 0;
    atls_x509_time n;
    n.year = now->year; n.month = now->month; n.day = now->day;
    n.hour = now->hour; n.minute = now->minute; n.second = now->second;
    return time_cmp(&n, not_before) >= 0 && time_cmp(&n, not_after) <= 0;
}

/* ---- Signature verification (dispatch) ---- */

static int verify_cert_signature(const atls_x509_cert *child,
                                 const atls_x509_cert *issuer) {
    /* The child's signature algorithm must match the issuer's SPKI
     * algorithm. */
    const uint8_t *sig = child->signature.data;
    size_t sig_len = child->signature.len;

    /* Ed25519: OID 1.3.101.112 (2b 65 70) */
    if (atls_oid_eq(&child->outer_sig_oid, ATLS_OID_ED25519, 3)) {
        if (!atls_oid_eq(&issuer->spki_alg_oid, ATLS_OID_ED25519, 3))
            return ATLS_CERTVAL_ERR_UNSUPPORTED;
        if (issuer->spki_key.len != 32 || issuer->spki_key_unused_bits)
            return ATLS_CERTVAL_ERR_SIGNATURE;
        int edrc = atls_ed25519_verify(sig, issuer->spki_key.data,
                                       child->tbs.data, child->tbs.len);
        return edrc == ATLS_OK
               ? ATLS_CERTVAL_OK : ATLS_CERTVAL_ERR_SIGNATURE;
    }

    /* RSA PKCS#1v1.5 SHA-256: OID 1.2.840.113549.1.1.11 */
    if (atls_oid_eq(&child->outer_sig_oid, ATLS_OID_SHA256_RSA, 9)) {
        if (!atls_oid_eq(&issuer->spki_alg_oid, ATLS_OID_RSA_ENCRYPTION, 9)) {
            return ATLS_CERTVAL_ERR_UNSUPPORTED;
        }
        /* Parse SPKI to extract modulus and exponent.
         * SPKI is SEQUENCE { AlgorithmIdentifier, BIT STRING }
         * The BIT STRING contents (after unused-bits byte) is
         * RSAPublicKey = SEQUENCE { modulus INTEGER, exponent INTEGER }. */
        const uint8_t *key = issuer->spki_key.data;
        size_t key_len = issuer->spki_key.len;
        if (key_len < 4 || key[0] != 0x30) {
            return ATLS_CERTVAL_ERR_SIGNATURE;
        }
        /* Skip outer SEQUENCE. */
        size_t pos = 1;
        /* Parse length. */
        if (key[pos] < 0x80) { pos += 1 + key[pos]; }
        else { int n = key[pos] & 0x7f; pos += 1 + n; }
        /* Find modulus INTEGER. */
        if (pos >= key_len || key[pos] != 0x02) {
            return ATLS_CERTVAL_ERR_SIGNATURE;
        }
        pos++;
        size_t n_len = key[pos]; pos++;
        if (n_len >= 0x80) { int nb = n_len & 0x7f; n_len = 0; for (int i = 0; i < nb; i++) n_len = (n_len << 8) | key[pos++]; }
        const uint8_t *n_bytes = key + pos;
        size_t n_content_len = n_len; /* save original DER content length */
        /* Skip leading zero if present (DER integer sign byte). */
        if (n_len > 0 && n_bytes[0] == 0x00) { n_bytes++; n_len--; }
        pos += n_content_len; /* advance past ALL DER content bytes */
        /* Find exponent INTEGER. */
        if (pos >= key_len || key[pos] != 0x02) {
            return ATLS_CERTVAL_ERR_SIGNATURE;
        }
        pos++;
        size_t e_len = key[pos]; pos++;
        if (e_len >= 0x80) { int nb = e_len & 0x7f; e_len = 0; for (int i = 0; i < nb; i++) e_len = (e_len << 8) | key[pos++]; }
        const uint8_t *e_bytes = key + pos;
        /* Skip leading zero for exponent too (unlikely but consistent). */
        if (e_len > 0 && e_bytes[0] == 0x00) { e_bytes++; e_len--; }

        int rsarc = atls_rsa_verify_pkcs1v15(sig, sig_len,
                                             child->tbs.data, child->tbs.len,
                                             n_bytes, n_len, e_bytes, e_len);
        return rsarc == ATLS_OK
               ? ATLS_CERTVAL_OK : ATLS_CERTVAL_ERR_SIGNATURE;
    }

    /* ECDSA P-256 SHA-256: OID 1.2.840.10045.4.3.2 (REALINTERNET_PLAN X1).
     * The issuer's public key must be an uncompressed P-256 point
     * (0x04 || X || Y), and the certificate signature is a DER
     * ECDSA-Sig-Value over the TBS bytes. */
    if (atls_oid_eq(&child->outer_sig_oid, ATLS_OID_ECDSA_SHA256, 8)) {
        if (!atls_oid_eq(&issuer->spki_alg_oid, ATLS_OID_EC_PUBLIC_KEY, 7))
            return ATLS_CERTVAL_ERR_UNSUPPORTED;
        if (issuer->spki_key.len != 65 || issuer->spki_key_unused_bits ||
            issuer->spki_key.data[0] != 0x04)
            return ATLS_CERTVAL_ERR_SIGNATURE;
        int rc = atls_ecdsa_p256_verify(sig, sig_len,
                                        issuer->spki_key.data,
                                        child->tbs.data, child->tbs.len);
        return rc == ATLS_OK ? ATLS_CERTVAL_OK : ATLS_CERTVAL_ERR_SIGNATURE;
    }

    /* Any other signature algorithm is unsupported. */
    return ATLS_CERTVAL_ERR_UNSUPPORTED;
}

/* ---- Chain validation ---- */

int atls_certval_verify(atls_certval_ctx *ctx,
                        const uint8_t **chain, const size_t *chain_lens,
                        int chain_len,
                        const char *hostname,
                        const atls_time_now *now) {
    if (!ctx || !chain || !chain_lens || chain_len < 1)
        return ATLS_CERTVAL_ERR_CHAIN;
    if (chain_len > ATLS_CERTVAL_MAX_CHAIN)
        return ATLS_CERTVAL_ERR_CHAIN;

    /* Parse all certificates. */
    atls_x509_cert certs[ATLS_CERTVAL_MAX_CHAIN];
    for (int i = 0; i < chain_len; i++) {
        if (atls_x509_parse(chain[i], chain_lens[i], &certs[i]) != ATLS_OK)
            return ATLS_CERTVAL_ERR_CHAIN;
    }

    /* 1. Verify chain links. */
    for (int i = 0; i < chain_len - 1; i++) {
        /* issuer[i] must equal subject[i+1]. */
        if (certs[i].issuer.len != certs[i + 1].subject.len) {
            return ATLS_CERTVAL_ERR_CHAIN;
        }
        for (size_t j = 0; j < certs[i].issuer.len; j++) {
            if (certs[i].issuer.data[j] != certs[i + 1].subject.data[j]) {
                return ATLS_CERTVAL_ERR_CHAIN;
            }
        }
        /* Signature of cert[i] verified by cert[i+1]'s public key. */
        int src = verify_cert_signature(&certs[i], &certs[i + 1]);
        if (src != ATLS_CERTVAL_OK) return src;
    }

    /* 2. Verify last cert against trust store. */
    const atls_x509_cert *last = &certs[chain_len - 1];
    int found_root = 0;
    for (int r = 0; r < ctx->num_roots; r++) {
        atls_x509_cert root;
        if (atls_x509_parse(ctx->roots[r].der, ctx->roots[r].der_len,
                            &root) != ATLS_OK)
            continue;
        /* issuer[last] == subject[root]? */
        if (last->issuer.len != root.subject.len) continue;
        int match = 1;
        for (size_t j = 0; j < last->issuer.len; j++) {
            if (last->issuer.data[j] != root.subject.data[j]) {
                match = 0; break;
            }
        }
        if (!match) continue;
        /* Verify last cert's signature with root's public key. */
        int ssrc = verify_cert_signature(last, &root);
        if (ssrc != ATLS_CERTVAL_OK) return ssrc;
        found_root = 1;
        break;
    }
    if (!found_root) return ATLS_CERTVAL_ERR_UNKNOWN_ROOT;   /* X8 diagnosis */

    /* 3. Validity dates. */
    if (now) {
        for (int i = 0; i < chain_len; i++) {
            if (!atls_certval_time_valid(&certs[i].not_before,
                                         &certs[i].not_after, now)) {
                return ATLS_CERTVAL_ERR_EXPIRED;
            }
        }
    }

    /* 4. Hostname matching (leaf only). */
    if (hostname) {
        int matched = 0;
        for (int i = 0; i < certs[0].san_dns_count; i++) {
            if (atls_certval_hostname_match(hostname,
                                            certs[0].san_dns[i].data,
                                            certs[0].san_dns[i].len)) {
                matched = 1;
                break;
            }
        }
        if (!matched) {
            return ATLS_CERTVAL_ERR_HOSTNAME;
        }
    }

    /* 5. Basic constraints.
     * - Leaf: cA must NOT be TRUE.
     * - Intermediates: cA must be TRUE. */
    for (int i = 1; i < chain_len; i++) printf(" cert[%d].is_ca=%d", i, certs[i].is_ca);
    printf("\n");
    if (certs[0].is_ca) {
        return ATLS_CERTVAL_ERR_CA;
    }
    for (int i = 1; i < chain_len; i++) {
        if (!certs[i].is_ca) {
            return ATLS_CERTVAL_ERR_CA;
        }
    }

    /* 6. Key usage.
     * - CA certs: keyCertSign (bit 5) must be set.
     * - Leaf: digitalSignature (bit 0) or keyEncipherment (bit 2). */
    for (int i = 1; i < chain_len; i++) {
        if (certs[i].key_usage_present && !(certs[i].key_usage & (1 << 5)))
            return ATLS_CERTVAL_ERR_KEYUSAGE;
    }
    if (certs[0].key_usage_present) {
        if (!(certs[0].key_usage & ((1 << 0) | (1 << 2))))
            return ATLS_CERTVAL_ERR_KEYUSAGE;
    }

    return ATLS_CERTVAL_OK;
}

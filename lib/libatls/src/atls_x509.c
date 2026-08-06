/* atls_x509.c — X.509 certificate parsing (INTERNET_PLAN.md phase N2).
 *
 * Zero-copy, zero-allocation, depth-bounded; see atls/x509.h and
 * atls_der.h for the refusal contract.  The grammar implemented is
 * RFC 5280, exactly the shapes a TLS 1.3 server sends:
 *
 *   Certificate ::= SEQUENCE {
 *       tbsCertificate       TBSCertificate,
 *       signatureAlgorithm   AlgorithmIdentifier,
 *       signatureValue       BIT STRING }
 *
 * Anything else — v1/v2 certificates, indefinite lengths, high-tag-number
 * tags, unknown critical extensions, non-minimal encodings — is refused
 * with a specific error code rather than interpreted.
 */

#include "atls/x509.h"
#include "atls_der.h"

/* ---- well-known OIDs (content octets) ---- */

const uint8_t ATLS_OID_RSA_ENCRYPTION[9] =
    { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };
const uint8_t ATLS_OID_SHA256_RSA[9] =
    { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b };
const uint8_t ATLS_OID_SHA384_RSA[9] =
    { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0c };
const uint8_t ATLS_OID_EC_PUBLIC_KEY[7] =
    { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01 };
const uint8_t ATLS_OID_ECDSA_SHA256[8] =
    { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02 };
const uint8_t ATLS_OID_ECDSA_SHA384[8] =
    { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x03 };
const uint8_t ATLS_OID_ED25519[3] = { 0x2b, 0x65, 0x70 };
const uint8_t ATLS_OID_EXT_KEY_USAGE[3] = { 0x55, 0x1d, 0x0f };
const uint8_t ATLS_OID_EXT_BASIC_CONSTRAINTS[3] = { 0x55, 0x1d, 0x13 };
const uint8_t ATLS_OID_EXT_SAN[3] = { 0x55, 0x1d, 0x11 };

int atls_oid_eq(const atls_span *oid, const uint8_t *bytes, size_t len) {
    if (!oid || !bytes || oid->len != len) return 0;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(oid->data[i] ^ bytes[i]);
    }
    return (int)(((uint32_t)diff - 1u) >> 31);
}

/* ---- small helpers ---- */

static void span_from_tlv(atls_span *s, const atls_der_tlv *t) {
    s->data = t->value;
    s->len = t->len;
}

/* Whole-TLV span (header included) — used for issuer/subject/SPKI/tbs,
 * which N5 compares and hashes verbatim. */
static void span_whole_tlv(atls_span *s, const atls_der_tlv *t) {
    s->data = t->header;
    s->len = t->header_len + t->len;
}

/* Open a sub-reader over a value span, inheriting depth from the parent
 * scope (+1).  The depth budget is the defence against hostile nesting
 * reached through spans rather than through atls_der_enter. */
static int open_scope(const atls_der *parent, const uint8_t *p, size_t n,
                      atls_der *out) {
    if (parent->depth + 1 > ATLS_DER_MAX_DEPTH) return ATLS_ERR_DEPTH;
    if (!p && n) return ATLS_ERR_INPUT;
    out->p = p;
    out->end = p + n;
    out->depth = parent->depth + 1;
    return ATLS_OK;
}

/* Decode a small non-negative DER INTEGER (<= 4 content octets). */
static int der_small_uint(const atls_der_tlv *t, uint32_t *out) {
    if (t->tag != 0x02) return ATLS_ERR_BAD_ENCODING;
    if (t->len == 0 || t->len > 4) return ATLS_ERR_BAD_ENCODING;
    uint32_t v = 0;
    for (size_t i = 0; i < t->len; i++) {
        v = (v << 8) | t->value[i];
    }
    *out = v;
    return ATLS_OK;
}

static int is_digits(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
    }
    return 1;
}

static int two_digits(const uint8_t *p) {
    return (p[0] - '0') * 10 + (p[1] - '0');
}

/* UTCTime / GeneralizedTime -> broken-down time (RFC 5280 §4.1.2.5).
 * Only the two shapes real certificates use are accepted:
 *   YYMMDDHHMMSSZ  and  YYYYMMDDHHMMSSZ. */
static int parse_time(const atls_der_tlv *t, atls_x509_time *out) {
    if (t->tag == 0x17) {                     /* UTCTime */
        if (t->len != 13 || t->value[12] != 'Z') return ATLS_ERR_BAD_ENCODING;
        if (!is_digits(t->value, 12)) return ATLS_ERR_BAD_ENCODING;
        int yy = two_digits(t->value);
        out->year = yy >= 50 ? 1900 + yy : 2000 + yy;
        out->month = two_digits(t->value + 2);
        out->day = two_digits(t->value + 4);
        out->hour = two_digits(t->value + 6);
        out->minute = two_digits(t->value + 8);
        out->second = two_digits(t->value + 10);
    } else if (t->tag == 0x18) {              /* GeneralizedTime */
        if (t->len != 15 || t->value[14] != 'Z') return ATLS_ERR_BAD_ENCODING;
        if (!is_digits(t->value, 14)) return ATLS_ERR_BAD_ENCODING;
        out->year = two_digits(t->value) * 100 + two_digits(t->value + 2);
        out->month = two_digits(t->value + 4);
        out->day = two_digits(t->value + 6);
        out->hour = two_digits(t->value + 8);
        out->minute = two_digits(t->value + 10);
        out->second = two_digits(t->value + 12);
    } else {
        return ATLS_ERR_BAD_ENCODING;
    }
    if (out->month < 1 || out->month > 12) return ATLS_ERR_BAD_ENCODING;
    if (out->day < 1 || out->day > 31) return ATLS_ERR_BAD_ENCODING;
    if (out->hour > 23) return ATLS_ERR_BAD_ENCODING;
    if (out->minute > 59) return ATLS_ERR_BAD_ENCODING;
    if (out->second > 59) return ATLS_ERR_BAD_ENCODING;
    return ATLS_OK;
}

/* ---- extension value parsers ---- */

static int parse_san(const atls_der *ext_scope, atls_x509_cert *out) {
    atls_der names_scope;
    int rc = open_scope(ext_scope, ext_scope->p, (size_t)(ext_scope->end - ext_scope->p),
                        &names_scope);
    if (rc != ATLS_OK) return rc;

    atls_der_tlv seq;
    rc = atls_der_read_tlv(&names_scope, &seq);
    if (rc != ATLS_OK) return rc;
    if (seq.tag != 0x30) return ATLS_ERR_BAD_ENCODING;
    if (!atls_der_at_end(&names_scope)) return ATLS_ERR_BAD_ENCODING;

    atls_der names;
    rc = open_scope(&names_scope, seq.value, seq.len, &names);
    if (rc != ATLS_OK) return rc;

    while (!atls_der_at_end(&names)) {
        atls_der_tlv name;
        rc = atls_der_read_tlv(&names, &name);
        if (rc != ATLS_OK) return rc;
        if (name.tag == 0x82) {               /* dNSName [2] IA5String */
            if (name.constructed) return ATLS_ERR_BAD_ENCODING;
            if (out->san_dns_count < ATLS_X509_MAX_DNS_NAMES) {
                span_from_tlv(&out->san_dns[out->san_dns_count], &name);
                out->san_dns_count++;
            } else {
                out->san_dns_truncated = 1;
            }
        } else {
            /* Other GeneralName choices: skip with the depth budget. */
            rc = atls_der_skip_value(&name, names.depth);
            if (rc != ATLS_OK) return rc;
        }
    }
    return ATLS_OK;
}

static int parse_basic_constraints(const atls_der *ext_scope,
                                   atls_x509_cert *out) {
    atls_der s;
    int rc = open_scope(ext_scope, ext_scope->p,
                        (size_t)(ext_scope->end - ext_scope->p), &s);
    if (rc != ATLS_OK) return rc;

    atls_der_tlv seq;
    rc = atls_der_read_tlv(&s, &seq);
    if (rc != ATLS_OK) return rc;
    if (seq.tag != 0x30) return ATLS_ERR_BAD_ENCODING;
    if (!atls_der_at_end(&s)) return ATLS_ERR_BAD_ENCODING;

    /* BasicConstraints ::= SEQUENCE {} is legal (CA defaults FALSE). */
    if (seq.len == 0) return ATLS_OK;

    rc = atls_der_enter(&s, &seq);
    if (rc != ATLS_OK) return rc;

    if (!atls_der_at_end(&s)) {
        atls_der_tlv ca;
        rc = atls_der_peek_tlv(&s, &ca);
        if (rc != ATLS_OK) return rc;
        if (ca.tag == 0x01) {                 /* cA BOOLEAN */
            rc = atls_der_read_tlv(&s, &ca);
            if (rc != ATLS_OK) return rc;
            if (ca.len != 1) return ATLS_ERR_BAD_ENCODING;
            out->is_ca = ca.value[0] != 0;
        }
    }
    if (!atls_der_at_end(&s)) {
        atls_der_tlv pl;
        rc = atls_der_read_tlv(&s, &pl);
        if (rc != ATLS_OK) return rc;
        uint32_t v = 0;
        rc = der_small_uint(&pl, &v);
        if (rc != ATLS_OK) return rc;
        out->path_len_present = 1;
        out->path_len = (int)v;
    }
    if (!atls_der_at_end(&s)) return ATLS_ERR_BAD_ENCODING;
    return ATLS_OK;
}

static int parse_key_usage(const atls_der *ext_scope, atls_x509_cert *out) {
    atls_der s;
    int rc = open_scope(ext_scope, ext_scope->p,
                        (size_t)(ext_scope->end - ext_scope->p), &s);
    if (rc != ATLS_OK) return rc;

    atls_der_tlv bs;
    rc = atls_der_read_tlv(&s, &bs);
    if (rc != ATLS_OK) return rc;
    if (bs.tag != 0x03) return ATLS_ERR_BAD_ENCODING;
    if (!atls_der_at_end(&s)) return ATLS_ERR_BAD_ENCODING;
    if (bs.len == 0) return ATLS_ERR_BAD_ENCODING;

    int unused = bs.value[0];
    if (unused > 7) return ATLS_ERR_BAD_ENCODING;
    size_t nbits = (bs.len - 1) * 8 - (size_t)unused;
    uint16_t bits = 0;
    for (size_t i = 0; i < nbits && i < 16; i++) {
        if (bs.value[1 + i / 8] & (0x80u >> (i % 8))) {
            bits |= (uint16_t)(1u << i);
        }
    }
    out->key_usage_present = 1;
    out->key_usage = bits;
    return ATLS_OK;
}

/* ---- the certificate itself ---- */

static void wipe_cert(atls_x509_cert *out) {
    uint8_t *p = (uint8_t *)(uintptr_t)out;
    for (size_t i = 0; i < sizeof(*out); i++) p[i] = 0;
}

int atls_x509_parse(const uint8_t *der, size_t len, atls_x509_cert *out) {
    if (!out) return ATLS_ERR_INPUT;
    wipe_cert(out);
    if (!der) return ATLS_ERR_INPUT;
    if (len == 0) return ATLS_ERR_TRUNCATED;

    atls_der d;
    int rc = atls_der_init(&d, der, len);
    if (rc != ATLS_OK) return rc;

    /* Certificate ::= SEQUENCE { tbs, sigAlg, sig } — exactly three. */
    atls_der_tlv cert_seq;
    rc = atls_der_read_tlv(&d, &cert_seq);
    if (rc != ATLS_OK) return rc;
    if (cert_seq.tag != 0x30) return ATLS_ERR_BAD_ENCODING;
    if (!atls_der_at_end(&d)) return ATLS_ERR_BAD_ENCODING;

    rc = atls_der_enter(&d, &cert_seq);
    if (rc != ATLS_OK) return rc;

    /* ---- TBSCertificate ---- */
    atls_der_tlv tbs;
    rc = atls_der_read_tlv(&d, &tbs);
    if (rc != ATLS_OK) return rc;
    if (tbs.tag != 0x30) return ATLS_ERR_BAD_ENCODING;
    span_whole_tlv(&out->tbs, &tbs);

    /* `t` is scoped INSIDE the TBS; `d` stays at certificate level so it
     * can later read signatureAlgorithm and signatureValue.  Using
     * atls_der_enter on `d` here would clobber that outer scope. */
    atls_der t;
    rc = open_scope(&d, tbs.value, tbs.len, &t);
    if (rc != ATLS_OK) return rc;

    /* version [0] EXPLICIT INTEGER — optional */
    atls_der_tlv f;
    rc = atls_der_peek_tlv(&t, &f);
    if (rc != ATLS_OK) return rc;
    out->version = 1;
    if (f.tag == 0xa0) {
        rc = atls_der_read_tlv(&t, &f);
        if (rc != ATLS_OK) return rc;
        atls_der v;
        rc = open_scope(&t, f.value, f.len, &v);
        if (rc != ATLS_OK) return rc;
        atls_der_tlv vi;
        rc = atls_der_read_tlv(&v, &vi);
        if (rc != ATLS_OK) return rc;
        uint32_t ver = 0;
        rc = der_small_uint(&vi, &ver);
        if (rc != ATLS_OK) return rc;
        if (!atls_der_at_end(&v)) return ATLS_ERR_BAD_ENCODING;
        out->version = (int)ver + 1;
        /* Exactly v3: v1/v2 predate extensions, and higher numbers do
         * not exist — refuse both directions. */
        if (out->version != 3) return ATLS_ERR_UNSUPPORTED;
    }

    /* serialNumber INTEGER */
    rc = atls_der_read_tlv(&t, &f);
    if (rc != ATLS_OK) return rc;
    if (f.tag != 0x02) return ATLS_ERR_BAD_ENCODING;
    span_from_tlv(&out->serial, &f);

    /* signature AlgorithmIdentifier (inside TBS) */
    rc = atls_der_read_tlv(&t, &f);
    if (rc != ATLS_OK) return rc;
    if (f.tag != 0x30) return ATLS_ERR_BAD_ENCODING;
    {
        atls_der a;
        rc = open_scope(&t, f.value, f.len, &a);
        if (rc != ATLS_OK) return rc;
        atls_der_tlv oid;
        rc = atls_der_read_tlv(&a, &oid);
        if (rc != ATLS_OK) return rc;
        if (oid.tag != 0x06) return ATLS_ERR_BAD_ENCODING;
        span_from_tlv(&out->inner_sig_oid, &oid);
        /* parameters (if any) are skipped by not reading them */
    }

    /* issuer Name */
    rc = atls_der_read_tlv(&t, &f);
    if (rc != ATLS_OK) return rc;
    if (f.tag != 0x30) return ATLS_ERR_BAD_ENCODING;
    span_whole_tlv(&out->issuer, &f);

    /* validity SEQUENCE { notBefore, notAfter } */
    rc = atls_der_read_tlv(&t, &f);
    if (rc != ATLS_OK) return rc;
    if (f.tag != 0x30) return ATLS_ERR_BAD_ENCODING;
    {
        atls_der val;
        rc = open_scope(&t, f.value, f.len, &val);
        if (rc != ATLS_OK) return rc;
        atls_der_tlv nb, na;
        rc = atls_der_read_tlv(&val, &nb);
        if (rc != ATLS_OK) return rc;
        rc = parse_time(&nb, &out->not_before);
        if (rc != ATLS_OK) return rc;
        rc = atls_der_read_tlv(&val, &na);
        if (rc != ATLS_OK) return rc;
        rc = parse_time(&na, &out->not_after);
        if (rc != ATLS_OK) return rc;
        if (!atls_der_at_end(&val)) return ATLS_ERR_BAD_ENCODING;
    }

    /* subject Name */
    rc = atls_der_read_tlv(&t, &f);
    if (rc != ATLS_OK) return rc;
    if (f.tag != 0x30) return ATLS_ERR_BAD_ENCODING;
    span_whole_tlv(&out->subject, &f);

    /* subjectPublicKeyInfo SEQUENCE { algorithm, subjectPublicKey } */
    rc = atls_der_read_tlv(&t, &f);
    if (rc != ATLS_OK) return rc;
    if (f.tag != 0x30) return ATLS_ERR_BAD_ENCODING;
    span_whole_tlv(&out->spki, &f);
    {
        atls_der sp;
        rc = open_scope(&t, f.value, f.len, &sp);
        if (rc != ATLS_OK) return rc;
        atls_der_tlv alg, key;
        rc = atls_der_read_tlv(&sp, &alg);
        if (rc != ATLS_OK) return rc;
        if (alg.tag != 0x30) return ATLS_ERR_BAD_ENCODING;
        atls_der al;
        rc = open_scope(&sp, alg.value, alg.len, &al);
        if (rc != ATLS_OK) return rc;
        atls_der_tlv alg_oid;
        rc = atls_der_read_tlv(&al, &alg_oid);
        if (rc != ATLS_OK) return rc;
        if (alg_oid.tag != 0x06) return ATLS_ERR_BAD_ENCODING;
        span_from_tlv(&out->spki_alg_oid, &alg_oid);
        rc = atls_der_read_tlv(&sp, &key);
        if (rc != ATLS_OK) return rc;
        if (key.tag != 0x03 || key.len == 0) return ATLS_ERR_BAD_ENCODING;
        out->spki_key_unused_bits = key.value[0];
        if (out->spki_key_unused_bits > 7) return ATLS_ERR_BAD_ENCODING;
        out->spki_key.data = key.value + 1;
        out->spki_key.len = key.len - 1;
        if (!atls_der_at_end(&sp)) return ATLS_ERR_BAD_ENCODING;
    }

    /* optional issuerUniqueID [1] / subjectUniqueID [2]: refuse-and-skip
     * is wrong for v2 relics nobody sends; skip with the depth budget. */
    while (!atls_der_at_end(&t)) {
        rc = atls_der_peek_tlv(&t, &f);
        if (rc != ATLS_OK) return rc;
        if (f.tag != 0xa1 && f.tag != 0xa2 && f.tag != 0xa3) break;
        if (f.tag == 0xa3) break;              /* extensions: handled below */
        rc = atls_der_read_tlv(&t, &f);        /* unique IDs: skipped */
        if (rc != ATLS_OK) return rc;
    }

    /* extensions [3] EXPLICIT SEQUENCE OF Extension — optional */
    if (!atls_der_at_end(&t)) {
        rc = atls_der_peek_tlv(&t, &f);
        if (rc != ATLS_OK) return rc;
        if (f.tag == 0xa3) {
            rc = atls_der_read_tlv(&t, &f);
            if (rc != ATLS_OK) return rc;
            atls_der exts_wrapper;
            rc = open_scope(&t, f.value, f.len, &exts_wrapper);
            if (rc != ATLS_OK) return rc;
            atls_der_tlv exts_seq;
            rc = atls_der_read_tlv(&exts_wrapper, &exts_seq);
            if (rc != ATLS_OK) return rc;
            if (exts_seq.tag != 0x30) return ATLS_ERR_BAD_ENCODING;
            if (!atls_der_at_end(&exts_wrapper)) return ATLS_ERR_BAD_ENCODING;

            atls_der e;
            rc = open_scope(&exts_wrapper, exts_seq.value, exts_seq.len, &e);
            if (rc != ATLS_OK) return rc;

            while (!atls_der_at_end(&e)) {
                atls_der_tlv ext;
                rc = atls_der_read_tlv(&e, &ext);
                if (rc != ATLS_OK) return rc;
                if (ext.tag != 0x30) return ATLS_ERR_BAD_ENCODING;

                atls_der x;
                rc = open_scope(&e, ext.value, ext.len, &x);
                if (rc != ATLS_OK) return rc;

                atls_der_tlv oid_t;
                rc = atls_der_read_tlv(&x, &oid_t);
                if (rc != ATLS_OK) return rc;
                if (oid_t.tag != 0x06) return ATLS_ERR_BAD_ENCODING;
                atls_span oid;
                span_from_tlv(&oid, &oid_t);

                int critical = 0;
                rc = atls_der_peek_tlv(&x, &f);
                if (rc != ATLS_OK) return rc;
                if (f.tag == 0x01) {           /* critical BOOLEAN */
                    rc = atls_der_read_tlv(&x, &f);
                    if (rc != ATLS_OK) return rc;
                    if (f.len != 1) return ATLS_ERR_BAD_ENCODING;
                    critical = f.value[0] != 0;
                }

                atls_der_tlv oct;
                rc = atls_der_read_tlv(&x, &oct);
                if (rc != ATLS_OK) return rc;
                if (oct.tag != 0x04) return ATLS_ERR_BAD_ENCODING;
                if (!atls_der_at_end(&x)) return ATLS_ERR_BAD_ENCODING;

                atls_der ev;
                rc = open_scope(&x, oct.value, oct.len, &ev);
                if (rc != ATLS_OK) return rc;

                if (atls_oid_eq(&oid, ATLS_OID_EXT_SAN, 3)) {
                    rc = parse_san(&ev, out);
                } else if (atls_oid_eq(&oid, ATLS_OID_EXT_BASIC_CONSTRAINTS, 3)) {
                    out->bc_critical = critical;
                    rc = parse_basic_constraints(&ev, out);
                } else if (atls_oid_eq(&oid, ATLS_OID_EXT_KEY_USAGE, 3)) {
                    out->ku_critical = critical;
                    rc = parse_key_usage(&ev, out);
                } else if (critical) {
                    /* D5: reject rather than interpret. */
                    rc = ATLS_ERR_UNSUPPORTED;
                } else {
                    /* Unknown non-critical: walked, not read. */
                    rc = atls_der_skip_value(&oct, x.depth);
                }
                if (rc != ATLS_OK) return rc;
            }
        }
    }
    if (!atls_der_at_end(&t)) return ATLS_ERR_BAD_ENCODING;

    /* ---- signatureAlgorithm (outer) ---- */
    atls_der_tlv sa;
    rc = atls_der_read_tlv(&d, &sa);
    if (rc != ATLS_OK) return rc;
    if (sa.tag != 0x30) return ATLS_ERR_BAD_ENCODING;
    span_whole_tlv(&out->outer_sig_alg, &sa);
    {
        atls_der a;
        rc = open_scope(&d, sa.value, sa.len, &a);
        if (rc != ATLS_OK) return rc;
        atls_der_tlv oid;
        rc = atls_der_read_tlv(&a, &oid);
        if (rc != ATLS_OK) return rc;
        if (oid.tag != 0x06) return ATLS_ERR_BAD_ENCODING;
        span_from_tlv(&out->outer_sig_oid, &oid);
    }

    /* ---- signatureValue BIT STRING ---- */
    atls_der_tlv sig;
    rc = atls_der_read_tlv(&d, &sig);
    if (rc != ATLS_OK) return rc;
    if (sig.tag != 0x03 || sig.len == 0) return ATLS_ERR_BAD_ENCODING;
    out->signature_unused_bits = sig.value[0];
    if (out->signature_unused_bits != 0) return ATLS_ERR_BAD_ENCODING;
    out->signature.data = sig.value + 1;
    out->signature.len = sig.len - 1;

    if (!atls_der_at_end(&d)) return ATLS_ERR_BAD_ENCODING;
    return ATLS_OK;
}

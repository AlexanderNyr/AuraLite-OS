/* tests/unit/atls_x509_testdata.c — crafted DER for the N2 gates.
 *
 * A tiny bottom-up DER writer plus three builders the test batteries use:
 *
 *   build_minimal_cert(version, critical_unknown_ext, include_ext)
 *       The smallest X.509 v3 shape the parser accepts: Ed25519-flavoured
 *       OIDs, empty Names, zero keys/signatures.  Variants exercise the
 *       v1 refusal, the unknown-critical-extension refusal (D5), and the
 *       extension-less case.
 *
 *   build_cert_with_san_blob(blob, len)
 *       Same minimal certificate, but the SAN extension's OCTET STRING
 *       contains `blob` verbatim — this is how the depth battery smuggles
 *       a 10 000-deep nesting into the parser's walking path.
 *
 *   atls_der_skip_value_test(p, n)
 *       Drives the internal iterative skipper directly for the depth gate.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "atls_der.h"
#include "atls/atls.h"

/* ---- tiny DER writer ---- */

static size_t emit_len(uint8_t *out, size_t len) {
    if (len < 0x80) {
        out[0] = (uint8_t)len;
        return 1;
    }
    if (len < 0x100) {
        out[0] = 0x81;
        out[1] = (uint8_t)len;
        return 2;
    }
    out[0] = 0x82;
    out[1] = (uint8_t)(len >> 8);
    out[2] = (uint8_t)len;
    return 3;
}

static size_t emit_tlv(uint8_t *out, uint8_t tag, const uint8_t *content,
                       size_t clen) {
    out[0] = tag;
    size_t n = emit_len(out + 1, clen);
    for (size_t i = 0; i < clen; i++) out[1 + n + i] = content[i];
    return 1 + n + clen;
}

/* ---- fixed parts ---- */

static const uint8_t OID_ED25519[] = { 0x2b, 0x65, 0x70 };        /* 1.3.101.112 */
static const uint8_t OID_UNKNOWN[] = { 0x2b, 0x06, 0x01, 0x04, 0x01 }; /* 1.3.6.1.4.1 */
static const uint8_t OID_SAN[] = { 0x55, 0x1d, 0x11 };

static size_t build_alg_id(uint8_t *out, const uint8_t *oid, size_t oidlen) {
    uint8_t oid_tlv[16];
    size_t on = emit_tlv(oid_tlv, 0x06, oid, oidlen);
    return emit_tlv(out, 0x30, oid_tlv, on);
}

size_t build_minimal_cert(uint8_t *out, int version, int critical_unknown_ext,
                          int include_ext) {
    uint8_t tbs_content[512];
    size_t tlen = 0;

    /* version [0] EXPLICIT INTEGER */
    {
        uint8_t v = (uint8_t)version;
        uint8_t vi[8];
        size_t vn = emit_tlv(vi, 0x02, &v, 1);
        uint8_t wrap[16];
        size_t wn = emit_tlv(wrap, 0xa0, vi, vn);
        memcpy(tbs_content + tlen, wrap, wn);
        tlen += wn;
    }
    /* serialNumber INTEGER 1 */
    {
        uint8_t s = 0x01;
        tlen += emit_tlv(tbs_content + tlen, 0x02, &s, 1);
    }
    /* signature AlgorithmIdentifier */
    tlen += build_alg_id(tbs_content + tlen, OID_ED25519, sizeof(OID_ED25519));
    /* issuer Name (empty) */
    tlen += emit_tlv(tbs_content + tlen, 0x30, NULL, 0);
    /* validity: two UTCTimes */
    {
        uint8_t times[64];
        size_t tn = 0;
        tn += emit_tlv(times + tn, 0x17,
                       (const uint8_t *)"260101000000Z", 13);
        tn += emit_tlv(times + tn, 0x17,
                       (const uint8_t *)"360101000000Z", 13);
        tlen += emit_tlv(tbs_content + tlen, 0x30, times, tn);
    }
    /* subject Name (empty) */
    tlen += emit_tlv(tbs_content + tlen, 0x30, NULL, 0);
    /* subjectPublicKeyInfo */
    {
        uint8_t spki_content[128];
        size_t sn = build_alg_id(spki_content, OID_ED25519, sizeof(OID_ED25519));
        uint8_t keybits[33];
        memset(keybits, 0, sizeof(keybits));      /* unused-bits 0 + 32 zero bytes */
        uint8_t bs[48];
        size_t bn = emit_tlv(bs, 0x03, keybits, sizeof(keybits));
        memcpy(spki_content + sn, bs, bn);
        sn += bn;
        tlen += emit_tlv(tbs_content + tlen, 0x30, spki_content, sn);
    }
    /* extensions [3] */
    if (include_ext) {
        uint8_t ext_content[64];
        size_t xn = 0;
        xn += emit_tlv(ext_content + xn, 0x06, OID_UNKNOWN, sizeof(OID_UNKNOWN));
        if (critical_unknown_ext) {
            uint8_t t = 0xff;
            xn += emit_tlv(ext_content + xn, 0x01, &t, 1);
        }
        xn += emit_tlv(ext_content + xn, 0x04, NULL, 0);   /* empty OCTET STRING */

        uint8_t ext_seq[96];
        size_t esn = emit_tlv(ext_seq, 0x30, ext_content, xn);
        uint8_t exts_seq[128];
        size_t sen = emit_tlv(exts_seq, 0x30, ext_seq, esn);
        uint8_t wrap[160];
        size_t wn = emit_tlv(wrap, 0xa3, exts_seq, sen);
        memcpy(tbs_content + tlen, wrap, wn);
        tlen += wn;
    }

    uint8_t cert_content[768];
    size_t clen = 0;
    clen += emit_tlv(cert_content + clen, 0x30, tbs_content, tlen);   /* TBS */
    clen += build_alg_id(cert_content + clen, OID_ED25519, sizeof(OID_ED25519));
    {
        uint8_t sigbits[65];
        memset(sigbits, 0, sizeof(sigbits));      /* unused-bits 0 + 64 zero bytes */
        uint8_t bs[80];
        size_t bn = emit_tlv(bs, 0x03, sigbits, sizeof(sigbits));
        memcpy(cert_content + clen, bs, bn);
        clen += bn;
    }
    return emit_tlv(out, 0x30, cert_content, clen);
}

size_t build_cert_with_san_blob(uint8_t *out, const uint8_t *blob,
                                size_t blob_len) {
    static uint8_t tbs_content[65536];
    size_t tlen = 0;

    /* version, serial, alg, issuer, validity, subject, spki — same as the
     * minimal certificate. */
    {
        uint8_t v = 2;
        uint8_t vi[8];
        size_t vn = emit_tlv(vi, 0x02, &v, 1);
        tlen += emit_tlv(tbs_content + tlen, 0xa0, vi, vn);
    }
    {
        uint8_t s = 0x01;
        tlen += emit_tlv(tbs_content + tlen, 0x02, &s, 1);
    }
    tlen += build_alg_id(tbs_content + tlen, OID_ED25519, sizeof(OID_ED25519));
    tlen += emit_tlv(tbs_content + tlen, 0x30, NULL, 0);
    {
        uint8_t times[64];
        size_t tn = 0;
        tn += emit_tlv(times + tn, 0x17, (const uint8_t *)"260101000000Z", 13);
        tn += emit_tlv(times + tn, 0x17, (const uint8_t *)"360101000000Z", 13);
        tlen += emit_tlv(tbs_content + tlen, 0x30, times, tn);
    }
    tlen += emit_tlv(tbs_content + tlen, 0x30, NULL, 0);
    {
        uint8_t spki_content[128];
        size_t sn = build_alg_id(spki_content, OID_ED25519, sizeof(OID_ED25519));
        uint8_t keybits[33];
        memset(keybits, 0, sizeof(keybits));
        uint8_t bs[48];
        size_t bn = emit_tlv(bs, 0x03, keybits, sizeof(keybits));
        memcpy(spki_content + sn, bs, bn);
        sn += bn;
        tlen += emit_tlv(tbs_content + tlen, 0x30, spki_content, sn);
    }
    /* extensions [3] { SEQUENCE { SEQUENCE { OID SAN, OCTET STRING blob } } } */
    {
        /* One static scratch buffer per wrapping stage; the battery's blob
         * can be ~47 KiB, so no small stack arrays here. */
        static uint8_t ext_body[65536];
        static uint8_t ext_seq[65536];
        static uint8_t exts_seq[65536];
        static uint8_t a3_wrap[65536];

        size_t xn = 0;
        xn += emit_tlv(ext_body + xn, 0x06, OID_SAN, sizeof(OID_SAN));
        xn += emit_tlv(ext_body + xn, 0x04, blob, blob_len);
        size_t esn = emit_tlv(ext_seq, 0x30, ext_body, xn);
        size_t sen = emit_tlv(exts_seq, 0x30, ext_seq, esn);
        size_t wn = emit_tlv(a3_wrap, 0xa3, exts_seq, sen);
        memcpy(tbs_content + tlen, a3_wrap, wn);
        tlen += wn;
    }

    static uint8_t cert_content[65536];
    size_t clen = 0;
    clen += emit_tlv(cert_content + clen, 0x30, tbs_content, tlen);
    clen += build_alg_id(cert_content + clen, OID_ED25519, sizeof(OID_ED25519));
    {
        uint8_t sigbits[65];
        memset(sigbits, 0, sizeof(sigbits));
        uint8_t bs[80];
        size_t bn = emit_tlv(bs, 0x03, sigbits, sizeof(sigbits));
        memcpy(cert_content + clen, bs, bn);
        clen += bn;
    }
    return emit_tlv(out, 0x30, cert_content, clen);
}

int atls_der_skip_value_test(const uint8_t *p, size_t n) {
    atls_der d;
    int rc = atls_der_init(&d, p, n);
    if (rc != ATLS_OK) return rc;
    atls_der_tlv t;
    rc = atls_der_read_tlv(&d, &t);
    if (rc != ATLS_OK) return rc;
    return atls_der_skip_value(&t, 0);
}

/* Emit `levels` nested SEQUENCEs around a NULL payload into buf, from
 * the inside out.  Returns total size; buf must hold ~5 bytes per level
 * (~48 KiB for 10 000 levels).  Shared by the host battery and the
 * in-guest x509test so both halves of the depth gate run the identical
 * construction. */
size_t atls_test_build_deep_nest(uint8_t *buf, size_t bufcap, int levels) {
    uint8_t payload[2] = { 0x05, 0x00 };
    size_t cur_len = 2;
    if (bufcap < cur_len + (size_t)levels * 5) return 0;
    size_t off = bufcap - cur_len;
    memcpy(buf + off, payload, cur_len);

    for (int i = 0; i < levels; i++) {
        uint8_t hdr[5];
        size_t hlen;
        hdr[0] = 0x30;
        if (cur_len < 0x80) {
            hdr[1] = (uint8_t)cur_len;
            hlen = 2;
        } else if (cur_len < 0x100) {
            hdr[1] = 0x81;
            hdr[2] = (uint8_t)cur_len;
            hlen = 3;
        } else if (cur_len < 0x10000) {
            hdr[1] = 0x82;
            hdr[2] = (uint8_t)(cur_len >> 8);
            hdr[3] = (uint8_t)cur_len;
            hlen = 4;
        } else {
            hdr[1] = 0x83;
            hdr[2] = (uint8_t)(cur_len >> 16);
            hdr[3] = (uint8_t)(cur_len >> 8);
            hdr[4] = (uint8_t)cur_len;
            hlen = 5;
        }
        off -= hlen;
        memcpy(buf + off, hdr, hlen);
        cur_len += hlen;
    }
    /* Slide the blob down to the buffer start.  The destination (buf) is
     * BELOW the source (buf+off), so a forward copy never overwrites
     * bytes it has not read yet; a plain loop keeps this independent of
     * memmove, which the guest libc does not provide. */
    for (size_t i = 0; i < cur_len; i++) buf[i] = buf[off + i];
    return cur_len;
}

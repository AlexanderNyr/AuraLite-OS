/* atls_pem.c — PEM base64 decoding for trust-store roots
 * (REALINTERNET_PLAN.md phase X2).
 *
 * Parses the first "-----BEGIN CERTIFICATE-----" ... "-----END CERTIFICATE-----"
 * block in a PEM text and decodes its base64 body into DER.  No allocation:
 * the caller supplies the output buffer and its capacity, and every length is
 * checked.  Decoding certificates we already trust, so this is not a
 * security boundary; it is a small, bounded utility.
 */

#include "atls/pem.h"
#include <string.h>

/* ---- base64 ---- */

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode base64 (no line breaks expected).  Returns bytes written, or -1. */
static int b64_decode(const char *in, size_t in_len,
                      uint8_t *out, size_t out_cap) {
    size_t w = 0;
    uint32_t acc = 0;
    int nbits = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        if (c == '=') break;              /* padding */
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = b64_val(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            if (w >= out_cap) return -1;
            out[w++] = (uint8_t)((acc >> nbits) & 0xFF);
        }
    }
    return (int)w;
}

int atls_pem_cert_to_der(const char *pem, size_t pem_len,
                         uint8_t *der, size_t der_cap, size_t *der_len) {
    if (!pem || !der || !der_len) return ATLS_ERR_INPUT;

    static const char begin[] = "-----BEGIN CERTIFICATE-----";
    static const char end[] = "-----END CERTIFICATE-----";
    const size_t bl = sizeof(begin) - 1;
    const size_t el = sizeof(end) - 1;

    /* Find the begin marker. */
    size_t bpos = 0;
    for (; bpos + bl <= pem_len; bpos++) {
        if (memcmp(pem + bpos, begin, bl) == 0) break;
    }
    if (bpos + bl > pem_len) return ATLS_ERR_BAD_ENCODING;

    /* Find the end marker after it. */
    size_t body_start = bpos + bl;
    size_t epos = body_start;
    for (; epos + el <= pem_len; epos++) {
        if (memcmp(pem + epos, end, el) == 0) break;
    }
    if (epos + el > pem_len) return ATLS_ERR_BAD_ENCODING;

    /* base64 body is between the two markers. */
    size_t body_len = epos - body_start;
    int n = b64_decode(pem + body_start, body_len, der, der_cap);
    if (n < 0) return ATLS_ERR_BAD_ENCODING;
    *der_len = (size_t)n;
    return ATLS_OK;
}

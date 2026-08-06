/* atls_der.c — DER TLV reader (INTERNET_PLAN.md phase N2).
 *
 * Zero allocation, bounded depth, bounded lengths — see atls_der.h for
 * the contract.  Every parser in this phase fails CLOSED: any path that
 * does not recognise the input as strict DER returns an error rather
 * than guessing.
 */

#include "atls_der.h"
#include "atls/atls.h"

int atls_der_init(atls_der *d, const uint8_t *data, size_t len) {
    if (!d || (!data && len)) return ATLS_ERR_INPUT;
    d->p = data;
    d->end = data + len;
    d->depth = 0;
    return ATLS_OK;
}

int atls_der_at_end(const atls_der *d) {
    return d->p >= d->end;
}

/* Parse one TLV starting at d->p, bounded by d->end.  Does not advance. */
static int parse_tlv(const uint8_t *p, const uint8_t *end, atls_der_tlv *out) {
    if (p >= end) return ATLS_ERR_TRUNCATED;

    uint8_t tag = p[0];
    out->tag = tag;
    out->tag_class = (tag >> 6) & 3;
    out->constructed = (tag & 0x20) != 0;
    out->header = p;

    /* High-tag-number form (tag & 0x1f == 0x1f) never appears in the
     * X.509 structures this project reads; refuse instead of parsing a
     * shape nobody has audited. */
    if ((tag & 0x1f) == 0x1f) return ATLS_ERR_UNSUPPORTED;

    const uint8_t *q = p + 1;
    if (q >= end) return ATLS_ERR_TRUNCATED;

    size_t len;
    size_t header_len;
    uint8_t first = q[0];

    if (first < 0x80) {
        len = first;
        header_len = 2;
    } else if (first == 0x80) {
        /* Indefinite length: not valid DER, and the classic smuggling
         * shape.  Refuse. */
        return ATLS_ERR_BAD_LENGTH;
    } else {
        int n = first & 0x7f;
        if (n > 8) return ATLS_ERR_BAD_LENGTH;     /* > 2^64 is absurd */
        if (q + 1 + n > end) return ATLS_ERR_TRUNCATED;
        /* DER minimality: no leading zero in the length octets, and the
         * value must actually need this many octets. */
        if (q[1] == 0) return ATLS_ERR_BAD_LENGTH;
        if (n == 1 && q[1] < 0x80) return ATLS_ERR_BAD_LENGTH;
        len = 0;
        for (int i = 0; i < n; i++) {
            len = (len << 8) | q[1 + i];
        }
        header_len = (size_t)(2 + n);
    }

    const uint8_t *value = p + header_len;
    /* The decisive bounds check: the claimed length must fit inside the
     * enclosing scope.  This is where a 4 GiB claim dies — nothing is
     * allocated, nothing is even iterated. */
    if ((size_t)(end - value) < len) return ATLS_ERR_TRUNCATED;

    out->header_len = header_len;
    out->value = value;
    out->len = len;
    return ATLS_OK;
}

int atls_der_peek_tlv(const atls_der *d, atls_der_tlv *out) {
    return parse_tlv(d->p, d->end, out);
}

int atls_der_read_tlv(atls_der *d, atls_der_tlv *out) {
    int rc = parse_tlv(d->p, d->end, out);
    if (rc != ATLS_OK) return rc;
    d->p = out->value + out->len;
    return ATLS_OK;
}

int atls_der_enter(atls_der *d, const atls_der_tlv *t) {
    if (!t->constructed) return ATLS_ERR_BAD_ENCODING;
    if (d->depth + 1 > ATLS_DER_MAX_DEPTH) return ATLS_ERR_DEPTH;
    d->p = t->value;
    d->end = t->value + t->len;
    d->depth++;
    return ATLS_OK;
}

/* Iterative walk over arbitrary constructed content.  A small explicit
 * frame table replaces recursion: a 10 000-deep nest runs out of depth
 * budget long before it runs out of anything else. */
int atls_der_skip_value(const atls_der_tlv *t, int start_depth) {
    struct frame {
        const uint8_t *p;
        const uint8_t *end;
    } frames[ATLS_DER_MAX_DEPTH];

    if (!t->constructed) return ATLS_OK;
    if (start_depth + 1 > ATLS_DER_MAX_DEPTH) return ATLS_ERR_DEPTH;

    int n = 0;
    frames[n].p = t->value;
    frames[n].end = t->value + t->len;
    n++;

    while (n > 0) {
        struct frame *f = &frames[n - 1];
        if (f->p >= f->end) {
            n--;
            continue;
        }
        atls_der_tlv child;
        int rc = parse_tlv(f->p, f->end, &child);
        if (rc != ATLS_OK) return rc;
        f->p = child.value + child.len;
        if (child.constructed) {
            if (start_depth + n + 1 > ATLS_DER_MAX_DEPTH) {
                return ATLS_ERR_DEPTH;
            }
            if (n >= ATLS_DER_MAX_DEPTH) return ATLS_ERR_DEPTH;
            frames[n].p = child.value;
            frames[n].end = child.value + child.len;
            n++;
        }
    }
    return ATLS_OK;
}

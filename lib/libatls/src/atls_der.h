#ifndef LIBATLS_ATLS_DER_H
#define LIBATLS_ATLS_DER_H

/* atls_der.h — DER TLV reader (INTERNET_PLAN.md phase N2).  Internal.
 *
 * This is the code that reads attacker-controlled, deeply nested,
 * length-prefixed binary from a stranger, so its contract is written
 * around refusal:
 *
 *   - ZERO allocation.  Everything is a span into the caller's buffer;
 *     a length field claiming 4 GiB costs the parser nothing but an
 *     error.  "Bounded memory" holds by construction.
 *   - Explicit DEPTH budget.  The reader carries the nesting consumed
 *     so far, and no operation descends past ATLS_DER_MAX_DEPTH.  The
 *     10 000-deep certificate in the test battery dies at depth 32, not
 *     at the bottom of the 64 KiB user stack.
 *   - Explicit LENGTH checks.  Long-form lengths are bounds-checked
 *     against the remaining buffer BEFORE anything else looks at them;
 *     indefinite length (0x80) is not DER and is refused; non-minimal
 *     encodings are refused.
 *   - Skipping unknown constructed content walks ITERATIVELY with its
 *     own frame table — never recursively.
 */

#include <stdint.h>
#include <stddef.h>

#define ATLS_DER_MAX_DEPTH 32   /* real X.509 nests ~7 levels deep */

typedef struct {
    const uint8_t *p;      /* current read position */
    const uint8_t *end;    /* end of the enclosing value */
    int depth;             /* nesting consumed to reach this scope */
} atls_der;

typedef struct {
    uint8_t tag;           /* full first tag byte */
    int tag_class;         /* 0 universal, 1 application, 2 context, 3 private */
    int constructed;
    const uint8_t *header; /* first byte of the TLV */
    size_t header_len;
    const uint8_t *value;  /* contents octets */
    size_t len;            /* length of contents */
} atls_der_tlv;

/* Result codes are the public ATLS_ERR_* values from atls.h:
 *   ATLS_OK, ATLS_ERR_TRUNCATED, ATLS_ERR_BAD_LENGTH, ATLS_ERR_DEPTH,
 *   ATLS_ERR_BAD_ENCODING. */

int atls_der_init(atls_der *d, const uint8_t *data, size_t len);

/* Read the next TLV in this scope and advance past it. */
int atls_der_read_tlv(atls_der *d, atls_der_tlv *out);

/* Read the next TLV WITHOUT consuming it. */
int atls_der_peek_tlv(const atls_der *d, atls_der_tlv *out);

/* Descend into a constructed TLV's value as a new scope (depth + 1,
 * refused at the budget). */
int atls_der_enter(atls_der *d, const atls_der_tlv *t);

/* True when the scope is exhausted. */
int atls_der_at_end(const atls_der *d);

/* Walk an arbitrary (possibly constructed) value without interpreting
 * it, honouring the depth budget from `start_depth`.  This is how
 * unknown non-critical extensions and unknown GeneralName choices are
 * stepped over — iteratively, so no hostile nesting reaches the stack. */
int atls_der_skip_value(const atls_der_tlv *t, int start_depth);

#endif /* LIBATLS_ATLS_DER_H */

/* atls_common.c — constant-time utilities (INTERNET_PLAN.md D7).
 *
 * The rule, stated once and enforced by test_atls_hash grepping these
 * sources: NOTHING in libatls compares secret or MAC/signature material
 * with memcmp/strncmp/bcmp.  The one primitive for that is atls_ct_eq.
 */

#include "atls/atls.h"

int atls_ct_eq(const uint8_t *a, const uint8_t *b, size_t len) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    /* diff == 0 -> 1, else 0: (0u - 1) is all-ones in 32 bits. */
    return (int)(((uint32_t)diff - 1u) >> 31);
}

void atls_wipe(void *p, size_t len) {
    volatile uint8_t *q = (volatile uint8_t *)p;
    for (size_t i = 0; i < len; i++) q[i] = 0;
}

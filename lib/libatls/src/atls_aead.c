/* atls_aead.c — AEAD_CHACHA20_POLY1305 (RFC 8439 section 2.8).
 *
 * The record-layer cipher of this stack (decision D4: ChaCha20-Poly1305
 * over AES-GCM, because it is fast and constant-time in portable C).
 *
 * The Poly1305 input layout is exactly §2.8:
 *     aad || pad16(aad) || ciphertext || pad16(ciphertext)
 *         || le64(aadlen) || le64(ctlen)
 *
 * Decryption verifies the tag (with atls_ct_eq) BEFORE any plaintext is
 * handed out, and wipes the output buffer on failure — release nothing
 * unauthenticated.
 */

#include "atls/atls.h"

/* Declared, not included: the guest's <stdlib.h> and the host's both
 * provide these, and keeping the library header-free here means this
 * file compiles identically in both worlds. */
extern void *malloc(size_t);
extern void  free(void *);

static void pad16_append(uint8_t *dst, size_t *off, size_t section_len) {
    size_t pad = (16 - (section_len % 16)) % 16;
    for (size_t i = 0; i < pad; i++) dst[(*off)++] = 0;
}

static void le64_put(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

/* Build the Poly1305 input for §2.8.  Returns a malloc'd buffer (caller
 * wipes+frees) or NULL. */
static uint8_t *mac_data(const uint8_t *aad, size_t aadlen,
                         const uint8_t *ct, size_t ctlen,
                         size_t *total) {
    size_t pad_aad = (16 - (aadlen % 16)) % 16;
    size_t pad_ct  = (16 - (ctlen % 16)) % 16;
    size_t n = aadlen + pad_aad + ctlen + pad_ct + 16;
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) return NULL;

    size_t off = 0;
    for (size_t i = 0; i < aadlen; i++) buf[off++] = aad[i];
    pad16_append(buf, &off, aadlen);
    for (size_t i = 0; i < ctlen; i++) buf[off++] = ct[i];
    pad16_append(buf, &off, ctlen);
    le64_put(buf + off, (uint64_t)aadlen); off += 8;
    le64_put(buf + off, (uint64_t)ctlen);  off += 8;

    *total = n;
    return buf;
}

int atls_aead_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *aad, size_t aadlen,
                      const uint8_t *pt, size_t ptlen,
                      uint8_t *ct, uint8_t tag[16]) {
    if (!key || !nonce || !tag) return ATLS_ERR_INPUT;
    if ((!aad && aadlen) || (!pt && ptlen) || (!ct && ptlen)) {
        return ATLS_ERR_INPUT;
    }

    uint8_t poly_key[64] = { 0 };
    atls_chacha20_xor(key, 0, nonce, poly_key, poly_key, 64);  /* block 0 */

    atls_chacha20_xor(key, 1, nonce, pt, ct, ptlen);           /* ctr 1.. */

    size_t n = 0;
    uint8_t *md = mac_data(aad, aadlen, ct, ptlen, &n);
    if (!md) return ATLS_ERR_INPUT;
    atls_poly1305(poly_key, md, n, tag);

    atls_wipe(md, n);
    free(md);
    atls_wipe(poly_key, sizeof(poly_key));
    return ATLS_OK;
}

int atls_aead_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *aad, size_t aadlen,
                      const uint8_t *ct, size_t ctlen,
                      const uint8_t tag[16], uint8_t *pt) {
    if (!key || !nonce || !tag) return ATLS_ERR_INPUT;
    if ((!aad && aadlen) || (!ct && ctlen) || (!pt && ctlen)) {
        return ATLS_ERR_INPUT;
    }

    uint8_t poly_key[64] = { 0 };
    atls_chacha20_xor(key, 0, nonce, poly_key, poly_key, 64);

    size_t n = 0;
    uint8_t *md = mac_data(aad, aadlen, ct, ctlen, &n);
    if (!md) return ATLS_ERR_INPUT;
    uint8_t check[16];
    atls_poly1305(poly_key, md, n, check);
    atls_wipe(md, n);
    free(md);
    atls_wipe(poly_key, sizeof(poly_key));

    if (!atls_ct_eq(check, tag, 16)) {
        atls_wipe(check, sizeof(check));
        if (pt && ctlen) atls_wipe(pt, ctlen);   /* release nothing */
        return ATLS_ERR_AUTH;
    }
    atls_wipe(check, sizeof(check));

    atls_chacha20_xor(key, 1, nonce, ct, pt, ctlen);
    return ATLS_OK;
}

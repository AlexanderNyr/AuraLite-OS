/* atls_hmac.c — HMAC-SHA256 (RFC 2104).
 *
 * Streaming API.  Keys longer than the 64-byte block are hashed first,
 * as the RFC requires.
 */

#include "atls/atls.h"

void atls_hmac_sha256_init(atls_hmac_sha256_ctx *c,
                           const uint8_t *key, size_t keylen) {
    uint8_t kblock[64];
    for (int i = 0; i < 64; i++) kblock[i] = 0;

    if (keylen > 64) {
        uint8_t kh[32];
        atls_sha256(key, keylen, kh);
        for (int i = 0; i < 32; i++) kblock[i] = kh[i];
        atls_wipe(kh, sizeof(kh));
    } else if (key && keylen) {
        for (size_t i = 0; i < keylen; i++) kblock[i] = key[i];
    }

    /* Inner hash starts over (ipad ^ key) || message... */
    atls_sha256_init(&c->inner);
    for (int i = 0; i < 64; i++) {
        uint8_t b = (uint8_t)(kblock[i] ^ 0x36);
        atls_sha256_update(&c->inner, &b, 1);
    }
    /* Precompute the outer padded block; final() hashes it over the
     * inner digest. */
    for (int i = 0; i < 64; i++) {
        c->opad_hash_prefix[i] = (uint8_t)(kblock[i] ^ 0x5c);
    }
    atls_wipe(kblock, sizeof(kblock));
}

void atls_hmac_sha256_update(atls_hmac_sha256_ctx *c,
                             const void *data, size_t len) {
    atls_sha256_update(&c->inner, data, len);
}

void atls_hmac_sha256_final(atls_hmac_sha256_ctx *c, uint8_t out[32]) {
    uint8_t inner_digest[32];
    atls_sha256_final(&c->inner, inner_digest);

    atls_sha256_ctx outer;
    atls_sha256_init(&outer);
    atls_sha256_update(&outer, c->opad_hash_prefix, 64);
    atls_sha256_update(&outer, inner_digest, 32);
    atls_sha256_final(&outer, out);

    atls_wipe(inner_digest, sizeof(inner_digest));
    atls_wipe(c, sizeof(*c));
}

void atls_hmac_sha256(const uint8_t *key, size_t keylen,
                      const void *msg, size_t msglen, uint8_t out[32]) {
    atls_hmac_sha256_ctx c;
    atls_hmac_sha256_init(&c, key, keylen);
    atls_hmac_sha256_update(&c, msg, msglen);
    atls_hmac_sha256_final(&c, out);
}

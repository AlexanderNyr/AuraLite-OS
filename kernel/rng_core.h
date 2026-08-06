#ifndef AURALITE_KERNEL_RNG_CORE_H
#define AURALITE_KERNEL_RNG_CORE_H

/* kernel/rng_core.h — ChaCha20 DRBG core (INTERNET_PLAN.md phase N0).
 *
 * A pure, freestanding header: no kernel includes, no inline asm, no
 * globals.  The exact same code compiles into the kernel (kernel/rng.c)
 * and into the host-side unit test (tests/unit/test_rng.c), so the bytes
 * the QEMU guest generates are the bytes the RFC-vector test verified.
 * This follows the tree's existing "pure core, host-tested" pattern
 * (kernel/lib/bitmap.h, kernel/mm/heap.c).
 *
 * Construction:
 *
 *   - ChaCha20 block function exactly as RFC 8439 section 2.3 (constants,
 *     32-bit key words, 32-bit block counter, 96-bit nonce, 20 rounds).
 *   - DRBG state = (256-bit key, 96-bit nonce, 32-bit block counter).
 *     Seeding consumes RNGC_SEED_LEN = 48 bytes of entropy material:
 *     first 32 -> key, next 12 -> nonce, counter reset to 0.
 *   - rngc_drbg_fill() serves output keystream, then IMMEDIATELY re-keys
 *     from the next 48 bytes of its own stream ("key after every request").
 *     This gives backtracking resistance: compromising the current state
 *     does not reveal bytes already handed out.
 *   - rngc_drbg_reseed() folds fresh entropy material into key+nonce with
 *     XOR and resets the counter, so a reseed never re-enters an earlier
 *     keystream position with the same key.
 *
 * What this file deliberately does NOT do: choose or gather entropy.  The
 * quality of the output is exactly the quality of the material handed to
 * seed/reseed; that responsibility (RDSEED / RDRAND / interrupt jitter,
 * with a loud -ENOSYS refusal when nothing good exists) lives in
 * kernel/rng.c, per INTERNET_PLAN.md decision D1.
 */

#include <stdint.h>
#include <stddef.h>
/* NOTE: deliberately no <string.h> and no kernel includes — this header is
 * freestanding so the exact same code compiles into the kernel and into the
 * host unit test.  The byte copy it needs is its own tiny loop. */

static void rngc_copy(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

#define RNGC_SEED_LEN 48   /* 32 key bytes + 12 nonce bytes + 4 pad bytes */

static inline uint32_t rngc_rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

#define RNGC_QR(a, b, c, d)                 \
    do {                                    \
        a += b; d ^= a; d = rngc_rotl32(d, 16); \
        c += d; b ^= c; b = rngc_rotl32(b, 12); \
        a += b; d ^= a; d = rngc_rotl32(d, 8);  \
        c += d; b ^= c; b = rngc_rotl32(b, 7);  \
    } while (0)

/* One ChaCha20 block (RFC 8439 §2.3).  `out` receives sixteen 32-bit words
 * in little-endian word order — the same order the RFC vectors print. */
static void rngc_chacha20_block(const uint32_t key[8], uint32_t counter,
                                const uint32_t nonce[3], uint32_t out[16]) {
    uint32_t st[16], x[16];
    int i, round;

    st[0] = 0x61707865u;  /* "expa" */
    st[1] = 0x3320646eu;  /* "nd 3" */
    st[2] = 0x79622d32u;  /* "2-by" */
    st[3] = 0x6b206574u;  /* "te k" */
    for (i = 0; i < 8; i++) st[4 + i] = key[i];
    st[12] = counter;
    st[13] = nonce[0];
    st[14] = nonce[1];
    st[15] = nonce[2];

    for (i = 0; i < 16; i++) x[i] = st[i];
    for (round = 0; round < 10; round++) {
        /* column round */
        RNGC_QR(x[0], x[4], x[8],  x[12]);
        RNGC_QR(x[1], x[5], x[9],  x[13]);
        RNGC_QR(x[2], x[6], x[10], x[14]);
        RNGC_QR(x[3], x[7], x[11], x[15]);
        /* diagonal round */
        RNGC_QR(x[0], x[5], x[10], x[15]);
        RNGC_QR(x[1], x[6], x[11], x[12]);
        RNGC_QR(x[2], x[7], x[8],  x[13]);
        RNGC_QR(x[3], x[4], x[9],  x[14]);
    }
    for (i = 0; i < 16; i++) out[i] = x[i] + st[i];
}

/* Serialise a block as 64 little-endian bytes (the keystream order used by
 * RFC 8439 §2.4 encryption). */
static void rngc_chacha20_block_bytes(const uint32_t key[8], uint32_t counter,
                                      const uint32_t nonce[3], uint8_t out[64]) {
    uint32_t w[16];
    int i;
    rngc_chacha20_block(key, counter, nonce, w);
    for (i = 0; i < 16; i++) {
        out[i * 4 + 0] = (uint8_t)(w[i] >> 0);
        out[i * 4 + 1] = (uint8_t)(w[i] >> 8);
        out[i * 4 + 2] = (uint8_t)(w[i] >> 16);
        out[i * 4 + 3] = (uint8_t)(w[i] >> 24);
    }
}

/* Little-endian word loads, for building keys/nonces from byte material. */
static inline uint32_t rngc_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct rngc_drbg {
    uint32_t key[8];
    uint32_t nonce[3];
    uint32_t counter;
};

/* Install fresh entropy material (48 bytes) as the complete state. */
static void rngc_drbg_seed(struct rngc_drbg *d,
                           const uint8_t material[RNGC_SEED_LEN]) {
    int i;
    for (i = 0; i < 8; i++) d->key[i] = rngc_le32(material + i * 4);
    for (i = 0; i < 3; i++) d->nonce[i] = rngc_le32(material + 32 + i * 4);
    d->counter = 0;
}

/* Fold NEW entropy into the running state (XOR) and reset the block
 * counter.  XOR keeps every bit of old state mixed with every bit of new
 * material; the counter reset plus changed key/nonce guarantees no block
 * position is ever reused under the same key. */
static void rngc_drbg_reseed(struct rngc_drbg *d,
                             const uint8_t material[RNGC_SEED_LEN]) {
    int i;
    for (i = 0; i < 8; i++) d->key[i] ^= rngc_le32(material + i * 4);
    for (i = 0; i < 3; i++) d->nonce[i] ^= rngc_le32(material + 32 + i * 4);
    d->counter = 0;
}

/* Generate `len` keystream bytes into `out`, then re-key from the stream
 * itself so the served bytes can never be reconstructed from a later state
 * compromise (backtracking resistance). */
static void rngc_drbg_fill(struct rngc_drbg *d, uint8_t *out, size_t len) {
    uint8_t block[64];

    while (len > 0) {
        rngc_chacha20_block_bytes(d->key, d->counter, d->nonce, block);
        d->counter++;
        size_t take = len < 64 ? len : 64;
        rngc_copy(out, block, take);
        out += take;
        len -= take;
    }

    /* Re-key: the next 48 bytes of stream become the new state. */
    uint8_t fresh[RNGC_SEED_LEN + 16];
    rngc_chacha20_block_bytes(d->key, d->counter, d->nonce, fresh);
    d->counter++;
    rngc_drbg_seed(d, fresh);
}

#endif /* AURALITE_KERNEL_RNG_CORE_H */

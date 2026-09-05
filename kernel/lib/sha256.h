#ifndef AURALITE_LIB_SHA256_H
#define AURALITE_LIB_SHA256_H

/* kernel/lib/sha256.h — the kernel-local SHA-256 (RESIDUE2 T3).
 *
 * Design rule D2 keeps the kernel off libatls, and the btrfs SHA-256
 * checksum work needs a digest the kernel may call — so this is a
 * small, freestanding FIPS 180-4 SHA-256 vendored next to the kernel's
 * other pure-C utilities (bitmap.h, string.c).  One header, one .c, no
 * allocation, no locking (callers serialise their own contexts; the
 * btrfs mount lock is the first consumer's).  The SAME source compiles
 * into the kernel and into the host unit test (tests/unit/test_ksha256.c),
 * which carries the RFC 6234 vectors.
 *
 * This is deliberately NOT a second crypto policy layer: libatls stays
 * the userspace stack; this file exists only where D2 says libatls may
 * not go.
 */

#include <stdint.h>
#include <stddef.h>

#define KSHA256_DIGEST_SIZE 32u

/* One-shot digest of `len` bytes at `data` into `out[32]`. */
void ksha256(const void *data, size_t len, uint8_t out[KSHA256_DIGEST_SIZE]);

/* Streaming form, for inputs staged in pieces (unused by the first
 * consumer; kept so the API is the standard shape). */
struct ksha256_ctx {
    uint32_t h[8];
    uint64_t total;          /* bytes hashed so far */
    uint8_t  buf[64];
    size_t   buflen;
};

void ksha256_init(struct ksha256_ctx *ctx);
void ksha256_update(struct ksha256_ctx *ctx, const void *data, size_t len);
void ksha256_final(struct ksha256_ctx *ctx,
                   uint8_t out[KSHA256_DIGEST_SIZE]);

#endif /* AURALITE_LIB_SHA256_H */

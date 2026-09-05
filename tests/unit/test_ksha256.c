/* tests/unit/test_ksha256.c — host-side vector gate for the kernel-local
 * SHA-256 (RESIDUE2 T3).
 *
 * The kernel's btrfs checksum calls the SAME source file
 * (kernel/lib/sha256.c); this test pins it against the RFC 6234
 * (FIPS 180-4) vectors so a wrong digest never reaches disk.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/lib/sha256.h"

static int failures;

static void hex_of(const uint8_t *d, char out[65]) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = h[d[i] >> 4];
        out[i * 2 + 1] = h[d[i] & 0xF];
    }
    out[64] = '\0';
}

static void check(const char *name, const uint8_t *data, size_t len,
                  const char *want_hex, const char *hex_want) {
    (void)want_hex;
    uint8_t digest[32];
    char got[65];
    ksha256(data, len, digest);
    hex_of(digest, got);
    if (strcmp(got, hex_want) != 0) {
        printf("FAIL %s:\n  want %s\n  got  %s\n", name, hex_want, got);
        failures++;
    } else {
        printf("PASS %s\n", name);
    }
}

/* The streaming API must agree with the one-shot form byte for byte. */
static void check_stream(const char *name, const uint8_t *data, size_t len,
                         const char *hex_want) {
    struct ksha256_ctx ctx;
    uint8_t digest[32];
    char got[65];
    ksha256_init(&ctx);
    /* Feed in awkwardly-sized pieces to exercise the tail handling. */
    size_t off = 0;
    size_t step = 7;
    while (off < len) {
        size_t take = step < len - off ? step : len - off;
        ksha256_update(&ctx, data + off, take);
        off += take;
        step = step * 3 % 61 + 1;
    }
    ksha256_final(&ctx, digest);
    hex_of(digest, got);
    if (strcmp(got, hex_want) != 0) {
        printf("FAIL %s (streaming):\n  want %s\n  got  %s\n",
               name, hex_want, got);
        failures++;
    } else {
        printf("PASS %s (streaming)\n", name);
    }
}

int main(void) {
    /* RFC 6234 §8.5 / FIPS 180-4 test vectors. */

    /* SHA256("") */
    check("empty message", (const uint8_t *)"", 0, NULL,
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    /* SHA256("abc") */
    check("abc", (const uint8_t *)"abc", 3, NULL,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    /* 448-bit message */
    check("448-bit",
          (const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          56, NULL,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* 896-bit message (exercises two blocks plus a third padded one) */
    check("896-bit",
          (const uint8_t *)"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghij"
                           "klmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrst"
                           "nopqrstu",
          112, NULL,
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

    /* One million 'a' — the long-input gate, streamed. */
    {
        static uint8_t million[1000000];
        memset(million, 'a', sizeof(million));
        struct ksha256_ctx ctx;
        uint8_t digest[32];
        char got[65];
        ksha256_init(&ctx);
        for (int i = 0; i < 100; i++)
            ksha256_update(&ctx, million + i * 10000, 10000);
        ksha256_final(&ctx, digest);
        hex_of(digest, got);
        const char *want =
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
        if (strcmp(got, want) != 0) {
            printf("FAIL million-a:\n  want %s\n  got  %s\n", want, got);
            failures++;
        } else {
            printf("PASS million-a\n");
        }
    }

    /* Streaming must reproduce the RFC vectors fed in odd pieces. */
    check_stream("abc", (const uint8_t *)"abc", 3,
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_stream("448-bit",
                 (const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                 56,
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* Streaming == one-shot on a block-boundary input (exactly 64 bytes)
     * and one byte past it — the two padding branches. */
    {
        uint8_t blk[65];
        for (int i = 0; i < 65; i++)
            blk[i] = (uint8_t)i;
        uint8_t one[32], two[32];
        ksha256(blk, 64, one);
        struct ksha256_ctx ctx;
        ksha256_init(&ctx);
        ksha256_update(&ctx, blk, 63);
        ksha256_update(&ctx, blk + 63, 1);
        ksha256_final(&ctx, two);
        if (memcmp(one, two, 32) != 0) {
            printf("FAIL 64-byte boundary: one-shot != streaming\n");
            failures++;
        } else {
            printf("PASS 64-byte boundary parity\n");
        }
        /* 65 bytes: the padding spills into a second block; streaming in
         * odd pieces must still equal the one-shot digest. */
        uint8_t three[32], four[32];
        ksha256(blk, 65, three);
        struct ksha256_ctx ctx2;
        ksha256_init(&ctx2);
        ksha256_update(&ctx2, blk, 50);
        ksha256_update(&ctx2, blk + 50, 15);
        ksha256_final(&ctx2, four);
        if (memcmp(three, four, 32) != 0) {
            printf("FAIL 65-byte spill: one-shot != streaming\n");
            failures++;
        } else {
            printf("PASS 65-byte spill parity\n");
        }
    }

    /* The btrfs shape: a whole 4096-byte block with its first 32 bytes
     * zeroed (the checksum field's on-disk position). */
    {
        static uint8_t block[4096];
        for (size_t i = 0; i < sizeof(block); i++)
            block[i] = (uint8_t)(i * 31 + 7);
        memset(block, 0, 32);
        uint8_t d1[32], d2[32];
        ksha256(block, sizeof(block), d1);
        /* Determinism + sensitivity: one flipped byte MUST change the
         * digest (this is the whole point of the checksum). */
        block[2000] ^= 1;
        ksha256(block, sizeof(block), d2);
        if (memcmp(d1, d2, 32) == 0) {
            printf("FAIL 4096-byte sensitivity: flip invisible\n");
            failures++;
        } else {
            printf("PASS 4096-byte block digest + flip sensitivity\n");
        }
    }

    if (failures) {
        printf("[ksha256] %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("[ksha256] all SHA-256 vector checks passed\n");
    return 0;
}

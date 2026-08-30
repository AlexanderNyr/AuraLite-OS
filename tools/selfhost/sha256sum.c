/* tools/selfhost/sha256sum.c -- SELFHOST_PLAN.md SH7a: the in-guest hash tool.
 *
 * SH7 replaces the host-only image/build tooling (Fact 5) with C twins the
 * guest tcc can compile and the in-guest `sh build.sh` can run.  SH7a is the
 * first of those twins and the smallest: a `sha256sum` that reuses the ONE
 * SHA-256 implementation the tree already ships and tests (libatls, the same
 * code the TLS stack and cryptotest use) rather than adding a second copy
 * that could drift.  "one implementation, tested once".
 *
 * Modes (the probe drives these and branches on the exit status, because the
 * scripting shell has no `cut`/`grep` to parse a digest line):
 *
 *   sha256sum FILE...        print "<hex>  FILE" for each named file
 *   sha256sum                read stdin, print "<hex>  -"   (coreutils shape)
 *   sha256sum --selftest     run the published FIPS vectors; exit 0/1
 *   sha256sum --eq FILE      hash stdin and FILE, report MATCH/MISMATCH;
 *                            exit 0 iff identical -- the file/stdin parity
 *                            check the SH7 receipts will lean on
 *
 * Exit status is 0 on success / match, 1 on any failure (bad file, mismatch,
 * or a failed selftest vector) so a build script can branch on `$?`.
 */
#include <stdio.h>
#include <string.h>
#include "atls/atls.h"

static const char HEXD[] = "0123456789abcdef";

/* Render a 32-byte digest as 64 lowercase hex chars into out (>= 65 bytes). */
void sha256_to_hex(const uint8_t d[32], char out[65]) {
    int i;
    for (i = 0; i < 32; i++) {
        out[i * 2]     = HEXD[(d[i] >> 4) & 0xf];
        out[i * 2 + 1] = HEXD[d[i] & 0xf];
    }
    out[64] = '\0';
}

/* Stream fp through SHA-256 in 4 KiB blocks; write the digest to out. */
static int hash_stream(FILE *fp, uint8_t out[32]) {
    atls_sha256_ctx ctx;
    unsigned char buf[4096];
    size_t n;
    atls_sha256_init(&ctx);
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0)
        atls_sha256_update(&ctx, buf, n);
    if (ferror(fp))
        return -1;
    atls_sha256_final(&ctx, out);
    return 0;
}

static int hash_file(const char *path, uint8_t out[32]) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "sha256sum: %s: cannot open\n", path);
        return -1;
    }
    int rc = hash_stream(fp, out);
    fclose(fp);
    if (rc != 0)
        fprintf(stderr, "sha256sum: %s: read error\n", path);
    return rc;
}

/* FIPS 180-4 / RFC 6234 published vectors, so "it is self-consistent" can
 * never pass: the empty string, "abc", and the 448-char block of 'a'. */
static int selftest(void) {
    static const char *vec_hex[] = {
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
    };
    atls_sha256_ctx ctx;
    uint8_t d[32];
    char hex[65];
    int i, fails = 0;

    /* vector 0: empty */
    atls_sha256("", 0, d);
    sha256_to_hex(d, hex);
    if (strcmp(hex, vec_hex[0]) != 0) { fails++; printf("FAIL: empty vector\n"); }

    /* vector 1: "abc" */
    atls_sha256("abc", 3, d);
    sha256_to_hex(d, hex);
    if (strcmp(hex, vec_hex[1]) != 0) { fails++; printf("FAIL: abc vector\n"); }

    /* vector 2: one million 'a' -- exercises the multi-block + padding path */
    atls_sha256_init(&ctx);
    {
        char a[1000];
        memset(a, 'a', sizeof a);
        for (i = 0; i < 1000; i++)
            atls_sha256_update(&ctx, a, sizeof a);
    }
    atls_sha256_final(&ctx, d);
    sha256_to_hex(d, hex);
    if (strcmp(hex, vec_hex[2]) != 0) { fails++; printf("FAIL: 1M 'a' vector\n"); }

    if (fails) {
        printf("[selfhost] sha256 SELFTEST FAILED (%d vector(s))\n", fails);
        return 1;
    }
    printf("[selfhost] sha256 SELFTEST OK (3 vectors)\n");
    return 0;
}

/* Hash stdin and the named file; they must match (same content, two paths). */
static int eq_mode(const char *path) {
    uint8_t a[32], b[32];
    char ha[65], hb[65];
    if (hash_file(path, a) != 0)
        return 1;
    if (hash_stream(stdin, b) != 0) {
        fprintf(stderr, "sha256sum: stdin: read error\n");
        return 1;
    }
    sha256_to_hex(a, ha);
    sha256_to_hex(b, hb);
    if (strcmp(ha, hb) != 0) {
        printf("[selfhost] sha256 MISMATCH: %s != stdin\n", path);
        return 1;
    }
    printf("[selfhost] sha256 MATCH: stdin == %s (%s)\n", path, ha);
    return 0;
}

#ifndef SHA256SUM_NO_MAIN
int main(int argc, char **argv) {
    uint8_t d[32];
    char hex[65];
    int i, rc = 0;

    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0)
        return selftest();

    if (argc >= 3 && strcmp(argv[1], "--eq") == 0)
        return eq_mode(argv[2]);

    if (argc < 2) {
        /* no file: stdin -> "<hex>  -" */
        if (hash_stream(stdin, d) != 0)
            return 1;
        sha256_to_hex(d, hex);
        printf("%s  -\n", hex);
        return 0;
    }

    for (i = 1; i < argc; i++) {
        if (hash_file(argv[i], d) != 0) { rc = 1; continue; }
        sha256_to_hex(d, hex);
        printf("%s  %s\n", hex, argv[i]);
    }
    return rc;
}
#endif

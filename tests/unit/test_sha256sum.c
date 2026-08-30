/* tests/unit/test_sha256sum.c -- SELFHOST_PLAN.md SH7a host gate.
 *
 * Links the REAL tool source (tools/selfhost/sha256sum.c, main() compiled
 * out via SHA256SUM_NO_MAIN) and the REAL libatls SHA-256 it calls -- the
 * tree's "never test a copy" rule.  Vectors are the published FIPS 180-4 /
 * RFC 6234 ones: "self-consistent output" proves nothing.  The same tool
 * binary ships in the initrd and is re-exercised in-guest by
 * tests/integration/cases/test_selfhost_sha256sum.sh; this host test pins
 * the logic at dev speed without a VM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHA256SUM_NO_MAIN 1
#include "tools/selfhost/sha256sum.c"

static int pass_count = 0, fail_count = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (cond) { pass_count++; printf("PASS: %s\n", msg); }  \
        else { fail_count++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } \
    } while (0)

static int hex_eq(const char *got, const char *want) {
    return strcmp(got, want) == 0;
}

int main(void) {
    uint8_t d[32];
    char hex[65];

    /* empty string */
    atls_sha256("", 0, d);
    sha256_to_hex(d, hex);
    CHECK(hex_eq(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "SHA-256(\"\") is the FIPS empty vector");

    /* "abc" */
    atls_sha256("abc", 3, d);
    sha256_to_hex(d, hex);
    CHECK(hex_eq(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "SHA-256(\"abc\") is the FIPS abc vector");

    /* streaming the million-'a' vector through the 4 KiB block loop */
    {
        atls_sha256_ctx ctx;
        char a[1000];
        int i;
        memset(a, 'a', sizeof a);
        atls_sha256_init(&ctx);
        for (i = 0; i < 1000; i++)
            atls_sha256_update(&ctx, a, sizeof a);
        atls_sha256_final(&ctx, d);
        sha256_to_hex(d, hex);
        CHECK(hex_eq(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
              "streamed SHA-256(1M 'a') matches the FIPS multi-block vector");
    }

    /* the tool's own --selftest (all three vectors) returns success */
    CHECK(selftest() == 0, "tool selftest() passes its 3 vectors");

    /* --eq parity mode: equal content -> 0 (match), different -> 1.
     * eq_mode() hashes stdin and the named file; drive stdin via a pipe. */
    {
        const char *eqpath = "/tmp/aura_sha256_eq.bin";
        FILE *fp = fopen(eqpath, "wb");
        const char *payload = "parity-bytes-for-eq-mode\n";
        int rc_same = -1, rc_diff = -1;
        if (fp) { fputs(payload, fp); fclose(fp); }

        /* helper macro-free: redirect stdin from a temp file holding payload */
        {
            FILE *in = fopen("/tmp/aura_sha256_stdin.bin", "wb");
            if (in) { fputs(payload, in); fclose(in); }
        }
        freopen("/tmp/aura_sha256_stdin.bin", "rb", stdin);
        if (fp) rc_same = eq_mode(eqpath);

        freopen("/dev/null", "rb", stdin);   /* empty stdin -> different bytes */
        if (fp) rc_diff = eq_mode(eqpath);

        CHECK(rc_same == 0, "eq_mode: identical stdin/file content reports MATCH (exit 0)");
        CHECK(rc_diff == 1, "eq_mode: differing content reports MISMATCH (exit 1)");
        remove(eqpath);
        remove("/tmp/aura_sha256_stdin.bin");
    }

    /* hex rendering is lowercase and 64 chars + NUL */
    CHECK(strlen(hex) == 64, "digest renders as 64 hex chars");
    {
        int all_lower = 1, k;
        for (k = 0; k < 64; k++)
            if (!((hex[k] >= '0' && hex[k] <= '9') ||
                  (hex[k] >= 'a' && hex[k] <= 'f')))
                all_lower = 0;
        CHECK(all_lower, "digest is lowercase hex (coreutils shape)");
    }

    /* hash_stream over a real file: matches one-shot atls_sha256. */
    {
        const char *path = "/tmp/aura_sha256_test.bin";
        FILE *fp;
        unsigned char buf[5000];
        size_t k;
        for (k = 0; k < sizeof buf; k++) buf[k] = (unsigned char)(k * 31 + 7);
        fp = fopen(path, "wb");
        if (fp) {
            fwrite(buf, 1, sizeof buf, fp);
            fclose(fp);
            fp = fopen(path, "rb");
            CHECK(fp != 0 && hash_stream(fp, d) == 0, "hash_stream reads a multi-block file");
            if (fp) fclose(fp);
            {
                uint8_t ref[32];
                atls_sha256(buf, sizeof buf, ref);
                CHECK(memcmp(d, ref, 32) == 0,
                      "file hash equals the one-shot hash of the same bytes");
            }
        } else {
            CHECK(0, "could not create scratch file");
        }
        remove(path);
    }

    if (fail_count) {
        printf("\n%d passed, %d FAILED\n", pass_count, fail_count);
        return 1;
    }
    printf("\nall %d SH7a sha256sum checks passed\n", pass_count);
    return 0;
}

/* tests/unit/test_ota_manifest.c — OTA_PLAN O4 host gate.
 *
 * Two things are under test, in the no-second-copy discipline of
 * test_sha256sum: the manifest parser (userspace/apps/ota/ota_manifest.c,
 * the same translation unit the guest `ota` app links) and the digest
 * hex helpers, against the REAL libatls SHA-256 with published NIST
 * FIPS 180-4 vectors — including a chunked-streaming case that mirrors
 * exactly how `ota apply` feeds the hasher (4 KiB writes).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ota_manifest.h"
#include "atls/atls.h"

static int failures = 0;

#define CHECK(cond, name) do {                                   \
    if (cond) { printf("PASS: %s\n", name); }                    \
    else      { printf("FAIL: %s\n", name); failures++; }        \
} while (0)

/* ---- NIST FIPS 180-4 SHA-256 test vectors ---- */

static void nist_vectors(void) {
    unsigned char d[32];
    char hex[OTA_SHA256_HEX];

    atls_sha256("abc", 3, d);
    ota_digest_hex(d, hex);
    CHECK(strcmp(hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
        "NIST: sha256(\"abc\")");

    atls_sha256("", 0, d);
    ota_digest_hex(d, hex);
    CHECK(strcmp(hex,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
        "NIST: sha256(\"\")");

    const char *two_block =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    atls_sha256(two_block, strlen(two_block), d);
    ota_digest_hex(d, hex);
    CHECK(strcmp(hex,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0,
        "NIST: sha256(two-block message)");

    /* Streaming equivalence, the `ota apply` shape: the million-'a'
     * vector fed through init/update/final in 4 KiB chunks must equal
     * the published digest. */
    static char million[1000000];
    memset(million, 'a', sizeof(million));
    atls_sha256_ctx c;
    atls_sha256_init(&c);
    size_t off = 0;
    while (off < sizeof(million)) {
        size_t n = sizeof(million) - off;
        if (n > 4096) n = 4096;
        atls_sha256_update(&c, million + off, n);
        off += n;
    }
    atls_sha256_final(&c, d);
    ota_digest_hex(d, hex);
    CHECK(strcmp(hex,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0,
        "NIST: sha256(million 'a') streamed in 4 KiB chunks");
}

/* ---- hex helpers ---- */

static void hex_helpers(void) {
    unsigned char d[32];
    for (int i = 0; i < 32; i++) d[i] = (unsigned char)i;
    char hex[OTA_SHA256_HEX];
    ota_digest_hex(d, hex);
    CHECK(strcmp(hex,
        "000102030405060708090a0b0c0d0e0f"
        "101112131415161718191a1b1c1d1e1f") == 0,
        "ota_digest_hex layout");

    CHECK(ota_hex_digest_ok(d, hex) == 1, "hex digest round-trip");
    char upper[OTA_SHA256_HEX];
    for (int i = 0; i < 64; i++)
        upper[i] = (hex[i] >= 'a' && hex[i] <= 'f')
                       ? (char)(hex[i] - 'a' + 'A') : hex[i];
    upper[64] = '\0';
    CHECK(ota_hex_digest_ok(d, upper) == 1, "hex compare is case-insensitive");

    char bad[OTA_SHA256_HEX];
    strcpy(bad, hex);
    bad[0] = (bad[0] == '0') ? '1' : '0';
    CHECK(ota_hex_digest_ok(d, bad) == 0, "hex compare catches a flipped nibble");
    bad[0] = 'g';
    CHECK(ota_hex_digest_ok(d, bad) == 0, "hex compare rejects non-hex");
}

/* ---- manifest parser ---- */

static const char *M_GOOD =
    "version=0.0.2-ota\n"
    "url=http://10.0.2.2:18081/kernel-0.0.2.elf\n"
    "size=1431224\n"
    "sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n";

static void parser(void) {
    ota_manifest m;

    int rc = ota_manifest_parse(M_GOOD, strlen(M_GOOD), &m);
    CHECK(rc == OTA_MANIFEST_OK, "valid manifest parses");
    CHECK(rc == OTA_MANIFEST_OK && strcmp(m.version, "0.0.2-ota") == 0,
          "version field");
    CHECK(rc == OTA_MANIFEST_OK &&
          strcmp(m.url, "http://10.0.2.2:18081/kernel-0.0.2.elf") == 0,
          "url field");
    CHECK(rc == OTA_MANIFEST_OK && m.size == 1431224, "size field");
    CHECK(rc == OTA_MANIFEST_OK &&
          strcmp(m.sha256_hex,
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "sha256 field");

    /* CRLF, blank lines, unknown keys, trailing spaces: all tolerated. */
    const char *crlf =
        "\r\n"
        "# comment-ish line without equals is ignored\r\n"
        "version=0.0.3 \r\n"
        "extra_key=whatever\n"
        "\n"
        "url=http://h/p\r\n"
        "size=7\n"
        "sha256=0000000000000000000000000000000000000000000000000000000000000000\r\n";
    rc = ota_manifest_parse(crlf, strlen(crlf), &m);
    CHECK(rc == OTA_MANIFEST_OK, "CRLF + blanks + unknown keys parse");
    CHECK(rc == OTA_MANIFEST_OK && strcmp(m.version, "0.0.3") == 0,
          "trailing whitespace is trimmed");

    /* Each missing required key is its own named error. */
#define PARSE(lit) rc = ota_manifest_parse((lit), strlen((lit)), &m)
    PARSE("url=u\nsize=1\nsha256="
          "0000000000000000000000000000000000000000000000000000000000000000\n");
    CHECK(rc == OTA_MANIFEST_EVERSION, "missing version= -> EVERSION");

    PARSE("version=v\nsize=1\nsha256="
          "0000000000000000000000000000000000000000000000000000000000000000\n");
    CHECK(rc == OTA_MANIFEST_EURL, "missing url= -> EURL");

    PARSE("version=v\nurl=u\nsha256="
          "0000000000000000000000000000000000000000000000000000000000000000\n");
    CHECK(rc == OTA_MANIFEST_ESIZE, "missing size= -> ESIZE");

    PARSE("version=v\nurl=u\nsize=1\n");
    CHECK(rc == OTA_MANIFEST_ESHA, "missing sha256= -> ESHA");

    PARSE("version=v\nurl=u\nsize=12x4\nsha256="
          "0000000000000000000000000000000000000000000000000000000000000000\n");
    CHECK(rc == OTA_MANIFEST_ESIZEFMT, "size=12x4 -> ESIZEFMT");

    PARSE("version=v\nurl=u\nsize=1\nsha256=short\n");
    CHECK(rc == OTA_MANIFEST_ESHAFMT, "sha256=short -> ESHAFMT");

    PARSE("version=v\nurl=u\nsize=1\nsha256="
          "zz7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n");
    CHECK(rc == OTA_MANIFEST_ESHAFMT, "sha256 with non-hex chars -> ESHAFMT");

    PARSE("version=\nurl=u\nsize=1\nsha256="
          "0000000000000000000000000000000000000000000000000000000000000000\n");
    CHECK(rc == OTA_MANIFEST_EVERSION, "empty version= -> EVERSION");
#undef PARSE

    /* Overlong values are refused, not truncated. */
    char big[1024];
    strcpy(big, "version=");
    memset(big + 8, 'v', 600);
    strcpy(big + 8 + 600, "\nurl=u\nsize=1\nsha256="
          "0000000000000000000000000000000000000000000000000000000000000000\n");
    rc = ota_manifest_parse(big, strlen(big), &m);
    CHECK(rc == OTA_MANIFEST_ETOOLONG, "overlong version -> ETOOLONG");

    strcpy(big, "version=v\nurl=");
    memset(big + 13, 'u', 600);
    strcpy(big + 13 + 600, "\nsize=1\nsha256="
          "0000000000000000000000000000000000000000000000000000000000000000\n");
    rc = ota_manifest_parse(big, strlen(big), &m);
    CHECK(rc == OTA_MANIFEST_EURL, "overlong url -> EURL");

    CHECK(ota_manifest_parse(NULL, 0, &m) == OTA_MANIFEST_EARG,
          "NULL text -> EARG");
}

int main(void) {
    nist_vectors();
    hex_helpers();
    parser();
    if (failures == 0) {
        printf("test_ota_manifest: ALL PASS\n");
        return 0;
    }
    printf("test_ota_manifest: %d FAILURE(S)\n", failures);
    return 1;
}

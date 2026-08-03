/*
 * test_apkg.c — host-side unit tests for the .apkg package parser
 * (SDK_PLAN phase S4).
 *
 * WHY THIS TEST EXISTS
 *
 * apkg_parse() reads a file the user was told to obtain from somewhere else.
 * Every byte of it is attacker-controlled, and the interesting inputs are all
 * malformed: a truncated header, a size that disagrees with the file, an
 * integer that overflows, a line with no newline at the end. A parser tested
 * only on well-formed input is a parser tested on the one case that never
 * causes trouble.
 *
 * The shipping source is compiled in directly, so this cannot drift from what
 * `apm` and `mkapkg` actually run.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lib/libc/include/apkg.h"

static int tn = 0, passed = 0, failed = 0;

#define RUN(fn) do {                                    \
    tn++;                                               \
    if (fn()) { passed++; }                             \
    else { failed++; printf("  FAIL: %s\n", #fn); }     \
} while (0)

#define CHECK(c) do {                                                   \
    if (!(c)) { printf("    L%d: %s\n", __LINE__, #c); return 0; }      \
} while (0)

/* Build a package in a static buffer: header text, then payload. */
static unsigned char pkgbuf[8192];

static size_t build(const char *header, const void *payload, size_t plen) {
    size_t hlen = strlen(header);
    memcpy(pkgbuf, header, hlen);
    if (plen) memcpy(pkgbuf + hlen, payload, plen);
    return hlen + plen;
}

/* A well-formed package around the given payload. */
static size_t build_valid(const char *payload) {
    size_t plen = strlen(payload);
    uint32_t crc = apkg_crc32(payload, plen);
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "AURAPKG1\n"
             "name: testpkg\n"
             "version: 1.2.3\n"
             "description: a test package\n"
             "size: %zu\n"
             "crc32: 0x%08x\n"
             "\n", plen, crc);
    return build(hdr, payload, plen);
}

/* ---- well-formed -------------------------------------------------------- */

static int t_parses_valid(void) {
    size_t n = build_valid("PAYLOAD");
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_OK);
    CHECK(strcmp(h.name, "testpkg") == 0);
    CHECK(strcmp(h.version, "1.2.3") == 0);
    CHECK(strcmp(h.desc, "a test package") == 0);
    CHECK(h.size == 7);
    return 1;
}

static int t_verifies_valid(void) {
    size_t n = build_valid("PAYLOAD");
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_OK);
    CHECK(apkg_verify(pkgbuf, n, &h) == APKG_OK);
    return 1;
}

/* Round-trip: the payload read back is byte-identical. */
static int t_round_trip(void) {
    const char *payload = "\x7f" "ELF and some bytes";
    size_t plen = strlen(payload);
    size_t n = build_valid(payload);
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_OK);
    CHECK(h.size == plen);
    CHECK(memcmp(pkgbuf + h.payload_off, payload, plen) == 0);
    return 1;
}

/* description is optional; name/version/size/crc32 are not. */
static int t_description_optional(void) {
    size_t n = build("AURAPKG1\nname: x\nversion: 1\nsize: 3\ncrc32: 0x884863d2\n\n",
                     "abc", 3);
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_OK);
    CHECK(h.desc[0] == '\0');
    return 1;
}

static int t_empty_payload(void) {
    size_t n = build_valid("");
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_OK);
    CHECK(h.size == 0);
    CHECK(apkg_verify(pkgbuf, n, &h) == APKG_OK);
    return 1;
}

/* ---- not a package ------------------------------------------------------ */

static int t_rejects_wrong_magic(void) {
    size_t n = build("NOTAPKG1\nname: x\n\n", "y", 1);
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_MAGIC);
    return 1;
}

/* A bare ELF — which is exactly what the old .pkg files were. */
static int t_rejects_raw_elf(void) {
    const char elf[] = "\x7f" "ELF\x02\x01\x01";
    memcpy(pkgbuf, elf, sizeof(elf));
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, sizeof(elf), &h) == APKG_ERR_MAGIC);
    return 1;
}

static int t_rejects_empty_and_tiny(void) {
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, 0, &h) == APKG_ERR_MAGIC);
    CHECK(apkg_parse("AURAPKG", 7, &h) == APKG_ERR_MAGIC);
    /* magic present but no newline after it */
    CHECK(apkg_parse("AURAPKG1x", 9, &h) == APKG_ERR_MAGIC);
    return 1;
}

static int t_rejects_null(void) {
    struct apkg_header h;
    CHECK(apkg_parse(NULL, 10, &h) == APKG_ERR_MAGIC);
    CHECK(apkg_parse(pkgbuf, 10, NULL) == APKG_ERR_MAGIC);
    return 1;
}

/* ---- truncation --------------------------------------------------------- */

/* A header that never reaches its blank terminator line. */
static int t_rejects_unterminated_header(void) {
    size_t n = build("AURAPKG1\nname: x\nversion: 1\n", "", 0);
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_TRUNCATED);
    return 1;
}

/* A field line with no newline at the end of the file: the scan must stop. */
static int t_rejects_line_without_newline(void) {
    size_t n = build("AURAPKG1\nname: x", "", 0);
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_TRUNCATED);
    return 1;
}

/* THE case this format exists to catch: the transfer stopped early. */
static int t_rejects_short_payload(void) {
    size_t n = build_valid("PAYLOAD");
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_OK);      /* whole file: fine */
    CHECK(apkg_parse(pkgbuf, n - 3, &h) == APKG_ERR_SIZE);  /* cut short */
    return 1;
}

/* A header longer than the cap must be refused rather than scanned forever. */
static int t_rejects_oversized_header(void) {
    char big[APKG_HEADER_MAX + 512];
    int p = snprintf(big, sizeof(big), "AURAPKG1\n");
    while (p < APKG_HEADER_MAX + 100) {
        p += snprintf(big + p, sizeof(big) - (size_t)p, "name: x\n");
    }
    struct apkg_header h;
    CHECK(apkg_parse(big, (size_t)p, &h) != APKG_OK);
    return 1;
}

/* ---- malformed fields --------------------------------------------------- */

static int t_rejects_missing_fields(void) {
    struct apkg_header h;
    size_t n;

    n = build("AURAPKG1\nversion: 1\nsize: 1\ncrc32: 0x0\n\n", "a", 1);
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_MISSING);   /* no name */

    n = build("AURAPKG1\nname: x\nsize: 1\ncrc32: 0x0\n\n", "a", 1);
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_MISSING);   /* no version */

    n = build("AURAPKG1\nname: x\nversion: 1\ncrc32: 0x0\n\n", "a", 1);
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_MISSING);   /* no size */

    n = build("AURAPKG1\nname: x\nversion: 1\nsize: 1\n\n", "a", 1);
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_MISSING);   /* no crc32 */
    return 1;
}

static int t_rejects_no_separator(void) {
    size_t n = build("AURAPKG1\nname x\n\n", "a", 1);
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_FIELD);
    return 1;
}

/* An unknown field is refused, not ignored: a format that silently accepts
 * what it does not understand cannot be extended unambiguously later. */
static int t_rejects_unknown_field(void) {
    size_t n = build("AURAPKG1\nname: x\nversion: 1\nsize: 1\ncrc32: 0x0\n"
                     "surprise: yes\n\n", "a", 1);
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_FIELD);
    return 1;
}

static int t_rejects_empty_name(void) {
    size_t n = build("AURAPKG1\nname: \nversion: 1\nsize: 1\ncrc32: 0x0\n\n", "a", 1);
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_FIELD);
    return 1;
}

/* A name longer than the field must fail, not truncate: a silently shortened
 * name is a different package from the one on disk. */
static int t_rejects_overlong_name(void) {
    char hdr[512];
    char longname[APKG_NAME_MAX + 16];
    memset(longname, 'a', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    snprintf(hdr, sizeof(hdr),
             "AURAPKG1\nname: %s\nversion: 1\nsize: 1\ncrc32: 0x0\n\n", longname);
    size_t n = build(hdr, "a", 1);
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_FIELD);
    return 1;
}

static int t_rejects_non_numeric_size(void) {
    struct apkg_header h;
    size_t n;

    n = build("AURAPKG1\nname: x\nversion: 1\nsize: abc\ncrc32: 0x0\n\n", "a", 1);
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_FIELD);

    /* trailing rubbish after a valid number */
    n = build("AURAPKG1\nname: x\nversion: 1\nsize: 12x\ncrc32: 0x0\n\n", "a", 1);
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_FIELD);

    /* empty value */
    n = build("AURAPKG1\nname: x\nversion: 1\nsize: \ncrc32: 0x0\n\n", "a", 1);
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_FIELD);
    return 1;
}

/* An integer that would wrap must be refused, not silently reduced. */
static int t_rejects_overflowing_size(void) {
    size_t n = build("AURAPKG1\nname: x\nversion: 1\n"
                     "size: 99999999999999999999999\ncrc32: 0x0\n\n", "a", 1);
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_FIELD);
    return 1;
}

/* A size beyond what the kernel will ever load is refused up front: a package
 * that installs and then cannot run is a worse outcome than a refusal. */
static int t_rejects_absurd_size(void) {
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "AURAPKG1\nname: x\nversion: 1\nsize: %d\ncrc32: 0x0\n\n",
             APKG_PAYLOAD_MAX + 1);
    size_t n = build(hdr, "a", 1);
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_SIZE);
    return 1;
}

static int t_rejects_crc_too_large(void) {
    size_t n = build("AURAPKG1\nname: x\nversion: 1\nsize: 1\n"
                     "crc32: 0x1FFFFFFFF\n\n", "a", 1);
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_ERR_FIELD);
    return 1;
}

/* ---- checksum ----------------------------------------------------------- */

static int t_detects_corrupt_payload(void) {
    size_t n = build_valid("PAYLOAD");
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_OK);
    pkgbuf[h.payload_off] ^= 0xFF;                 /* flip one byte */
    CHECK(apkg_verify(pkgbuf, n, &h) == APKG_ERR_CRC);
    return 1;
}

/* A single flipped bit must be caught, which is the point of a CRC. */
static int t_detects_single_bit_flip(void) {
    size_t n = build_valid("a longer payload so the crc has something to chew");
    struct apkg_header h;
    CHECK(apkg_parse(pkgbuf, n, &h) == APKG_OK);
    pkgbuf[h.payload_off + 5] ^= 0x01;
    CHECK(apkg_verify(pkgbuf, n, &h) == APKG_ERR_CRC);
    return 1;
}

static int t_crc_is_stable(void) {
    /* The zlib/IEEE CRC-32 of "123456789" is 0xCBF43926 — a standard check
     * value, so this catches a wrong polynomial or a reversed algorithm
     * rather than merely an inconsistent one. */
    CHECK(apkg_crc32("123456789", 9) == 0xCBF43926u);
    CHECK(apkg_crc32("", 0) == 0u);
    return 1;
}

/* ---- diagnostics -------------------------------------------------------- */

static int t_strerror_covers_every_code(void) {
    int codes[] = { APKG_OK, APKG_ERR_MAGIC, APKG_ERR_TRUNCATED,
                    APKG_ERR_FIELD, APKG_ERR_MISSING, APKG_ERR_SIZE,
                    APKG_ERR_CRC };
    for (unsigned i = 0; i < sizeof(codes)/sizeof(codes[0]); i++) {
        const char *m = apkg_strerror(codes[i]);
        CHECK(m != NULL);
        CHECK(m[0] != '\0');
        CHECK(strcmp(m, "unknown error") != 0);
    }
    CHECK(strcmp(apkg_strerror(9999), "unknown error") == 0);
    return 1;
}

int main(void) {
    printf("test_apkg: .apkg package format\n");

    RUN(t_parses_valid);
    RUN(t_verifies_valid);
    RUN(t_round_trip);
    RUN(t_description_optional);
    RUN(t_empty_payload);

    RUN(t_rejects_wrong_magic);
    RUN(t_rejects_raw_elf);
    RUN(t_rejects_empty_and_tiny);
    RUN(t_rejects_null);

    RUN(t_rejects_unterminated_header);
    RUN(t_rejects_line_without_newline);
    RUN(t_rejects_short_payload);
    RUN(t_rejects_oversized_header);

    RUN(t_rejects_missing_fields);
    RUN(t_rejects_no_separator);
    RUN(t_rejects_unknown_field);
    RUN(t_rejects_empty_name);
    RUN(t_rejects_overlong_name);
    RUN(t_rejects_non_numeric_size);
    RUN(t_rejects_overflowing_size);
    RUN(t_rejects_absurd_size);
    RUN(t_rejects_crc_too_large);

    RUN(t_detects_corrupt_payload);
    RUN(t_detects_single_bit_flip);
    RUN(t_crc_is_stable);

    RUN(t_strerror_covers_every_code);

    printf("  %d/%d passed, %d failed\n", passed, tn, failed);
    return failed == 0 ? 0 : 1;
}

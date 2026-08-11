/*
 * test_w32_utf.c — host unit tests for WIN32_PLAN.md phase W32-1.
 *
 * The gate from the plan: "round-trip ASCII, BMP, astral (surrogate pairs),
 * and malformed input (lone surrogate, truncated sequence, over-long UTF-8) —
 * refused, not crashed, no over-read past the buffer."
 *
 * Malformed cases come first, deliberately: this converter's job is to refuse,
 * and a converter that only handles good input is the bug being guarded
 * against.  Every rejection case is also run against a *non*-NUL-terminated
 * buffer sized exactly to its content, so an over-read walks off the end of a
 * heap block rather than finding a convenient zero byte.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "w32/w32_utf.h"
#include "../../w32/src/w32_utf.c"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; \
                    else printf("  [%s] FAILED\n", #f); } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long _a = (long)(a), _e = (long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=%ld want %ld\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

/* Convert with the source in its own exact-sized heap block, so any read past
 * the declared length is a genuine out-of-bounds access that ASan can see. */
static int conv8(const uint8_t *bytes, size_t n, uint16_t *dst, size_t cap,
                 size_t *need) {
    uint8_t *tight = malloc(n ? n : 1);
    memcpy(tight, bytes, n);
    int rc = w32_utf8_to_utf16((const char *)tight, n, dst, cap, need);
    free(tight);
    return rc;
}

static int conv16(const uint16_t *units, size_t n, char *dst, size_t cap,
                  size_t *need) {
    uint16_t *tight = malloc((n ? n : 1) * sizeof(uint16_t));
    memcpy(tight, units, n * sizeof(uint16_t));
    int rc = w32_utf16_to_utf8(tight, n, dst, cap, need);
    free(tight);
    return rc;
}

/* ---- malformed UTF-8, all must be refused -------------------------------- */

static void test_utf8_overlong(void) {
    uint16_t out[8]; size_t need;
    /* C0 80 is an overlong encoding of U+0000 — the classic filter bypass. */
    const uint8_t a[] = { 0xC0, 0x80 };
    CHECK_EQ(conv8(a, 2, out, 8, &need), W32_UTF_ERR_ENCODING);
    /* E0 80 80 overlong for U+0000, and C1 BF overlong for U+007F. */
    const uint8_t b[] = { 0xE0, 0x80, 0x80 };
    CHECK_EQ(conv8(b, 3, out, 8, &need), W32_UTF_ERR_ENCODING);
    const uint8_t c[] = { 0xC1, 0xBF };
    CHECK_EQ(conv8(c, 2, out, 8, &need), W32_UTF_ERR_ENCODING);
    /* F0 80 80 80 overlong for U+0000. */
    const uint8_t d[] = { 0xF0, 0x80, 0x80, 0x80 };
    CHECK_EQ(conv8(d, 4, out, 8, &need), W32_UTF_ERR_ENCODING);
}

static void test_utf8_truncated(void) {
    uint16_t out[8]; size_t need;
    const uint8_t a[] = { 0xE2, 0x82 };            /* 3-byte seq, 2 present */
    CHECK_EQ(conv8(a, 2, out, 8, &need), W32_UTF_ERR_TRUNCATED);
    const uint8_t b[] = { 0xF0, 0x9F };            /* 4-byte seq, 2 present */
    CHECK_EQ(conv8(b, 2, out, 8, &need), W32_UTF_ERR_TRUNCATED);
    const uint8_t c[] = { 0xC2 };                  /* 2-byte seq, 1 present */
    CHECK_EQ(conv8(c, 1, out, 8, &need), W32_UTF_ERR_TRUNCATED);
}

static void test_utf8_bad_continuation(void) {
    uint16_t out[8]; size_t need;
    const uint8_t a[] = { 0xE2, 0x28, 0xA1 };      /* 0x28 is not 10xxxxxx */
    CHECK_EQ(conv8(a, 3, out, 8, &need), W32_UTF_ERR_ENCODING);
    const uint8_t b[] = { 0x80 };                  /* stray continuation */
    CHECK_EQ(conv8(b, 1, out, 8, &need), W32_UTF_ERR_ENCODING);
    const uint8_t c[] = { 0xF8, 0x88, 0x80, 0x80, 0x80 };  /* 5-byte: invalid */
    CHECK_EQ(conv8(c, 5, out, 8, &need), W32_UTF_ERR_ENCODING);
}

static void test_utf8_surrogate_and_range(void) {
    uint16_t out[8]; size_t need;
    /* ED A0 80 = U+D800 encoded in UTF-8: a surrogate, never valid. */
    const uint8_t a[] = { 0xED, 0xA0, 0x80 };
    CHECK_EQ(conv8(a, 3, out, 8, &need), W32_UTF_ERR_ENCODING);
    /* F4 90 80 80 = U+110000, one past the maximum. */
    const uint8_t b[] = { 0xF4, 0x90, 0x80, 0x80 };
    CHECK_EQ(conv8(b, 4, out, 8, &need), W32_UTF_ERR_ENCODING);
}

/* ---- malformed UTF-16 ---------------------------------------------------- */

static void test_utf16_lone_surrogate(void) {
    char out[16]; size_t need;
    const uint16_t hi[] = { 0xD800 };              /* high, nothing follows */
    CHECK_EQ(conv16(hi, 1, out, sizeof out, &need), W32_UTF_ERR_TRUNCATED);
    const uint16_t lo[] = { 0xDC00 };              /* low without a high */
    CHECK_EQ(conv16(lo, 1, out, sizeof out, &need), W32_UTF_ERR_ENCODING);
    const uint16_t hh[] = { 0xD800, 0xD800 };      /* high followed by high */
    CHECK_EQ(conv16(hh, 2, out, sizeof out, &need), W32_UTF_ERR_ENCODING);
    const uint16_t ha[] = { 0xD800, 0x0041 };      /* high followed by 'A' */
    CHECK_EQ(conv16(ha, 2, out, sizeof out, &need), W32_UTF_ERR_ENCODING);
}

/* ---- well-formed: round trips -------------------------------------------- */

static void roundtrip(const uint16_t *u16, size_t n16,
                      const uint8_t *u8, size_t n8) {
    char b8[256]; size_t need8;
    CHECK_EQ(w32_utf16_to_utf8(u16, n16, b8, sizeof b8, &need8), W32_UTF_OK);
    CHECK_EQ(need8, n8);
    CHECK(memcmp(b8, u8, n8) == 0);

    uint16_t b16[256]; size_t need16;
    CHECK_EQ(w32_utf8_to_utf16((const char *)u8, n8, b16, 256, &need16), W32_UTF_OK);
    CHECK_EQ(need16, n16);
    CHECK(memcmp(b16, u16, n16 * sizeof(uint16_t)) == 0);
}

static void test_ascii(void) {
    const uint16_t u16[] = { 'H','e','l','l','o' };
    const uint8_t  u8[]  = { 'H','e','l','l','o' };
    roundtrip(u16, 5, u8, 5);
}

static void test_bmp_two_and_three_byte(void) {
    /* U+00E9 e-acute -> C3 A9;  U+20AC euro -> E2 82 AC */
    const uint16_t u16[] = { 0x00E9, 0x20AC };
    const uint8_t  u8[]  = { 0xC3, 0xA9, 0xE2, 0x82, 0xAC };
    roundtrip(u16, 2, u8, 5);
}

static void test_astral_surrogate_pair(void) {
    /* U+1F600 GRINNING FACE = D83D DE00 = F0 9F 98 80 */
    const uint16_t u16[] = { 0xD83D, 0xDE00 };
    const uint8_t  u8[]  = { 0xF0, 0x9F, 0x98, 0x80 };
    roundtrip(u16, 2, u8, 4);

    /* Boundaries of the astral range: U+10000 and U+10FFFF. */
    const uint16_t lo16[] = { 0xD800, 0xDC00 };
    const uint8_t  lo8[]  = { 0xF0, 0x90, 0x80, 0x80 };
    roundtrip(lo16, 2, lo8, 4);

    const uint16_t hi16[] = { 0xDBFF, 0xDFFF };
    const uint8_t  hi8[]  = { 0xF4, 0x8F, 0xBF, 0xBF };
    roundtrip(hi16, 2, hi8, 4);
}

static void test_embedded_nul(void) {
    /* An embedded NUL is data, not a terminator: it must survive. */
    const uint16_t u16[] = { 'a', 0x0000, 'b' };
    const uint8_t  u8[]  = { 'a', 0x00, 'b' };
    roundtrip(u16, 3, u8, 3);
}

/* ---- sizing and capacity ------------------------------------------------- */

static void test_measure_then_convert(void) {
    const uint16_t u16[] = { 0x20AC, 0x20AC };     /* 3 bytes each */
    size_t need = 0;
    /* Measuring pass: NULL dst with 0 capacity is success, not an error. */
    CHECK_EQ(w32_utf16_to_utf8(u16, 2, NULL, 0, &need), W32_UTF_OK);
    CHECK_EQ(need, 6);

    char buf[6]; size_t need2 = 0;
    CHECK_EQ(w32_utf16_to_utf8(u16, 2, buf, sizeof buf, &need2), W32_UTF_OK);
    CHECK_EQ(need2, 6);
}

static void test_too_small_reports_need(void) {
    const uint16_t u16[] = { 0x20AC, 0x20AC };
    char buf[5]; size_t need = 0;
    CHECK_EQ(w32_utf16_to_utf8(u16, 2, buf, sizeof buf, &need), W32_UTF_ERR_SPACE);
    CHECK_EQ(need, 6);          /* still tells the caller what to allocate */

    uint16_t b16[1]; size_t n16 = 0;
    const uint8_t u8[] = { 0xE2, 0x82, 0xAC, 0xE2, 0x82, 0xAC };
    CHECK_EQ(w32_utf8_to_utf16((const char *)u8, 6, b16, 1, &n16),
             W32_UTF_ERR_SPACE);
    CHECK_EQ(n16, 2);
}

static void test_empty_and_degenerate(void) {
    size_t need = 12345;
    char c[4]; uint16_t w[4];
    CHECK_EQ(w32_utf16_to_utf8(NULL, 0, c, sizeof c, &need), W32_UTF_OK);
    CHECK_EQ(need, 0);
    need = 12345;
    CHECK_EQ(w32_utf8_to_utf16(NULL, 0, w, 4, &need), W32_UTF_OK);
    CHECK_EQ(need, 0);
    /* NULL source with a non-zero length is an argument error, not a crash. */
    CHECK_EQ(w32_utf16_to_utf8(NULL, 3, c, sizeof c, &need), W32_UTF_ERR_ARG);
    CHECK_EQ(w32_utf8_to_utf16(NULL, 3, w, 4, &need), W32_UTF_ERR_ARG);
}

static void test_utf16_len_bounded(void) {
    const uint16_t term[] = { 'a', 'b', 0 };
    CHECK_EQ(w32_utf16_len(term, 8), 2);
    /* Unterminated: must stop at the bound, not run away. */
    const uint16_t unterm[] = { 'a', 'b', 'c', 'd' };
    CHECK_EQ(w32_utf16_len(unterm, 4), 4);
    CHECK_EQ(w32_utf16_len(NULL, 4), 0);
}

int main(void) {
    printf("== w32 UTF-16/UTF-8 conversion (W32-1) ==\n");

    RUN(test_utf8_overlong);
    RUN(test_utf8_truncated);
    RUN(test_utf8_bad_continuation);
    RUN(test_utf8_surrogate_and_range);
    RUN(test_utf16_lone_surrogate);

    RUN(test_ascii);
    RUN(test_bmp_two_and_three_byte);
    RUN(test_astral_surrogate_pair);
    RUN(test_embedded_nul);

    RUN(test_measure_then_convert);
    RUN(test_too_small_reports_need);
    RUN(test_empty_and_degenerate);
    RUN(test_utf16_len_bounded);

    printf("%s: %d/%d tests passed\n",
           failed ? "FAIL" : "PASS", passed, tn);
    return failed ? 1 : 0;
}

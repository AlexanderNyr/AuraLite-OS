/* tests/unit/test_ao_utf8.c — libaodoc's UTF-8 layer (OFFICE_PLAN.md OF1).
 *
 * Text in the office suite is UTF-8 and nothing else, so the decoder is a
 * boundary of sorts: a document that arrives with an overlong form or a
 * surrogate half must be REFUSED at the boundary, not normalised into "almost
 * the same text" that then fails an equality test three layers up.  Every
 * rejection class has a case below, and every accept case has a round-trip.
 */

#include <stdio.h>
#include <string.h>

#include "aodoc.h"

static int checks = 0, failures = 0;

static void ck(const char *what, int cond) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static void expect_ok(const char *bytes, size_t len, int want_n, uint32_t want_cp) {
    uint32_t cp = 0;
    int n = ao_utf8_decode(bytes, len, &cp);
    char msg[128];
    snprintf(msg, sizeof(msg), "decode -> %d bytes, cp U+%04X", want_n, want_cp);
    ck(msg, n == want_n && cp == want_cp);
}

static void expect_err(const char *bytes, size_t len, int want, const char *why) {
    uint32_t cp = 0x1234;
    int n = ao_utf8_decode(bytes, len, &cp);
    char msg[160];
    snprintf(msg, sizeof(msg), "reject %s", why);
    ck(msg, n == want);
    snprintf(msg, sizeof(msg), "rejection %s leaves U+FFFD", why);
    ck(msg, cp == 0xFFFDu);
}

int main(void) {
    /* ---- round trip over the ranges the suite actually sets text in ------- */
    static const uint32_t cps[] = {
        0x0000, 0x0009, 0x000A, 0x0020, 0x007E, 0x00A0, 0x00AB, 0x00BB,
        0x0410, 0x0430, 0x0401, 0x0451, 0x0416, 0x044F, 0x0490,
        0x2013, 0x2014, 0x2026, 0x2116, 0x20AC, 0x4E2D, 0xD7FF, 0xE000,
        0xFFFF, 0x10000, 0x1F600, 0x10FFFF,
    };
    int rt_ok = 1;
    for (size_t i = 0; i < sizeof(cps) / sizeof(cps[0]); i++) {
        char buf[4];
        int n = ao_utf8_encode(cps[i], buf);
        uint32_t back = 0;
        int m = (n > 0) ? ao_utf8_decode(buf, (size_t)n, &back) : -99;
        if (n <= 0 || m != n || back != cps[i]) {
            printf("  round-trip failed at U+%04X (n=%d m=%d back=U+%04X)\n",
                   cps[i], n, m, back);
            rt_ok = 0;
        }
    }
    ck("utf8: 27 codepoints round-trip through encode/decode", rt_ok);

    /* ---- shortest forms ---------------------------------------------------- */
    expect_ok("A", 1, 1, 0x41);
    expect_ok("\xC2\xA9", 2, 2, 0x00A9);                 /* © */
    expect_ok("\xD0\x9F", 2, 2, 0x041F);                 /* П */
    expect_ok("\xE2\x80\x94", 3, 3, 0x2014);             /* — */
    expect_ok("\xF0\x9F\x98\x80", 4, 4, 0x1F600);        /* emoji */
    expect_ok("\xEF\xBF\xBD", 3, 3, 0xFFFD);             /* replacement itself */

    /* ---- the rejection classes -------------------------------------------- */
    expect_err("\xC0\x80", 2, AO_UTF8_E_OVERLONG, "overlong NUL (C0 80)");
    expect_err("\xC1\xBF", 2, AO_UTF8_E_OVERLONG, "overlong 0x7F (C1 BF)");
    expect_err("\xE0\x80\x80", 3, AO_UTF8_E_OVERLONG, "3-byte overlong zero");
    expect_err("\xE0\x9F\xBF", 3, AO_UTF8_E_OVERLONG, "3-byte overlong below U+0800");
    expect_err("\xF0\x8F\xBF\xBF", 4, AO_UTF8_E_OVERLONG, "4-byte overlong below U+10000");
    expect_err("\xED\xA0\x80", 3, AO_UTF8_E_SURROGATE, "high surrogate U+D800");
    expect_err("\xED\xBF\xBF", 3, AO_UTF8_E_SURROGATE, "low surrogate U+DFFF");
    expect_err("\xF4\x90\x80\x80", 4, AO_UTF8_E_RANGE, "U+110000");
    expect_err("\xF7\xBF\xBF\xBF", 4, AO_UTF8_E_LEAD, "F7 cannot lead (5-byte form)");
    expect_err("\xFF", 1, AO_UTF8_E_LEAD, "0xFF is never valid");
    expect_err("\x80", 1, AO_UTF8_E_LEAD, "stray continuation byte");
    expect_err("\xD0", 1, AO_UTF8_E_TRUNCATED, "lead byte with no tail");
    expect_err("\xD0\x9F" + 1, 1, AO_UTF8_E_LEAD, "lone continuation");
    expect_err("\xE2\x80", 2, AO_UTF8_E_TRUNCATED, "3-byte, one byte short");
    expect_err("\xF0\x9F\x98", 3, AO_UTF8_E_TRUNCATED, "4-byte, one byte short");
    expect_err("\xE2" "A\xA8", 3, AO_UTF8_E_TRUNCATED, "non-continuation in tail");

    /* ---- encoding refuses what decoding refuses ---------------------------- */
    {
        char buf[4];
        ck("utf8: encode refuses a surrogate", ao_utf8_encode(0xD800, buf) == 0);
        ck("utf8: encode refuses > U+10FFFF", ao_utf8_encode(0x110000, buf) == 0);
        ck("utf8: encode accepts U+10FFFF", ao_utf8_encode(0x10FFFF, buf) == 4);
    }

    /* ---- validity and counting over whole strings -------------------------- */
    {
        const char *ru = "Привет, мир — 2026";
        size_t len = strlen(ru);
        ck("utf8: «Привет, мир — 2026» is valid", ao_utf8_valid(ru, len) == 1);
        ck("utf8: it is 18 codepoints", ao_utf8_count(ru, len) == 18);
        ck("utf8: it is more bytes than codepoints", len > 18);

        const char bad[] = "ok\xC0\x80ok";
        ck("utf8: a string containing an overlong is invalid",
           ao_utf8_valid(bad, sizeof(bad) - 1) == 0);
        /* Damage counts as one codepoint per bad BYTE, which is how a renderer
         * resyncs and keeps drawing. */
        ck("utf8: damaged bytes each count as one",
           ao_utf8_count(bad, sizeof(bad) - 1) > 4);
    }

    /* ---- NUL is data inside a length-delimited run ------------------------- */
    {
        uint32_t cp = 0xFFFF;
        int n = ao_utf8_decode("\0", 1, &cp);
        ck("utf8: NUL decodes as U+0000 in a length-delimited buffer",
           n == 1 && cp == 0);
        ck("utf8: zero bytes is truncated, not a crash",
           ao_utf8_decode("x", 0, &cp) == AO_UTF8_E_TRUNCATED);
    }

    /* ---- backward stepping is codepoint-wise ------------------------------- */
    {
        const char *s = "aП\xD0\xB1";        /* a П б */
        size_t len = strlen(s);
        size_t a = ao_utf8_prev(s, len);          /* before б */
        size_t b = ao_utf8_prev(s, a);            /* before П */
        size_t c = ao_utf8_prev(s, b);            /* before a */
        ck("utf8: prev(4) steps over the 2-byte б", a == 3);
        ck("utf8: prev(3) steps over the 2-byte П", b == 1);
        ck("utf8: prev(1) steps over ASCII", c == 0);
        ck("utf8: prev(0) is 0", ao_utf8_prev(s, 0) == 0);
    }

    printf("AO UTF8: %s (%d checks)\n", failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}

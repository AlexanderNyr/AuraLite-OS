/* w32/tests/w32a2_locale.c — W32A-2 guest fixture: locales and strings.
 *
 * Code pages, locale info, string types, comparison, case mapping,
 * conversions, and the lstr family.  Exits 55/1.
 *
 * NOTE: CharUpperW/CharLowerW/IsChar*W/IsTextUnicode are implemented by
 * W32A-2 and pinned by the host suite, but they are user32/advapi32
 * exports on Windows, so a guest cannot import them until W32A-5 binds
 * them; this fixture does not touch them.
 */

#include "w32a2_common.h"

static int localesSeen;
static WCHAR localesFirst[8];

static BOOL CALLBACK localeCb(LPWSTR id) {
    int i;
    localesSeen++;
    if (localesSeen == 1 && id != NULL) {
        for (i = 0; i < 7 && id[i] != 0; i++)
            localesFirst[i] = id[i];
        localesFirst[i] = 0;
    }
    return TRUE;
}

void __stdcall winstart(void) {
    CPINFO cpi;
    char abuf[128];
    WCHAR w[128], a[16], b[16], dst[16], src[16], nm[16];
    int n;

    w32a2_out = GetStdHandle(STD_OUTPUT_HANDLE);

    CHECKX(GetACP() == 65001, "loc-acp");
    CHECKX(GetOEMCP() == 65001, "loc-oemcp");
    CHECKX(GetCPInfo(65001, &cpi) && cpi.MaxCharSize == 4, "loc-cp-u8");
    CHECKX(GetCPInfo(1252, &cpi) && cpi.MaxCharSize == 1, "loc-cp-1252");
    CHECKX(!GetCPInfo(65000, &cpi), "loc-cp-bad-fails");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER, "loc-cp-bad-code");
    CHECKX(IsValidCodePage(1252), "loc-validcp");
    CHECKX(!IsValidCodePage(65000), "loc-validcp-bad");
    CHECKX(!IsDBCSLeadByteEx(1252, 0x81), "loc-lead1252");
    CHECKX(IsDBCSLeadByteEx(65001, 0xD0), "loc-lead-u8");
    CHECKX(!IsDBCSLeadByteEx(65001, 0x41), "loc-lead-ascii");
    CHECKX(GetUserDefaultLCID() == 0x0409, "loc-lcid");
    CHECKX(IsValidLocale(0x0409, 0), "loc-valid");
    CHECKX(!IsValidLocale(0x0419, 0), "loc-valid-bad");

    localesSeen = 0;
    localesFirst[0] = 0;
    CHECKX(EnumSystemLocalesW(localeCb, LCID_SUPPORTED), "loc-enum");
    CHECKX(localesSeen == 1, "loc-enum-once");
    CHECKX(weq(localesFirst, "0409"), "loc-enum-id");
    CHECKX(!EnumSystemLocalesW(NULL, LCID_SUPPORTED), "loc-enum-null");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER,
        "loc-enum-null-code");
    CHECKX(!EnumSystemLocalesW(localeCb, 99), "loc-enum-badflags");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER,
        "loc-enum-badflags-code");

    n = GetLocaleInfoW(0x0409, LOCALE_SNAME, w, 128);
    CHECKX(n == 5 && weq(w, "en-US"), "loc-sname");
    GetLocaleInfoW(0x0409, LOCALE_SDAYNAME1, w, 128);
    CHECKX(weq(w, "Sunday"), "loc-day");
    GetLocaleInfoW(0x0409, 0x31u, w, 128);
    CHECKX(weq(w, "Sun"), "loc-abbr1");
    GetLocaleInfoW(0x0409, 0x37u, w, 128);
    CHECKX(weq(w, "Sat"), "loc-abbr7");
    GetLocaleInfoW(0x0409, LOCALE_SMONTHNAME12, w, 128);
    CHECKX(weq(w, "December"), "loc-month12");
    GetLocaleInfoA(0x0409, LOCALE_SSHORTDATE, abuf, 128);
    {
        int k = 0;
        while (abuf[k] != 0)
            k++;
        CHECKX(k == 8 && memcmp(abuf, "M/d/yyyy", 8) == 0,
            "loc-shortdate");
    }
    CHECKX(GetLocaleInfoW(0x0409, LOCALE_SNAME, w, 2) == 6,
        "loc-short-need");
    CHECKX(GetLastError() == ERROR_INSUFFICIENT_BUFFER,
        "loc-short-code");
    CHECKX(GetLocaleInfoW(0x0419, LOCALE_SNAME, w, 128) == 0,
        "loc-ru-fails");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER, "loc-ru-code");
    nm[0] = L'e'; nm[1] = L'n'; nm[2] = L'-'; nm[3] = L'U';
    nm[4] = L'S'; nm[5] = 0;
    CHECKX(GetLocaleInfoEx(nm, LOCALE_SCURRENCY, w, 128) == 1,
        "loc-ex-ok");
    CHECKX(weq(w, "$"), "loc-ex-cur");
    nm[0] = L'r'; nm[1] = L'u'; nm[4] = L'R';
    CHECKX(GetLocaleInfoEx(nm, LOCALE_SNAME, w, 128) == 0,
        "loc-ex-ru-fails");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER, "loc-ex-ru-code");

    /* String types, Latin and Cyrillic. */
    {
        WORD types[8];
        WCHAR s[8];
        s[0] = L'A'; s[1] = L'a'; s[2] = L'5'; s[3] = L' ';
        s[4] = 0x416u; s[5] = 0x436u; s[6] = 0;
        CHECKX(GetStringTypeW(CT_CTYPE1, s, -1, types), "loc-ct1");
        CHECKX((types[0] & C1_UPPER) != 0, "loc-ct-upper");
        CHECKX((types[1] & C1_LOWER) != 0, "loc-ct-lower");
        CHECKX((types[2] & C1_DIGIT) != 0, "loc-ct-digit");
        CHECKX((types[3] & C1_SPACE) != 0, "loc-ct-space");
        CHECKX((types[4] & C1_UPPER) != 0, "loc-ct-cyupper");
        CHECKX((types[5] & C1_LOWER) != 0, "loc-ct-cylower");
        CHECKX(GetStringTypeW(CT_CTYPE2, s, 2, types), "loc-ct2");
        CHECKX(types[0] == C2_LEFTTORIGHT, "loc-ct2-ltr");
        CHECKX(GetStringTypeW(CT_CTYPE3, s, 1, types), "loc-ct3");
        CHECKX(!GetStringTypeW(CT_CTYPE1 | CT_CTYPE2, s, 1, types),
            "loc-ct-both-fails");
        CHECKX(GetLastError() == ERROR_INVALID_PARAMETER,
            "loc-ct-both-code");
        CHECKX(GetStringTypeExA(0x0409, CT_CTYPE1, "Z", 1, types),
            "loc-ctexa");
        CHECKX((types[0] & C1_UPPER) != 0, "loc-ctexa-upper");
    }

    /* Compare. */
    a[0] = L'a'; a[1] = L'b'; a[2] = L'c'; a[3] = 0;
    b[0] = L'a'; b[1] = L'b'; b[2] = L'd'; b[3] = 0;
    CHECKX(CompareStringW(0x0409, 0, a, -1, b, -1) == CSTR_LESS_THAN,
        "loc-cmp-less");
    a[0] = L'A'; a[1] = L'B'; a[2] = L'C'; a[3] = 0;
    b[0] = L'a'; b[1] = L'b'; b[2] = L'c'; b[3] = 0;
    CHECKX(CompareStringW(0x0409, NORM_IGNORECASE, a, -1, b, -1) ==
        CSTR_EQUAL, "loc-cmp-ci");
    CHECKX(CompareStringW(0x0409, 0, a, -1, b, -1) == CSTR_LESS_THAN,
        "loc-cmp-case");
    a[0] = L'a'; a[1] = L'-'; a[2] = L'b'; a[3] = 0;
    b[0] = L'a'; b[1] = L'b'; b[2] = 0;
    CHECKX(CompareStringW(0x0409, NORM_IGNORESYMBOLS, a, -1, b, -1) ==
        CSTR_EQUAL, "loc-cmp-sym");
    a[0] = 0x410u; a[1] = 0;
    b[0] = 0x430u; b[1] = 0;
    CHECKX(CompareStringW(0x0409, NORM_IGNORECASE, a, -1, b, -1) ==
        CSTR_EQUAL, "loc-cmp-cy");
    a[0] = L'a'; a[1] = L'b'; a[2] = L'c'; a[3] = 0;
    b[0] = L'a'; b[1] = L'b'; b[2] = L'd'; b[3] = 0;
    nm[0] = L'e'; nm[1] = L'n'; nm[2] = L'-'; nm[3] = L'U';
    nm[4] = L'S'; nm[5] = 0;
    CHECKX(CompareStringEx(nm, 0, a, -1, b, -1, NULL, NULL, 0) ==
        CSTR_LESS_THAN, "loc-cmp-ex");

    /* Map. */
    src[0] = L'a'; src[1] = L'B'; src[2] = L'c'; src[3] = L'Z';
    src[4] = 0;
    CHECKX(LCMapStringW(0x0409, LCMAP_UPPERCASE, src, -1, dst, 16) == 5,
        "loc-map-up-n");
    CHECKX(weq(dst, "ABCZ"), "loc-map-up");
    CHECKX(LCMapStringW(0x0409, LCMAP_LOWERCASE, src, -1, dst, 16) == 5,
        "loc-map-lo-n");
    CHECKX(weq(dst, "abcz"), "loc-map-lo");
    src[0] = L'A'; src[1] = 0;
    CHECKX(LCMapStringW(0x0409, LCMAP_FULLWIDTH, src, -1, dst, 16) == 2,
        "loc-map-fw");
    CHECKX(dst[0] == 0xFF21u, "loc-map-fw-v");
    src[0] = 0x3042u; src[1] = 0;
    CHECKX(LCMapStringW(0x0409, LCMAP_KATAKANA, src, -1, dst, 16) == 2,
        "loc-map-kata");
    CHECKX(dst[0] == 0x30A2u, "loc-map-kata-v");
    {
        char key[16];
        src[0] = L'a'; src[1] = L'b'; src[2] = 0;
        CHECKX(LCMapStringW(0x0409, LCMAP_SORTKEY, src, -1, (LPWSTR)key,
            sizeof(key)) == 6, "loc-map-sort-n");
        CHECKX(key[0] == 0 && key[1] == 'a', "loc-map-sort");
    }
    src[0] = L'A'; src[1] = 0;
    CHECKX(LCMapStringW(0x0409, LCMAP_BYTEREV, src, -1, dst, 16) == 2,
        "loc-map-brev");
    CHECKX(dst[0] == 0x4100u, "loc-map-brev-v");

    /* Conversions. */
    n = MultiByteToWideChar(65001, 0, "A\xD0\x96", 3, w, 64);
    CHECKX(n == 2 && w[0] == 0x41u && w[1] == 0x416u, "loc-mb-u8");
    CHECKX(MultiByteToWideChar(65001, MB_ERR_INVALID_CHARS, "\xFF", 1, w,
        64) == 0, "loc-mb-strict-fails");
    CHECKX(GetLastError() == ERROR_NO_UNICODE_TRANSLATION,
        "loc-mb-strict-code");
    n = MultiByteToWideChar(65001, 0, "\xFF", 1, w, 64);
    CHECKX(n == 1 && w[0] == 0xFFFDu, "loc-mb-lax");
    n = MultiByteToWideChar(1252, 0, "\xE9\x80", 2, w, 64);
    CHECKX(n == 2 && w[0] == 0xE9u && w[1] == 0x20ACu, "loc-mb-1252");
    CHECKX(MultiByteToWideChar(1252, MB_ERR_INVALID_CHARS, "\x81", 1, w,
        64) == 0, "loc-mb-1252-strict");
    CHECKX(MultiByteToWideChar(65001, 0, "hi", -1, NULL, 0) == 3,
        "loc-mb-measure");
    {
        char mb[64];
        BOOL used = FALSE;
        w[0] = 0x41u; w[1] = 0x416u; w[2] = 0;
        n = WideCharToMultiByte(65001, 0, w, -1, mb, 64, NULL, NULL);
        CHECKX(n == 4 && memcmp(mb, "A\xD0\x96", 4) == 0, "loc-wm-u8");
        w[0] = 0xE9u; w[1] = 0;
        n = WideCharToMultiByte(1252, 0, w, -1, mb, 64, NULL, &used);
        CHECKX(n == 2 && (unsigned char)mb[0] == 0xE9u && !used,
            "loc-wm-1252");
        w[0] = 0x4E00u; w[1] = 0;
        n = WideCharToMultiByte(1252, 0, w, -1, mb, 64, NULL, &used);
        CHECKX(n == 2 && mb[0] == '?' && used, "loc-wm-subst");
    }

    /* lstr. */
    {
        WCHAR d[16];
        char cd[8];
        a[0] = L'a'; a[1] = L'b'; a[2] = L'c'; a[3] = 0;
        b[0] = L'a'; b[1] = L'b'; b[2] = L'd'; b[3] = 0;
        CHECKX(lstrcmpW(a, b) < 0, "loc-lstr-cmp");
        CHECKX(lstrcmpW(a, a) == 0, "loc-lstr-eq");
        a[0] = L'A'; a[1] = L'B'; a[2] = L'C'; a[3] = 0;
        b[0] = L'a'; b[1] = L'b'; b[2] = L'c'; b[3] = 0;
        CHECKX(lstrcmpiW(a, b) == 0, "loc-lstr-ci");
        CHECKX(lstrcmpiA("ABC", "abc") == 0, "loc-lstr-cia");
        CHECKX(lstrcpyW(d, b) == d && weq(d, "abc"), "loc-lstr-cpy");
        a[0] = L'h'; a[1] = L'e'; a[2] = L'l'; a[3] = L'l';
        a[4] = L'o'; a[5] = 0;
        CHECKX(lstrcpynW(d, a, 4) == d && weq(d, "hel"), "loc-lstr-cpyn");
        CHECKX(lstrcpynA(cd, "hello", 4) == cd, "loc-lstr-cpyna");
        CHECKX(cd[0] == 'h' && cd[1] == 'e' && cd[2] == 'l' &&
            cd[3] == 0, "loc-lstr-cpyna-v");
        d[0] = L'a'; d[1] = L'b'; d[2] = 0;
        b[0] = L'c'; b[1] = L'd'; b[2] = 0;
        CHECKX(lstrcatW(d, b) == d && weq(d, "abcd"), "loc-lstr-cat");
        CHECKX(lstrlenW(d) == 4, "loc-lstr-len");
    }

    w32a2_done("LOCALE");
}

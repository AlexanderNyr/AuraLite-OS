/* w32/src/kernel32_loc.c — W32A-2 locales and strings.
 *
 * Three models a reader needs before touching this file:
 *
 * PAGES.  ACP and OEMCP are both UTF-8 (65001).  That is a choice, not a
 * fallback: AuraLite's filesystem and environment are UTF-8 end to end, so
 * the A entry points pass bytes through instead of converting twice.  1252
 * exists as a real second page (its table is below, derived from the
 * Unicode standard's mapping); 65000 (UTF-7) is refused everywhere.
 *
 * LOCALES.  One locale exists: en-US (0x0409).  USER_DEFAULT and
 * SYSTEM_DEFAULT alias it; the invariant locale (0x7F) is accepted by the
 * comparison and classification entry points (their data is
 * locale-independent by construction) and refused by GetLocaleInfo (whose
 * strings are English words, not invariant ones).  The LCTYPE table is
 * en-US data the way the registry would hold it.
 *
 * LETTERS.  Case and class come from hand ranges, not tables: ASCII,
 * Latin-1, Latin Extended-A, Cyrillic, Greek, CJK Unified, kana, Hangul
 * syllables, Arabic and Hebrew.  Unlisted units fold to themselves and
 * classify as nothing — a documented boundary.  The same fold drives
 * CompareString, LCMapString, CharUpper/Lower and enumeration matching,
 * so one fix repairs all four.
 */

#include "w32/kernel32.h"
#include "w32/w32_handle.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"

#ifndef AURALITE_W32_HOST_TEST
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#endif

static W32_BOOL loc_fail(W32_DWORD code) {
    w32_set_last_error(code);
    return 0;
}

/* ---- code pages -------------------------------------------------------------------- */

/* 1252 bytes 0x80-0xFF to Unicode.  Five slots are undefined in 1252
 * (0x81,0x8D,0x8F,0x90,0x9D) and map to U+FFFF here as the sentinel the
 * converters test for.  Values are the Unicode standard's mapping. */
static const uint16_t loc_1252_hi[128] = {
    0x20AC, 0xFFFF, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFF, 0x017D, 0xFFFF,
    0xFFFF, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFF, 0x017E, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF
};

/* Unicode to 1252: the inverse search, linear over 128 entries.  Returns
 * -1 when the unit has no 1252 byte. */
static int loc_1252_from_u(uint16_t u) {
    int i;
    if (u < 0x80)
        return (int)u;
    for (i = 0; i < 128; i++) {
        if (loc_1252_hi[i] == u)
            return 0x80 + i;
    }
    return -1;
}

W32ABI W32_UINT GetACP(void) {
    return W32_CP_UTF8;
}

W32ABI W32_UINT GetOEMCP(void) {
    return W32_CP_UTF8;
}

W32ABI W32_BOOL GetCPInfo(W32_UINT cp, W32_CPINFO *out) {
    size_t i;
    if (!out)
        return loc_fail(W32_ERROR_INVALID_PARAMETER);
    if (cp == W32_CP_UTF8 || cp == W32_CP_ACP || cp == W32_CP_OEMCP ||
        cp == W32_CP_THREAD_ACP) {
        out->MaxCharSize = 4;
        out->DefaultChar[0] = (uint8_t)'?';
        out->DefaultChar[1] = 0;
        for (i = 0; i < 12; i++)
            out->LeadByte[i] = 0;
        return 1;
    }
    if (cp == 1252u) {
        out->MaxCharSize = 1;
        out->DefaultChar[0] = (uint8_t)'?';
        out->DefaultChar[1] = 0;
        for (i = 0; i < 12; i++)
            out->LeadByte[i] = 0;
        return 1;
    }
    return loc_fail(W32_ERROR_INVALID_PARAMETER);
}

W32ABI W32_BOOL IsValidCodePage(W32_UINT cp) {
    return (cp == W32_CP_ACP || cp == W32_CP_OEMCP ||
            cp == W32_CP_THREAD_ACP || cp == 1252u || cp == W32_CP_UTF8);
}

W32ABI W32_BOOL IsDBCSLeadByteEx(W32_UINT cp, uint8_t byte) {
    if (cp == 1252u)
        return 0;               /* single-byte page: never a lead */
    if (cp == W32_CP_UTF8 || cp == W32_CP_ACP || cp == W32_CP_OEMCP ||
        cp == W32_CP_THREAD_ACP) {
        /* UTF-8 lead bytes: 0xC2-0xF4 (0xC0/0xC1 never lead anything). */
        return (byte >= 0xC2 && byte <= 0xF4);
    }
    return 0;
}

W32ABI W32_LCID GetUserDefaultLCID(void) {
    return W32_LOCALE_EN_US;
}

W32ABI W32_LANGID GetUserDefaultLangID(void) {
    return W32_LANG_EN_US;
}

W32ABI W32_LANGID GetSystemDefaultLangID(void) {
    return W32_LANG_EN_US;
}

W32ABI W32_BOOL IsValidLocale(W32_LCID locale, W32_DWORD flags) {
    if (flags & ~0x7u)
        return 0;
    return (locale == W32_LOCALE_EN_US ||
            locale == W32_LOCALE_USER_DEFAULT ||
            locale == W32_LOCALE_SYSTEM_DEFAULT);
}

W32ABI W32_BOOL EnumSystemLocalesW(W32_LOCALE_ENUMPROC cb, W32_DWORD flags) {
    static W32_WCHAR id[5] = { '0', '4', '0', '9', 0 };
    if (!cb)
        return loc_fail(W32_ERROR_INVALID_PARAMETER);
    if (flags != 1u && flags != 2u)
        return loc_fail(W32_ERROR_INVALID_PARAMETER);
    return cb(id) ? 1 : 1;      /* one locale; a FALSE return stops nothing */
}

/* ---- case ---------------------------------------------------------------------------- */

W32_WCHAR w32_fold_char(W32_WCHAR c) {
    if (c >= (W32_WCHAR)'A' && c <= (W32_WCHAR)'Z')
        return (W32_WCHAR)(c + 32);
    if (c >= 0xC0u && c <= 0xDEu && c != 0xD7u)
        return (W32_WCHAR)(c + 32);
    if (c >= 0x100u && c <= 0x12Eu && (c % 2u) == 0u)
        return (W32_WCHAR)(c + 1);
    if (c == 0x130u)
        return (W32_WCHAR)'i';
    if (c == 0x132u || c == 0x134u || c == 0x136u)
        return (W32_WCHAR)(c + 1);
    if ((c == 0x139u || c == 0x13Bu || c == 0x13Du || c == 0x13Fu ||
         c == 0x141u || c == 0x143u || c == 0x145u || c == 0x147u))
        return (W32_WCHAR)(c + 1);
    if (c >= 0x14Au && c <= 0x176u && (c % 2u) == 0u)
        return (W32_WCHAR)(c + 1);
    if (c == 0x178u)
        return 0xFFu;
    if (c >= 0x179u && c <= 0x17Eu && (c % 2u) == 1u)
        return (W32_WCHAR)(c + 1);
    if (c >= 0x400u && c <= 0x40Fu)
        return (W32_WCHAR)(c + 0x50u);
    if (c >= 0x410u && c <= 0x42Fu)
        return (W32_WCHAR)(c + 0x20u);
    if ((c >= 0x391u && c <= 0x3A1u) || (c >= 0x3A3u && c <= 0x3ABu))
        return (W32_WCHAR)(c + 0x20u);
    /* Accented Greek capitals (0x386-0x38C) have no single-unit fold here;
     * they compare unfolded — documented, and fixtures do not cover them. */
    return c;
}

static W32_WCHAR loc_upper_char(W32_WCHAR c) {
    if (c >= (W32_WCHAR)'a' && c <= (W32_WCHAR)'z')
        return (W32_WCHAR)(c - 32);
    if (c >= 0xE0u && c <= 0xFEu && c != 0xF7u)
        return (W32_WCHAR)(c - 32);
    if (c == 0xFFu)
        return 0x178u;
    if (c == 0x131u)
        return (W32_WCHAR)'I';
    if (c >= 0x101u && c <= 0x12Fu && (c % 2u) == 1u)
        return (W32_WCHAR)(c - 1);
    if (c == 0x133u || c == 0x135u || c == 0x137u)
        return (W32_WCHAR)(c - 1);
    if (c == 0x13Au || c == 0x13Cu || c == 0x13Eu || c == 0x140u ||
        c == 0x142u || c == 0x144u || c == 0x146u || c == 0x148u)
        return (W32_WCHAR)(c - 1);
    if (c >= 0x14Bu && c <= 0x177u && (c % 2u) == 1u)
        return (W32_WCHAR)(c - 1);
    if (c >= 0x17Au && c <= 0x17Fu && (c % 2u) == 0u && c != 0x17Au)
        return (W32_WCHAR)(c - 1);
    if (c >= 0x17Bu && c <= 0x17Fu && (c % 2u) == 0u)
        return (W32_WCHAR)(c - 1);
    if (c >= 0x450u && c <= 0x45Fu)
        return (W32_WCHAR)(c - 0x50u);
    if (c >= 0x430u && c <= 0x44Fu)
        return (W32_WCHAR)(c - 0x20u);
    if ((c >= 0x3B1u && c <= 0x3C1u) || (c >= 0x3C3u && c <= 0x3CBu))
        return (W32_WCHAR)(c - 0x20u);
    return c;
}

/* ---- classification ------------------------------------------------------------------------ */

static W32_WORD loc_c1(W32_WCHAR c) {
    if (c < 0x80u) {
        if (c >= (W32_WCHAR)'A' && c <= (W32_WCHAR)'Z')
            return W32_C1_UPPER | W32_C1_ALPHA;
        if (c >= (W32_WCHAR)'a' && c <= (W32_WCHAR)'z')
            return W32_C1_LOWER | W32_C1_ALPHA;
        if (c >= (W32_WCHAR)'0' && c <= (W32_WCHAR)'9')
            return W32_C1_DIGIT | W32_C1_XDIGIT;
        if ((c >= (W32_WCHAR)'A' && c <= (W32_WCHAR)'F') ||
            (c >= (W32_WCHAR)'a' && c <= (W32_WCHAR)'f'))
            return W32_C1_XDIGIT | W32_C1_ALPHA |
                ((c <= (W32_WCHAR)'F') ? W32_C1_UPPER : W32_C1_LOWER);
        if (c == 0x20u || c == 0x09u)
            return W32_C1_SPACE | W32_C1_BLANK;
        if (c == 0x0Au || c == 0x0Du || c == 0x0Bu || c == 0x0Cu)
            return W32_C1_SPACE | W32_C1_CNTRL;
        if (c < 0x20u || c == 0x7Fu)
            return W32_C1_CNTRL;
        return W32_C1_PUNCT;
    }
    if (c >= 0xC0u && c <= 0xDEu && c != 0xD7u)
        return W32_C1_UPPER | W32_C1_ALPHA;
    if ((c >= 0xE0u && c <= 0xFEu && c != 0xF7u) || c == 0xDFu || c == 0xFFu)
        return W32_C1_LOWER | W32_C1_ALPHA;
    if (c == 0xB5u)
        return W32_C1_LOWER | W32_C1_ALPHA;
    if (c == 0xAAu || c == 0xBAu)
        return W32_C1_ALPHA;
    if (c == 0xA0u)
        return W32_C1_SPACE | W32_C1_BLANK;
    if (c >= 0xA1u && c <= 0xBFu)
        return W32_C1_PUNCT;
    if (c == 0xD7u || c == 0xF7u)
        return W32_C1_PUNCT;
    if (c >= 0x100u && c <= 0x17Fu && c != 0x138u) {
        /* Letters throughout (0x138 kra is lower too, handled by fold). */
        W32_WCHAR f = w32_fold_char(c);
        W32_WCHAR u = loc_upper_char(c);
        if (f != c)
            return W32_C1_UPPER | W32_C1_ALPHA;
        if (u != c)
            return W32_C1_LOWER | W32_C1_ALPHA;
        return W32_C1_ALPHA;
    }
    if (c >= 0x400u && c <= 0x42Fu)
        return W32_C1_UPPER | W32_C1_ALPHA;
    if (c >= 0x430u && c <= 0x45Fu)
        return W32_C1_LOWER | W32_C1_ALPHA;
    if ((c >= 0x391u && c <= 0x3ABu && c != 0x3A2u))
        return W32_C1_UPPER | W32_C1_ALPHA;
    if (c >= 0x3ABu + 1u && c <= 0x3CFu && c != 0x3BB + 0x17u) {
        if ((c >= 0x3B1u && c <= 0x3CBu) || (c >= 0x3ACu && c <= 0x3AFu) ||
            c == 0x3CDu || c == 0x3CEu)
            return W32_C1_LOWER | W32_C1_ALPHA;
    }
    if (c >= 0x386u && c <= 0x38Cu && c != 0x387u && c != 0x38Bu)
        return W32_C1_UPPER | W32_C1_ALPHA;
    if (c >= 0x4E00u && c <= 0x9FFFu)
        return W32_C1_ALPHA;
    if ((c >= 0x3041u && c <= 0x3096u) || (c >= 0x30A1u && c <= 0x30FAu))
        return W32_C1_ALPHA;
    if (c >= 0xAC00u && c <= 0xD7A3u)
        return W32_C1_ALPHA;
    if ((c >= 0x620u && c <= 0x64Au) || (c >= 0x5D0u && c <= 0x5EAu))
        return W32_C1_ALPHA;
    if (c >= 0xFF10u && c <= 0xFF19u)
        return W32_C1_DIGIT;
    if ((c >= 0xFF21u && c <= 0xFF3Au))
        return W32_C1_UPPER | W32_C1_ALPHA;
    if ((c >= 0xFF41u && c <= 0xFF5Au))
        return W32_C1_LOWER | W32_C1_ALPHA;
    if (c >= 0x660u && c <= 0x669u)
        return W32_C1_DIGIT;
    if (c == 0x3000u)
        return W32_C1_SPACE | W32_C1_BLANK;
    return 0;
}

static W32_WORD loc_c2(W32_WCHAR c) {
    W32_WORD c1 = loc_c1(c);
    if (c1 & (W32_C1_SPACE | W32_C1_BLANK))
        return W32_C2_WHITESPACE;
    if (c1 & W32_C1_DIGIT)
        return W32_C2_EUROPENUMBER;
    if ((c >= 0x620u && c <= 0x64Au) || (c >= 0x5D0u && c <= 0x5EAu))
        return W32_C2_RIGHTTOLEFT;
    if (c1 & W32_C1_ALPHA)
        return W32_C2_LEFTTORIGHT;
    return W32_C2_OTHERNEUTRAL;
}

static W32_WORD loc_c3(W32_WCHAR c) {
    W32_WORD t = 0;
    if (c >= 0x300u && c <= 0x36Fu) {
        t |= W32_C3_NONSPACING;
        if (c <= 0x315u)
            t |= W32_C3_DIACRITIC;
        return t;
    }
    if (c >= 0x30A1u && c <= 0x30FAu)
        return W32_C3_KATAKANA | W32_C3_ALPHA;
    if (c >= 0x3041u && c <= 0x3096u)
        return W32_C3_HIRAGANA | W32_C3_ALPHA;
    if (c >= 0x4E00u && c <= 0x9FFFu)
        return W32_C3_IDEOGRAPH | W32_C3_ALPHA;
    if (c >= 0xFF61u && c <= 0xFFDCu)
        return W32_C3_HALFWIDTH | W32_C3_KATAKANA;
    if (c >= 0xFF01u && c <= 0xFF5Eu) {
        t = W32_C3_FULLWIDTH;
        if ((c >= 0xFF21u && c <= 0xFF3Au) ||
            (c >= 0xFF41u && c <= 0xFF5Au))
            t |= W32_C3_ALPHA;
        return t;
    }
    if (c == 0x640u)
        return W32_C3_KASHIDA;
    if ((c >= 0x400u && c <= 0x45Fu) ||
        (c >= 0x391u && c <= 0x3CFu) ||
        (c >= 0x620u && c <= 0x64Au) ||
        (c >= 0x5D0u && c <= 0x5EAu))
        return W32_C3_ALPHA;
    if (c >= 0xAC00u && c <= 0xD7A3u)
        return W32_C3_ALPHA;
    return 0;
}

static int loc_ok(W32_LCID locale, int allowInvariant) {
    if (locale == W32_LOCALE_EN_US || locale == W32_LOCALE_USER_DEFAULT ||
        locale == W32_LOCALE_SYSTEM_DEFAULT)
        return 1;
    if (allowInvariant && locale == W32_LOCALE_INVARIANT)
        return 1;
    return 0;
}

/* ---- GetStringType --------------------------------------------------------------------- */

static W32_BOOL loc_string_type(W32_DWORD infoType, const W32_WCHAR *src,
                                W32_INT count, W32_WORD *types) {
    W32_INT n;
    W32_INT i;

    if (infoType != W32_CT_CTYPE1 && infoType != W32_CT_CTYPE2 &&
        infoType != W32_CT_CTYPE3)
        return loc_fail(W32_ERROR_INVALID_PARAMETER);
    if (!src || !types)
        return loc_fail(W32_ERROR_INVALID_PARAMETER);
    if (count < -1 || count == 0)
        return (count == 0);    /* 0: nothing to do, still success */
    if (count == -1) {
        n = 0;
        while (src[n] != 0)
            n++;
        n++;                    /* the NUL classifies as control */
    } else {
        n = count;
    }
    for (i = 0; i < n; i++) {
        if (infoType == W32_CT_CTYPE1)
            types[i] = loc_c1(src[i]);
        else if (infoType == W32_CT_CTYPE2)
            types[i] = loc_c2(src[i]);
        else
            types[i] = loc_c3(src[i]);
    }
    return 1;
}

W32ABI W32_BOOL GetStringTypeExW(W32_LCID locale, W32_DWORD infoType,
                                W32_LPCWSTR src, W32_INT count,
                                W32_WORD *types) {
    if (!loc_ok(locale, 1))
        return loc_fail(W32_ERROR_INVALID_PARAMETER);
    return loc_string_type(infoType, src, count, types);
}

W32ABI W32_BOOL GetStringTypeW(W32_DWORD infoType, W32_LPCWSTR src,
                              W32_INT count, W32_WORD *types) {
    return loc_string_type(infoType, src, count, types);
}

W32ABI W32_BOOL GetStringTypeExA(W32_LCID locale, W32_DWORD infoType,
                                W32_LPCSTR src, W32_INT count,
                                W32_WORD *types) {
    W32_INT n;
    W32_INT i;

    /* Byte classification follows 1252: for UTF-8 text the W form is
     * authoritative (documented in the file header). */
    if (!loc_ok(locale, 1))
        return loc_fail(W32_ERROR_INVALID_PARAMETER);
    if (infoType != W32_CT_CTYPE1 && infoType != W32_CT_CTYPE2 &&
        infoType != W32_CT_CTYPE3)
        return loc_fail(W32_ERROR_INVALID_PARAMETER);
    if (!src || !types)
        return loc_fail(W32_ERROR_INVALID_PARAMETER);
    if (count < -1 || count == 0)
        return (count == 0);
    if (count == -1) {
        n = 0;
        while (src[n] != '\0')
            n++;
        n++;
    } else {
        n = count;
    }
    for (i = 0; i < n; i++) {
        unsigned char b = (unsigned char)src[i];
        W32_WCHAR u = (b < 0x80) ? (W32_WCHAR)b : loc_1252_hi[b - 0x80];
        if (u == 0xFFFFu)
            u = (W32_WCHAR)'?';
        if (infoType == W32_CT_CTYPE1)
            types[i] = loc_c1(u);
        else if (infoType == W32_CT_CTYPE2)
            types[i] = loc_c2(u);
        else
            types[i] = loc_c3(u);
    }
    return 1;
}

/* ---- locale info ---------------------------------------------------------------------------- */

/* Sunday-first: SDAYNAME1 is Sunday, SDAYNAME7 is Saturday. */
static const char *loc_day_full[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday"
};

static const char *loc_day_abbr[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *loc_month_full[12] = {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December"
};

static const char *loc_month_abbr[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char *loc_lctype_str(W32_DWORD lctype) {
    switch (lctype) {
    case W32_LOCALE_SNAME: return "en-US";
    case W32_LOCALE_SENGLANGUAGE: return "English";
    case W32_LOCALE_SENGCOUNTRY: return "United States";
    case W32_LOCALE_SABBREVCTRYNAME: return "USA";
    case W32_LOCALE_SNATIVECTRYNAME: return "United States";
    case W32_LOCALE_SISO3166CTRYNAME: return "US";
    case W32_LOCALE_SISO639LANGNAME: return "en";
    case W32_LOCALE_SABBREVLANGNAME: return "ENU";
    case W32_LOCALE_SNATIVELANGNAME: return "English (United States)";
    case W32_LOCALE_IDATE: return "0";
    case W32_LOCALE_ILDATE: return "0";
    case W32_LOCALE_ITIME: return "0";
    case W32_LOCALE_ICURRDIGITS: return "2";
    case W32_LOCALE_IINTLCURRDIGITS: return "2";
    case W32_LOCALE_INEGNUMBER: return "1";
    case W32_LOCALE_STHOUSAND: return ",";
    case W32_LOCALE_SDECIMAL: return ".";
    case W32_LOCALE_SCURRENCY: return "$";
    case W32_LOCALE_SINTLSYMBOL: return "USD";
    case W32_LOCALE_SMONDECIMALSEP: return ".";
    case W32_LOCALE_SMONTHOUSANDSEP: return ",";
    case W32_LOCALE_SSHORTDATE: return "M/d/yyyy";
    case W32_LOCALE_SLONGDATE: return "dddd, MMMM d, yyyy";
    case W32_LOCALE_STIMEFORMAT: return "h:mm:ss tt";
    case W32_LOCALE_S1159: return "AM";
    case W32_LOCALE_S2359: return "PM";
    case W32_LOCALE_SPOSITIVESIGN: return "";
    case W32_LOCALE_SNEGATIVESIGN: return "-";
    default: break;
    }
    if (lctype >= W32_LOCALE_SDAYNAME1 && lctype <= W32_LOCALE_SDAYNAME7)
        return loc_day_full[lctype - W32_LOCALE_SDAYNAME1];
    if (lctype >= 0x31u && lctype <= 0x37u)
        return loc_day_abbr[lctype - 0x31u];
    if (lctype >= W32_LOCALE_SMONTHNAME1 && lctype <= W32_LOCALE_SMONTHNAME12)
        return loc_month_full[lctype - W32_LOCALE_SMONTHNAME1];
    if (lctype >= 0x44u && lctype <= W32_LOCALE_SABBREVMONTHNAME12)
        return loc_month_abbr[lctype - 0x44u];
    return NULL;
}

static W32_INT loc_info_out(const char *s, W32_LPWSTR buf, W32_INT cch) {
    size_t need = 0;
    size_t need2 = 0;
    int rc;

    rc = w32_utf8_to_utf16(s, strlen(s), NULL, 0, &need);
    if (rc != W32_UTF_OK) {
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    if (cch < 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (cch == 0 || !buf)
        return (W32_INT)(need + 1);
    if ((size_t)cch <= need) {
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return (W32_INT)(need + 1);
    }
    rc = w32_utf8_to_utf16(s, strlen(s), buf, need, &need2);
    if (rc != W32_UTF_OK || need2 != need) {
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    buf[need] = 0;
    return (W32_INT)need;
}

W32ABI W32_INT GetLocaleInfoW(W32_LCID locale, W32_DWORD lctype,
                              W32_LPWSTR buf, W32_INT cch) {
    const char *s;
    if (!loc_ok(locale, 0)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    s = loc_lctype_str(lctype);
    if (!s) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return loc_info_out(s, buf, cch);
}

W32ABI W32_INT GetLocaleInfoA(W32_LCID locale, W32_DWORD lctype,
                              W32_LPSTR buf, W32_INT cch) {
    const char *s;
    size_t n;
    if (!loc_ok(locale, 0)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    s = loc_lctype_str(lctype);
    if (!s) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* All table strings are ASCII: the A form copies bytes. */
    n = strlen(s);
    if (cch < 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (cch == 0 || !buf)
        return (W32_INT)(n + 1);
    if ((size_t)cch <= n) {
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return (W32_INT)(n + 1);
    }
    memcpy(buf, s, n + 1);
    return (W32_INT)n;
}

static int loc_name_ok(W32_LPCWSTR name) {
    static const char want[] = "en-US";
    size_t i;
    if (!name)
        return 0;
    for (i = 0; i < sizeof(want) - 1; i++) {
        W32_WCHAR c = name[i];
        if (c >= (W32_WCHAR)'A' && c <= (W32_WCHAR)'Z')
            c = (W32_WCHAR)(c + 32);
        {
            char w = want[i];
            if (w >= 'A' && w <= 'Z')
                w = (char)(w - 'A' + 'a');
            if ((char)c != w)
                return 0;
        }
    }
    return name[sizeof(want) - 1] == 0;
}

W32ABI W32_INT GetLocaleInfoEx(W32_LPCWSTR name, W32_DWORD lctype,
                               W32_LPWSTR buf, W32_INT cch) {
    const char *s;
    if (!loc_name_ok(name)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    s = loc_lctype_str(lctype);
    if (!s) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return loc_info_out(s, buf, cch);
}

/* ---- comparison ------------------------------------------------------------------------------ */

static W32_WCHAR loc_norm_unit(W32_WCHAR c, W32_DWORD flags) {
    if (flags & (W32_NORM_IGNORECASE | W32_LINGUISTIC_IGNORECASE))
        c = w32_fold_char(c);
    if (flags & W32_NORM_IGNOREWIDTH) {
        if (c >= 0xFF01u && c <= 0xFF5Eu)
            c = (W32_WCHAR)(c - 0xFEE0u);
        else if (c == 0x3000u)
            c = 0x20u;
    }
    if (flags & W32_NORM_IGNOREKANATYPE) {
        if (c >= 0x30A1u && c <= 0x30F6u)
            c = (W32_WCHAR)(c - 0x60u);
    }
    return c;
}

static int loc_skip_unit(W32_WCHAR c, W32_DWORD flags) {
    if ((flags & W32_NORM_IGNORENONSPACE) && c >= 0x300u && c <= 0x36Fu)
        return 1;
    if (flags & W32_NORM_IGNORESYMBOLS) {
        W32_WORD t = loc_c1(c);
        if (t & (W32_C1_PUNCT | W32_C1_SPACE | W32_C1_CNTRL))
            return 1;
    }
    return 0;
}

#define LOC_CMP_FLAGS (W32_NORM_IGNORECASE | W32_NORM_IGNORENONSPACE | \
    W32_NORM_IGNORESYMBOLS | W32_NORM_IGNOREKANATYPE | W32_NORM_IGNOREWIDTH | \
    W32_LINGUISTIC_IGNORECASE | W32_SORT_STRINGSORT)

static W32_INT loc_compare(W32_DWORD flags, W32_LPCWSTR s1, W32_INT n1,
                           W32_LPCWSTR s2, W32_INT n2) {
    W32_INT len1;
    W32_INT len2;
    W32_INT i1 = 0;
    W32_INT i2 = 0;

    if (flags & ~LOC_CMP_FLAGS) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!s1 || !s2) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (n1 < -1 || n2 < -1 || n1 == 0 || n2 == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* SORT_STRINGSORT is a no-op: code-unit order already is string sort. */
    if (n1 == -1) {
        len1 = 0;
        while (s1[len1] != 0)
            len1++;
    } else {
        len1 = n1;
    }
    if (n2 == -1) {
        len2 = 0;
        while (s2[len2] != 0)
            len2++;
    } else {
        len2 = n2;
    }
    for (;;) {
        W32_WCHAR c1;
        W32_WCHAR c2;
        while (i1 < len1 && loc_skip_unit(s1[i1], flags))
            i1++;
        while (i2 < len2 && loc_skip_unit(s2[i2], flags))
            i2++;
        if (i1 >= len1 || i2 >= len2) {
            if (i1 >= len1 && i2 >= len2)
                return W32_CSTR_EQUAL;
            return (i1 >= len1) ? W32_CSTR_LESS_THAN : W32_CSTR_GREATER_THAN;
        }
        c1 = loc_norm_unit(s1[i1], flags);
        c2 = loc_norm_unit(s2[i2], flags);
        if (c1 != c2)
            return (c1 < c2) ? W32_CSTR_LESS_THAN : W32_CSTR_GREATER_THAN;
        i1++;
        i2++;
    }
}

W32ABI W32_INT CompareStringW(W32_LCID locale, W32_DWORD flags,
                              W32_LPCWSTR s1, W32_INT n1,
                              W32_LPCWSTR s2, W32_INT n2) {
    if (!loc_ok(locale, 1)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return loc_compare(flags, s1, n1, s2, n2);
}

W32ABI W32_INT CompareStringEx(W32_LPCWSTR name, W32_DWORD flags,
                               W32_LPCWSTR s1, W32_INT n1,
                               W32_LPCWSTR s2, W32_INT n2,
                               void *version, void *reserved,
                               W32_LONG_PTR param) {
    if (version || reserved || param) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (name && name[0] == 0) {
        /* Empty name: the invariant locale, which compares the same. */
    } else if (!loc_name_ok(name)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return loc_compare(flags, s1, n1, s2, n2);
}

/* ---- mapping ------------------------------------------------------------------------------ */

static W32_INT loc_map_str(W32_DWORD flags, W32_LPCWSTR src, W32_INT srclen,
                           W32_LPWSTR dst, W32_INT dstlen) {
    W32_INT len;
    W32_INT i;

    if (flags & ~(W32_LCMAP_LOWERCASE | W32_LCMAP_UPPERCASE | W32_LCMAP_SORTKEY |
                  W32_LCMAP_BYTEREV | W32_LCMAP_HIRAGANA | W32_LCMAP_KATAKANA |
                  W32_LCMAP_HALFWIDTH | W32_LCMAP_FULLWIDTH | LOC_CMP_FLAGS)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((flags & (W32_LCMAP_LOWERCASE | W32_LCMAP_UPPERCASE)) ==
        (W32_LCMAP_LOWERCASE | W32_LCMAP_UPPERCASE)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!src) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (srclen < -1 || srclen == 0 || dstlen < 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (srclen == -1) {
        len = 0;
        while (src[len] != 0)
            len++;
        len++;
    } else {
        len = srclen;
    }
    if (flags & W32_LCMAP_SORTKEY) {
        /* Byte-image key: NORM-folded units as big-endian pairs plus the
         * terminator.  It orders exactly like CompareString — weights
         * would only compress it.  dstlen counts BYTES here. */
        W32_INT need = 0;
        W32_INT o = 0;
        for (i = 0; i < len; i++) {
            W32_WCHAR c = src[i];
            if (srclen == -1 && c == 0)
                break;
            if (loc_skip_unit(c, flags))
                continue;
            need += 2;
        }
        need += 2;
        if (dstlen == 0 || !dst)
            return need;
        if (dstlen < need) {
            w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        {
            unsigned char *b = (unsigned char *)dst;
            for (i = 0; i < len; i++) {
                W32_WCHAR c = src[i];
                if (srclen == -1 && c == 0)
                    break;
                if (loc_skip_unit(c, flags))
                    continue;
                c = loc_norm_unit(c, flags);
                b[o++] = (unsigned char)(c >> 8);
                b[o++] = (unsigned char)(c & 0xFFu);
            }
            b[o++] = 0;
            b[o++] = 0;
        }
        return need;
    }
    if (dstlen == 0 || !dst)
        return len;
    if (dstlen < len) {
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (i = 0; i < len; i++) {
        W32_WCHAR c = src[i];
        if ((srclen == -1 && c == 0) || loc_skip_unit(c, flags)) {
            /* Skipped units vanish from a mapping (they cannot sort into
             * a string that keeps positions)... except the NUL, which
             * terminates.  SKIP flags with a mapping are unusual; the
             * honest behaviour is to drop the skipped units. */
            if (srclen == -1 && c == 0) {
                dst[i] = 0;
                return i + 1;
            }
            continue;
        }
        if (flags & W32_LCMAP_LOWERCASE)
            c = w32_fold_char(c);
        else if (flags & W32_LCMAP_UPPERCASE)
            c = loc_upper_char(c);
        if (flags & W32_LCMAP_HIRAGANA) {
            if (c >= 0x30A1u && c <= 0x30F6u)
                c = (W32_WCHAR)(c - 0x60u);
        }
        if (flags & W32_LCMAP_KATAKANA) {
            if (c >= 0x3041u && c <= 0x3096u)
                c = (W32_WCHAR)(c + 0x60u);
        }
        if (flags & W32_LCMAP_HALFWIDTH) {
            if (c >= 0xFF01u && c <= 0xFF5Eu)
                c = (W32_WCHAR)(c - 0xFEE0u);
            else if (c == 0x3000u)
                c = 0x20u;
        }
        if (flags & W32_LCMAP_FULLWIDTH) {
            if (c >= 0x21u && c <= 0x7Eu)
                c = (W32_WCHAR)(c + 0xFEE0u);
            else if (c == 0x20u)
                c = 0x3000u;
        }
        if (flags & W32_LCMAP_BYTEREV)
            c = (W32_WCHAR)(((c & 0xFFu) << 8) | (c >> 8));
        dst[i] = c;
    }
    return len;
}

W32ABI W32_INT LCMapStringW(W32_LCID locale, W32_DWORD flags,
                            W32_LPCWSTR src, W32_INT srclen,
                            W32_LPWSTR dst, W32_INT dstlen) {
    if (!loc_ok(locale, 1)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return loc_map_str(flags, src, srclen, dst, dstlen);
}

W32ABI W32_INT LCMapStringEx(W32_LPCWSTR name, W32_DWORD flags,
                             W32_LPCWSTR src, W32_INT srclen,
                             W32_LPWSTR dst, W32_INT dstlen,
                             void *version, void *reserved,
                             W32_LONG_PTR param) {
    if (version || reserved || param) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (name && name[0] == 0) {
    } else if (!loc_name_ok(name)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return loc_map_str(flags, src, srclen, dst, dstlen);
}

W32ABI W32_INT LCMapStringA(W32_LCID locale, W32_DWORD flags,
                            W32_LPCSTR src, W32_INT srclen,
                            W32_LPSTR dst, W32_INT dstlen) {
    W32_INT wlen;
    W32_WCHAR *wbuf = NULL;
    W32_WCHAR *wout = NULL;
    W32_INT mapped;
    W32_INT i;

    if (!loc_ok(locale, 1)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!src) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (srclen < -1 || srclen == 0 || dstlen < 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* ACP is UTF-8: decode, map wide, encode back. */
    if (srclen == -1)
        srclen = (W32_INT)strlen(src) + 1;
    {
        size_t need = 0;
        size_t need2 = 0;
        int rc = w32_utf8_to_utf16(src, (size_t)srclen, NULL, 0, &need);
        if (rc != W32_UTF_OK) {
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        wbuf = (W32_WCHAR *)malloc((need + 1) * sizeof(W32_WCHAR));
        wout = (W32_WCHAR *)malloc((need + 1) * sizeof(W32_WCHAR));
        if (!wbuf || !wout) {
            free(wbuf);
            free(wout);
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        rc = w32_utf8_to_utf16(src, (size_t)srclen, wbuf, need, &need2);
        if (rc != W32_UTF_OK || need2 != need) {
            free(wbuf);
            free(wout);
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        wlen = (W32_INT)need;
    }
    mapped = loc_map_str(flags, wbuf, wlen, wout, wlen + 1);
    free(wbuf);
    if (mapped == 0) {
        free(wout);
        return 0;
    }
    {
        size_t need = 0;
        size_t need2 = 0;
        int rc = w32_utf16_to_utf8(wout, (size_t)mapped, NULL, 0, &need);
        if (rc != W32_UTF_OK) {
            free(wout);
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        if (dstlen == 0 || !dst) {
            free(wout);
            return (W32_INT)need;
        }
        if ((size_t)dstlen < need) {
            free(wout);
            w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        rc = w32_utf16_to_utf8(wout, (size_t)mapped, dst, (size_t)dstlen,
            &need2);
        free(wout);
        if (rc != W32_UTF_OK || need2 != need) {
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        (void)i;
        return (W32_INT)need;
    }
}

/* ---- conversion ------------------------------------------------------------------------------ */

W32ABI W32_INT MultiByteToWideChar(W32_UINT cp, W32_DWORD flags,
                                   W32_LPCSTR mb, W32_INT cbMulti,
                                   W32_LPWSTR wc, W32_INT cchWide) {
    int strict;
    W32_INT srclen;

    if (cp != W32_CP_UTF8 && cp != W32_CP_ACP && cp != W32_CP_OEMCP &&
        cp != W32_CP_THREAD_ACP && cp != 1252u) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (flags & ~(0x1u | 0x8u)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!mb) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (cbMulti < -1 || cbMulti == 0 || cchWide < 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    strict = (flags & 0x8u) != 0;       /* MB_ERR_INVALID_CHARS */
    if (cbMulti == -1)
        srclen = (W32_INT)strlen(mb) + 1;
    else
        srclen = cbMulti;
    if (cp == 1252u) {
        W32_INT i;
        if (cchWide == 0 || !wc)
            return srclen;
        if (cchWide < srclen) {
            w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        for (i = 0; i < srclen; i++) {
            unsigned char b = (unsigned char)mb[i];
            if (b < 0x80) {
                wc[i] = (W32_WCHAR)b;
            } else {
                uint16_t u = loc_1252_hi[b - 0x80];
                if (u == 0xFFFFu) {
                    if (strict) {
                        w32_set_last_error(
                            W32_ERROR_NO_UNICODE_TRANSLATION);
                        return 0;
                    }
                    u = 0xFFFDu;
                }
                wc[i] = u;
            }
        }
        return srclen;
    }
    /* UTF-8.  Strict mode converts whole; substitution mode walks byte by
     * byte emitting U+FFFD per bad sequence. */
    if (strict) {
        size_t need = 0;
        size_t need2 = 0;
        int rc = w32_utf8_to_utf16(mb, (size_t)srclen, NULL, 0, &need);
        if (rc != W32_UTF_OK) {
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        if (cchWide == 0 || !wc)
            return (W32_INT)need;
        if ((size_t)cchWide < need) {
            w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        rc = w32_utf8_to_utf16(mb, (size_t)srclen, wc, (size_t)cchWide,
            &need2);
        if (rc != W32_UTF_OK || need2 != need) {
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        return (W32_INT)need;
    } else {
        W32_INT si = 0;
        W32_INT wi = 0;
        while (si < srclen) {
            unsigned char c0 = (unsigned char)mb[si];
            size_t seqlen;
            size_t need = 0;
            size_t need2 = 0;
            uint16_t units[2];
            int rc;
            if (c0 < 0x80) {
                seqlen = 1;
            } else if (c0 < 0xC2) {
                seqlen = 1;     /* stray continuation: one FFFD */
                need = 0;
                goto subst;
            } else if (c0 < 0xE0) {
                seqlen = 2;
            } else if (c0 < 0xF0) {
                seqlen = 3;
            } else if (c0 < 0xF5) {
                seqlen = 4;
            } else {
                seqlen = 1;
                goto subst;
            }
            if (si + (W32_INT)seqlen > srclen) {
                seqlen = (size_t)(srclen - si);
                goto subst;
            }
            rc = w32_utf8_to_utf16(mb + si, seqlen, units, 2, &need);
            if (rc == W32_UTF_OK && need >= 1 && need <= 2) {
                if (wc) {
                    if (wi + (W32_INT)need > cchWide) {
                        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
                        return 0;
                    }
                    {
                        size_t k;
                        for (k = 0; k < need; k++)
                            wc[wi + (W32_INT)k] = units[k];
                    }
                }
                wi += (W32_INT)need;
                si += (W32_INT)seqlen;
                continue;
            }
subst:
            if (wc) {
                if (wi + 1 > cchWide) {
                    w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
                    return 0;
                }
                wc[wi] = 0xFFFDu;
            }
            wi++;
            si += (seqlen > 0) ? (W32_INT)seqlen : 1;
            (void)need2;
        }
        return wi;
    }
}

W32ABI W32_INT WideCharToMultiByte(W32_UINT cp, W32_DWORD flags,
                                   W32_LPCWSTR wc, W32_INT cchWide,
                                   W32_LPSTR mb, W32_INT cbMulti,
                                   W32_LPCSTR defaultChar,
                                   W32_BOOL *usedDefault) {
    int strict;
    W32_INT srclen;
    W32_INT si = 0;
    W32_INT bi = 0;
    int used = 0;

    if (cp != W32_CP_UTF8 && cp != W32_CP_ACP && cp != W32_CP_OEMCP &&
        cp != W32_CP_THREAD_ACP && cp != 1252u) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* COMPOSITECHECK, ERR_INVALID_CHARS, NO_BEST_FIT, DEFAULTCHAR,
     * SEPCHARS, DISCARDNS accepted; NO_BEST_FIT is a no-op (best-fit is
     * never applied); the bidi flags are no-ops (no shaping here). */
    if (flags & ~(0x10u | 0x20u | 0x40u | 0x80u | 0x200u | 0x400u)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!wc) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (cchWide < -1 || cchWide == 0 || cbMulti < 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((flags & 0x80u) && (defaultChar || usedDefault)) {
        /* ERR_INVALID_CHARS forbids the default-char parameters. */
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    strict = (flags & 0x80u) != 0;
    if (cchWide == -1) {
        srclen = 0;
        while (wc[srclen] != 0)
            srclen++;
        srclen++;
    } else {
        srclen = cchWide;
    }
    while (si < srclen) {
        uint16_t u = wc[si];
        uint32_t cp32 = u;
        int units = 1;
        if (u >= 0xD800u && u <= 0xDBFFu) {
            if (si + 1 < srclen && wc[si + 1] >= 0xDC00u &&
                wc[si + 1] <= 0xDFFFu) {
                cp32 = 0x10000u + ((uint32_t)(u - 0xD800u) << 10) +
                    (uint32_t)(wc[si + 1] - 0xDC00u);
                units = 2;
            } else {
                /* Lone high surrogate. */
                if (strict) {
                    w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
                    return 0;
                }
                cp32 = 0xFFFDu;
                used = 1;
            }
        } else if (u >= 0xDC00u && u <= 0xDFFFu) {
            if (strict) {
                w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
                return 0;
            }
            cp32 = 0xFFFDu;
            used = 1;
        }
        if (cp == 1252u) {
            int b;
            if (cp32 > 0xFFFFu) {
                b = -1;
            } else {
                b = loc_1252_from_u((uint16_t)cp32);
            }
            if (b < 0) {
                if (strict) {
                    w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
                    return 0;
                }
                used = 1;
                b = defaultChar ? (unsigned char)defaultChar[0] : '?';
            }
            if (mb) {
                if (bi + 1 > cbMulti) {
                    w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
                    return 0;
                }
                mb[bi] = (char)b;
            }
            bi++;
        } else {
            unsigned char seq[4];
            int sl;
            if (cp32 < 0x80u) {
                seq[0] = (unsigned char)cp32;
                sl = 1;
            } else if (cp32 < 0x800u) {
                seq[0] = (unsigned char)(0xC0u | (cp32 >> 6));
                seq[1] = (unsigned char)(0x80u | (cp32 & 0x3Fu));
                sl = 2;
            } else if (cp32 < 0x10000u) {
                seq[0] = (unsigned char)(0xE0u | (cp32 >> 12));
                seq[1] = (unsigned char)(0x80u | ((cp32 >> 6) & 0x3Fu));
                seq[2] = (unsigned char)(0x80u | (cp32 & 0x3Fu));
                sl = 3;
            } else {
                seq[0] = (unsigned char)(0xF0u | (cp32 >> 18));
                seq[1] = (unsigned char)(0x80u | ((cp32 >> 12) & 0x3Fu));
                seq[2] = (unsigned char)(0x80u | ((cp32 >> 6) & 0x3Fu));
                seq[3] = (unsigned char)(0x80u | (cp32 & 0x3Fu));
                sl = 4;
            }
            if (mb) {
                if (bi + sl > cbMulti) {
                    w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
                    return 0;
                }
                {
                    int k;
                    for (k = 0; k < sl; k++)
                        mb[bi + k] = (char)seq[k];
                }
            }
            bi += sl;
        }
        si += units;
    }
    if (usedDefault)
        *usedDefault = used;
    return bi;
}

/* ---- lstr --------------------------------------------------------------------------- */

W32ABI W32_INT lstrcmpW(W32_LPCWSTR a, W32_LPCWSTR b) {
    return CompareStringW(W32_LOCALE_USER_DEFAULT, 0, a, -1, b, -1) - 2;
}

W32ABI W32_INT lstrcmpiA(W32_LPCSTR a, W32_LPCSTR b) {
    W32_INT r;
    W32_WCHAR *wa = NULL;
    W32_WCHAR *wb = NULL;
    size_t na = 0;
    size_t nb = 0;
    size_t n2 = 0;
    int rc;

    if (!a || !b) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    rc = w32_utf8_to_utf16(a, strlen(a) + 1, NULL, 0, &na);
    if (rc != W32_UTF_OK) {
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    rc = w32_utf8_to_utf16(b, strlen(b) + 1, NULL, 0, &nb);
    if (rc != W32_UTF_OK) {
        w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    wa = (W32_WCHAR *)malloc((na + 1) * sizeof(W32_WCHAR));
    wb = (W32_WCHAR *)malloc((nb + 1) * sizeof(W32_WCHAR));
    if (!wa || !wb) {
        free(wa);
        free(wb);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    w32_utf8_to_utf16(a, strlen(a) + 1, wa, na, &n2);
    w32_utf8_to_utf16(b, strlen(b) + 1, wb, nb, &n2);
    r = CompareStringW(W32_LOCALE_USER_DEFAULT, W32_NORM_IGNORECASE,
        wa, -1, wb, -1) - 2;
    free(wa);
    free(wb);
    return r;
}

W32ABI W32_INT lstrcmpiW(W32_LPCWSTR a, W32_LPCWSTR b) {
    return CompareStringW(W32_LOCALE_USER_DEFAULT, W32_NORM_IGNORECASE,
        a, -1, b, -1) - 2;
}

W32ABI W32_LPWSTR lstrcpyW(W32_LPWSTR dst, W32_LPCWSTR src) {
    W32_WCHAR *d = dst;
    if (!dst || !src)
        return dst;
    while ((*d++ = *src++) != 0)
        ;
    return dst;
}

W32ABI W32_LPSTR lstrcpynA(W32_LPSTR dst, W32_LPCSTR src, W32_INT n) {
    W32_INT i;
    if (!dst || !src || n <= 0)
        return dst;
    for (i = 0; i < n - 1 && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
    return dst;
}

W32ABI W32_LPWSTR lstrcpynW(W32_LPWSTR dst, W32_LPCWSTR src, W32_INT n) {
    W32_INT i;
    if (!dst || !src || n <= 0)
        return dst;
    for (i = 0; i < n - 1 && src[i] != 0; i++)
        dst[i] = src[i];
    dst[i] = 0;
    return dst;
}

W32ABI W32_LPWSTR lstrcatW(W32_LPWSTR dst, W32_LPCWSTR src) {
    W32_WCHAR *d = dst;
    if (!dst || !src)
        return dst;
    while (*d != 0)
        d++;
    while ((*d++ = *src++) != 0)
        ;
    return dst;
}

W32ABI W32_INT lstrlenW(W32_LPCWSTR s) {
    W32_INT n = 0;
    if (!s)
        return 0;
    while (s[n] != 0)
        n++;
    return n;
}

/* ---- casing entry points ---------------------------------------------------------------------- */

W32ABI W32_LPWSTR CharUpperW(W32_LPWSTR s) {
    uintptr_t v = (uintptr_t)s;
    W32_WCHAR *p;
    if (v < 0x10000u)
        return (W32_LPWSTR)(uintptr_t)loc_upper_char((W32_WCHAR)v);
    if (!s)
        return s;
    for (p = s; *p != 0; p++)
        *p = loc_upper_char(*p);
    return s;
}

W32ABI W32_LPWSTR CharLowerW(W32_LPWSTR s) {
    uintptr_t v = (uintptr_t)s;
    W32_WCHAR *p;
    if (v < 0x10000u)
        return (W32_LPWSTR)(uintptr_t)w32_fold_char((W32_WCHAR)v);
    if (!s)
        return s;
    for (p = s; *p != 0; p++)
        *p = w32_fold_char(*p);
    return s;
}

W32ABI W32_BOOL IsCharAlphaW(W32_WCHAR c) {
    return (loc_c1(c) & W32_C1_ALPHA) != 0;
}

W32ABI W32_BOOL IsCharAlphaNumericW(W32_WCHAR c) {
    return (loc_c1(c) & (W32_C1_ALPHA | W32_C1_DIGIT)) != 0;
}

W32ABI W32_BOOL IsCharUpperW(W32_WCHAR c) {
    return (loc_c1(c) & W32_C1_UPPER) != 0;
}

W32ABI W32_BOOL IsCharLowerW(W32_WCHAR c) {
    return (loc_c1(c) & W32_C1_LOWER) != 0;
}

/* ---- IsTextUnicode: the heuristic soup, ladled honestly -------------------------------------------------
 *
 * Each requested test runs and sets its result bit; the return says whether
 * the buffer looks like UTF-16 text (no NOT_UNICODE bits, at least one
 * positive sign).  The statistics test counts NUL-high units the way the
 * documented heuristic does; controls looks for CR/LF/TAB/NUL pairs.
 */

W32ABI W32_BOOL IsTextUnicode(const void *buf, W32_INT len, W32_INT *flags) {
    const unsigned char *b;
    W32_INT in;
    W32_INT out = 0;
    W32_INT i;
    int odd;
    int nulHigh = 0;
    int units;

    if (!buf || len < 0)
        return 0;
    b = (const unsigned char *)buf;
    in = flags ? *flags : 0xFFFF;
    odd = (len % 2) != 0;
    units = len / 2;
    if ((in & W32_IS_TEXT_UNICODE_SIGNATURE) && len >= 2 && b[0] == 0xFF &&
        b[1] == 0xFE)
        out |= W32_IS_TEXT_UNICODE_SIGNATURE;
    if ((in & W32_IS_TEXT_UNICODE_REVERSE_SIGNATURE) && len >= 2 &&
        b[0] == 0xFE && b[1] == 0xFF)
        out |= W32_IS_TEXT_UNICODE_REVERSE_SIGNATURE;
    for (i = 0; i < units; i++) {
        if (b[i * 2 + 1] == 0)
            nulHigh++;
    }
    if ((in & W32_IS_TEXT_UNICODE_ASCII16) && units > 0 &&
        nulHigh == units && !odd)
        out |= W32_IS_TEXT_UNICODE_ASCII16;
    if ((in & W32_IS_TEXT_UNICODE_REVERSE_ASCII16) && units > 0 && !odd) {
        int nulLow = 0;
        for (i = 0; i < units; i++) {
            if (b[i * 2] == 0)
                nulLow++;
        }
        if (nulLow == units)
            out |= W32_IS_TEXT_UNICODE_REVERSE_ASCII16;
    }
    if (in & W32_IS_TEXT_UNICODE_CONTROLS) {
        for (i = 0; i < units; i++) {
            unsigned lo = b[i * 2];
            unsigned hi = b[i * 2 + 1];
            if (hi == 0 && (lo == 0x0A || lo == 0x0D || lo == 0x09 ||
                            lo == 0x00)) {
                out |= W32_IS_TEXT_UNICODE_CONTROLS;
                break;
            }
        }
    }
    if (in & W32_IS_TEXT_UNICODE_REVERSE_CONTROLS) {
        for (i = 0; i < units; i++) {
            unsigned lo = b[i * 2];
            unsigned hi = b[i * 2 + 1];
            if (lo == 0 && (hi == 0x0A || hi == 0x0D || hi == 0x09 ||
                            hi == 0x00)) {
                out |= W32_IS_TEXT_UNICODE_REVERSE_CONTROLS;
                break;
            }
        }
    }
    if (in & W32_IS_TEXT_UNICODE_STATISTICS) {
        /* At least half the high bytes NUL and no illegal units: text. */
        if (units > 0 && nulHigh * 2 >= units && !odd)
            out |= W32_IS_TEXT_UNICODE_STATISTICS;
    }
    if (in & W32_IS_TEXT_UNICODE_REVERSE_STATISTICS) {
        int nulLow = 0;
        for (i = 0; i < units; i++) {
            if (b[i * 2] == 0)
                nulLow++;
        }
        if (units > 0 && nulLow * 2 >= units && !odd)
            out |= W32_IS_TEXT_UNICODE_REVERSE_STATISTICS;
    }
    if (in & W32_IS_TEXT_UNICODE_ILLEGAL_CHARS) {
        for (i = 0; i < units; i++) {
            unsigned u = (unsigned)b[i * 2] | ((unsigned)b[i * 2 + 1] << 8);
            if (u >= 0xD800u && u <= 0xDBFFu) {
                if (i + 1 >= units) {
                    out |= W32_IS_TEXT_UNICODE_ILLEGAL_CHARS;
                    break;
                } else {
                    unsigned v = (unsigned)b[(i + 1) * 2] |
                        ((unsigned)b[(i + 1) * 2 + 1] << 8);
                    if (v < 0xDC00u || v > 0xDFFFu) {
                        out |= W32_IS_TEXT_UNICODE_ILLEGAL_CHARS;
                        break;
                    }
                    i++;
                }
            } else if (u >= 0xDC00u && u <= 0xDFFFu) {
                out |= W32_IS_TEXT_UNICODE_ILLEGAL_CHARS;
                break;
            } else if (u == 0xFFFFu || u == 0xFFFEu) {
                out |= W32_IS_TEXT_UNICODE_ILLEGAL_CHARS;
                break;
            }
        }
    }
    if (odd)
        out |= W32_IS_TEXT_UNICODE_ODD_LENGTH;
    if (in & W32_IS_TEXT_UNICODE_NULL_BYTES) {
        for (i = 0; i + 1 < len; i++) {
            if (b[i] == 0 && b[i + 1] == 0) {
                out |= W32_IS_TEXT_UNICODE_NULL_BYTES;
                break;
            }
        }
    }
    if (flags)
        *flags = out;
    if (out & W32_IS_TEXT_UNICODE_NOT_UNICODE_MASK)
        return 0;
    if (odd)
        return 0;
    return (out & ~W32_IS_TEXT_UNICODE_NOT_UNICODE_MASK) != 0;
}

/* ---- date and time pictures --------------------------------------------------------------------------- */

static const char *loc_wday_full[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
    "Saturday"
};

static const char *loc_wday_abbr[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static void loc_cur_time(W32_SYSTEMTIME *st) {
    struct timespec ts;
    int64_t secs;
    int64_t days;
    int64_t era;
    unsigned doe;
    unsigned yoe;
    int64_t yy;
    unsigned doy;
    unsigned mp;

    memset(st, 0, sizeof(*st));
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return;
    secs = (int64_t)ts.tv_sec;
    days = secs / 86400;
    secs %= 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    {
        int64_t z = days + 719468;
        era = (z >= 0 ? z : z - 146096) / 146097;
        doe = (unsigned)(z - era * 146097);
        yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
        yy = (int64_t)yoe + era * 400;
        doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
        mp = (5u * doy + 2u) / 153u;
        st->wDay = (W32_WORD)(doy - (153u * mp + 2u) / 5u + 1u);
        st->wMonth = (W32_WORD)(mp + (mp < 10u ? 3u : (unsigned)-9));
        st->wYear = (W32_WORD)(yy + (st->wMonth <= 2u ? 1 : 0));
        st->wDayOfWeek = (W32_WORD)(((days % 7) + 7 + 4) % 7);
    }
    st->wHour = (W32_WORD)(secs / 3600);
    st->wMinute = (W32_WORD)((secs % 3600) / 60);
    st->wSecond = (W32_WORD)(secs % 60);
    st->wMilliseconds = (W32_WORD)(ts.tv_nsec / 1000000L);
}

/* Append helpers write into a growable WCHAR buffer. */
struct loc_buf {
    W32_WCHAR *w;
    size_t len;
    size_t cap;
};

static int loc_push(struct loc_buf *b, W32_WCHAR c) {
    if (b->len + 1 >= b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 64;
        W32_WCHAR *nw = (W32_WCHAR *)realloc(b->w, ncap * sizeof(W32_WCHAR));
        if (!nw)
            return -1;
        b->w = nw;
        b->cap = ncap;
    }
    b->w[b->len++] = c;
    return 0;
}

static int loc_push_ascii(struct loc_buf *b, const char *s) {
    while (*s) {
        if (loc_push(b, (W32_WCHAR)(unsigned char)*s++) != 0)
            return -1;
    }
    return 0;
}

static int loc_push_num(struct loc_buf *b, unsigned v, int width) {
    char tmp[16];
    size_t n = 0;
    size_t i;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        char rev[16];
        size_t r = 0;
        while (v > 0) {
            rev[r++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (r > 0)
            tmp[n++] = rev[--r];
    }
    /* Pad with '0' toward width.  The counter is separate from n: n
     * stays the digit count for the copy loop below. */
    {
        size_t pad = 0;
        while (n + pad < (size_t)width) {
            if (loc_push(b, (W32_WCHAR)'0') != 0)
                return -1;
            pad++;
        }
    }
    for (i = 0; i < n; i++) {
        if (loc_push(b, (W32_WCHAR)(unsigned char)tmp[i]) != 0)
            return -1;
    }
    return 0;
}

/* Render a date picture.  Returns 0 on success, -1 on OOM. */
static int loc_render_date(struct loc_buf *b, const char *pic,
                           const W32_SYSTEMTIME *st) {
    const char *p = pic;
    while (*p) {
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'') {
                if (*p == '\'' && p[1] == '\'') {
                    if (loc_push(b, (W32_WCHAR)'\'') != 0)
                        return -1;
                    p += 2;
                } else {
                    if (loc_push(b, (W32_WCHAR)(unsigned char)*p++) != 0)
                        return -1;
                }
            }
            if (*p == '\'')
                p++;
            continue;
        }
        if (*p == 'd') {
            int n = 0;
            while (p[n] == 'd')
                n++;
            if (n >= 4) {
                if (loc_push_ascii(b, loc_wday_full[st->wDayOfWeek % 7]) != 0)
                    return -1;
            } else if (n == 3) {
                if (loc_push_ascii(b, loc_wday_abbr[st->wDayOfWeek % 7]) != 0)
                    return -1;
            } else if (n == 2) {
                if (loc_push_num(b, st->wDay, 2) != 0)
                    return -1;
            } else {
                if (loc_push_num(b, st->wDay, 1) != 0)
                    return -1;
            }
            p += n;
            continue;
        }
        if (*p == 'M') {
            int n = 0;
            unsigned m;
            while (p[n] == 'M')
                n++;
            m = (st->wMonth >= 1 && st->wMonth <= 12) ? st->wMonth - 1 : 0;
            if (n >= 4) {
                if (loc_push_ascii(b, loc_month_full[m]) != 0)
                    return -1;
            } else if (n == 3) {
                if (loc_push_ascii(b, loc_month_abbr[m]) != 0)
                    return -1;
            } else if (n == 2) {
                if (loc_push_num(b, st->wMonth, 2) != 0)
                    return -1;
            } else {
                if (loc_push_num(b, st->wMonth, 1) != 0)
                    return -1;
            }
            p += n;
            continue;
        }
        if (*p == 'y') {
            int n = 0;
            while (p[n] == 'y')
                n++;
            if (n >= 4) {
                if (loc_push_num(b, st->wYear, 4) != 0)
                    return -1;
            } else if (n == 2) {
                if (loc_push_num(b, st->wYear % 100, 2) != 0)
                    return -1;
            } else {
                if (loc_push_num(b, st->wYear % 100, 1) != 0)
                    return -1;
            }
            p += n;
            continue;
        }
        if (*p == 'g') {
            while (*p == 'g')
                p++;
            if (loc_push_ascii(b, "A.D.") != 0)
                return -1;
            continue;
        }
        if (loc_push(b, (W32_WCHAR)(unsigned char)*p++) != 0)
            return -1;
    }
    return 0;
}

static int loc_render_time(struct loc_buf *b, const char *pic,
                           const W32_SYSTEMTIME *st, int force24) {
    const char *p = pic;
    while (*p) {
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'') {
                if (loc_push(b, (W32_WCHAR)(unsigned char)*p++) != 0)
                    return -1;
            }
            if (*p == '\'')
                p++;
            continue;
        }
        if (*p == 'h' || *p == 'H') {
            int n = 0;
            int isH = (*p == 'H');
            unsigned h;
            while (p[n] == p[0])
                n++;
            h = st->wHour;
            if (!isH && !force24) {
                h = h % 12;
                if (h == 0)
                    h = 12;
            }
            if (loc_push_num(b, h, (n >= 2) ? 2 : 1) != 0)
                return -1;
            p += n;
            continue;
        }
        if (*p == 'm') {
            int n = 0;
            while (p[n] == 'm')
                n++;
            if (loc_push_num(b, st->wMinute, (n >= 2) ? 2 : 1) != 0)
                return -1;
            p += n;
            continue;
        }
        if (*p == 's') {
            int n = 0;
            while (p[n] == 's')
                n++;
            if (loc_push_num(b, st->wSecond, (n >= 2) ? 2 : 1) != 0)
                return -1;
            p += n;
            continue;
        }
        if (*p == 't') {
            int n = 0;
            int pm = st->wHour >= 12;
            while (p[n] == 't')
                n++;
            if (force24) {
                p += n;
                continue;
            }
            if (n >= 2) {
                if (loc_push_ascii(b, pm ? "PM" : "AM") != 0)
                    return -1;
            } else {
                if (loc_push(b, (W32_WCHAR)(pm ? 'P' : 'A')) != 0)
                    return -1;
            }
            p += n;
            continue;
        }
        if (loc_push(b, (W32_WCHAR)(unsigned char)*p++) != 0)
            return -1;
    }
    return 0;
}

static W32_INT loc_finish_buf(struct loc_buf *b, W32_LPWSTR buf, W32_INT cch) {
    W32_INT need;
    W32_INT i;
    if (loc_push(b, 0) != 0) {
        free(b->w);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    need = (W32_INT)b->len;
    if (cch == 0 || !buf) {
        free(b->w);
        return need;
    }
    if (cch < need) {
        free(b->w);
        w32_set_last_error(W32_ERROR_INSUFFICIENT_BUFFER);
        return need;
    }
    for (i = 0; i < need; i++)
        buf[i] = b->w[i];
    free(b->w);
    return need - 1;
}

W32ABI W32_INT GetDateFormatW(W32_LCID locale, W32_DWORD flags,
                              const W32_SYSTEMTIME *time, W32_LPCWSTR fmt,
                              W32_LPWSTR buf, W32_INT cch) {
    W32_SYSTEMTIME now;
    const char *pic = NULL;
    char *pic8 = NULL;
    struct loc_buf b;
    int rc;

    if (!loc_ok(locale, 0)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (flags & ~(0x1u | 0x2u | 0x8u | 0x10u | 0x20u | 0x80000000u)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!time) {
        loc_cur_time(&now);
        time = &now;
    }
    if (time->wMonth < 1 || time->wMonth > 12 || time->wDay < 1 ||
        time->wDay > 31 || time->wDayOfWeek > 6) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!fmt) {
        if (flags & 0x8u)
            pic = "MMMM yyyy";
        else if (flags & 0x2u)
            pic = "dddd, MMMM d, yyyy";
        else
            pic = "M/d/yyyy";
    } else {
        size_t n = 0;
        size_t need = 0;
        size_t need2 = 0;
        int ok;
        while (fmt[n] != 0)
            n++;
        ok = w32_utf16_to_utf8(fmt, n, NULL, 0, &need);
        if (ok != W32_UTF_OK) {
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        pic8 = (char *)malloc(need + 1);
        if (!pic8) {
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        ok = w32_utf16_to_utf8(fmt, n, pic8, need, &need2);
        if (ok != W32_UTF_OK || need2 != need) {
            free(pic8);
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        pic8[need] = '\0';
        pic = pic8;
    }
    b.w = NULL;
    b.len = 0;
    b.cap = 0;
    rc = loc_render_date(&b, pic, time);
    free(pic8);
    if (rc != 0) {
        free(b.w);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return loc_finish_buf(&b, buf, cch);
}

W32ABI W32_INT GetDateFormatEx(W32_LPCWSTR name, W32_DWORD flags,
                               const W32_SYSTEMTIME *time, W32_LPCWSTR fmt,
                               W32_LPWSTR buf, W32_INT cch, W32_LPCWSTR cal) {
    if (cal) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (name && name[0] == 0) {
    } else if (!loc_name_ok(name)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return GetDateFormatW(W32_LOCALE_EN_US, flags, time, fmt, buf, cch);
}

W32ABI W32_INT GetTimeFormatW(W32_LCID locale, W32_DWORD flags,
                              const W32_SYSTEMTIME *time, W32_LPCWSTR fmt,
                              W32_LPWSTR buf, W32_INT cch) {
    W32_SYSTEMTIME now;
    const char *pic = NULL;
    char *pic8 = NULL;
    char defpic[32];
    struct loc_buf b;
    int rc;
    int force24;

    if (!loc_ok(locale, 0)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    force24 = (flags & 0x8u) != 0;
    if (!time) {
        loc_cur_time(&now);
        time = &now;
    }
    if (time->wHour > 23 || time->wMinute > 59 || time->wSecond > 59) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!fmt) {
        if (flags & ~(0x1u | 0x2u | 0x4u | 0x8u | 0x80000000u)) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return 0;
        }
        /* Default "h:mm:ss tt" with the flag surgery applied. */
        {
            char *w = defpic;
            *w++ = force24 ? 'H' : 'h';
            *w++ = ':';
            *w++ = 'm';
            *w++ = 'm';
            if (!(flags & (0x1u | 0x2u))) {
                *w++ = ':';
                *w++ = 's';
                *w++ = 's';
            }
            if (!(flags & (0x1u | 0x4u)) && !force24) {
                *w++ = ' ';
                *w++ = 't';
                *w++ = 't';
            }
            *w = '\0';
        }
        pic = defpic;
    } else {
        size_t n = 0;
        size_t need = 0;
        size_t need2 = 0;
        int ok;
        if (flags & ~(0x8u | 0x80000000u)) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return 0;
        }
        while (fmt[n] != 0)
            n++;
        ok = w32_utf16_to_utf8(fmt, n, NULL, 0, &need);
        if (ok != W32_UTF_OK) {
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        pic8 = (char *)malloc(need + 1);
        if (!pic8) {
            w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        ok = w32_utf16_to_utf8(fmt, n, pic8, need, &need2);
        if (ok != W32_UTF_OK || need2 != need) {
            free(pic8);
            w32_set_last_error(W32_ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        pic8[need] = '\0';
        pic = pic8;
    }
    b.w = NULL;
    b.len = 0;
    b.cap = 0;
    rc = loc_render_time(&b, pic, time, force24);
    free(pic8);
    if (rc != 0) {
        free(b.w);
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return loc_finish_buf(&b, buf, cch);
}

W32ABI W32_INT GetTimeFormatEx(W32_LPCWSTR name, W32_DWORD flags,
                               const W32_SYSTEMTIME *time, W32_LPCWSTR fmt,
                               W32_LPWSTR buf, W32_INT cch) {
    if (name && name[0] == 0) {
    } else if (!loc_name_ok(name)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return GetTimeFormatW(W32_LOCALE_EN_US, flags, time, fmt, buf, cch);
}

/* ---- wsprintfW: formatted output, fed two ways ------------------------------------------------------------
 *
 * A Windows program calls wsprintfW with C varargs, but the guest bind
 * layer cannot forward C varargs, so the formatter is split in two:
 *
 *   - w32_wsprintf_core(buf, fmt, args, nargs): formats from pre-boxed
 *     arguments.  Fixed signature, ms_abi, driven by the host tests and
 *     (at bind time) by the guest bind layer, which boxes the trapped
 *     guest slots directly.
 *
 *   - wsprintfW(buf, fmt, ...): the C shell.  It walks the format with
 *     the same tokenizer, boxes each consumed vararg, and calls the core.
 *     The shell is PLAIN sysv (no W32ABI) on purpose: a host (sysv)
 *     caller cannot feed an ms_abi varargs callee.  GCC emits no MS
 *     home-area spill from a sysv caller, so the callee's va_arg reads
 *     garbage — probe-proven (ms_sum(10,20,30) came back -841987614
 *     while its sysv twin returned 60).  The guest never calls the shell
 *     (the bind intercepts wsprintfW by name and feeds the core), so the
 *     shell's ABI is the host test's business only.
 *
 * The tokenizer is the contract both sides share: per spec it reports
 * how many star-boxes precede the value and what kind the value box is.
 * Unknown conversions (floats included) consume nothing and pass through
 * literally, so the output shows what was not understood.  A missing box
 * (the shell caps at 64, a bind may run short) formats as zero/empty
 * rather than faulting.  No buffer bound exists — Windows parity,
 * documented here so no caller mistakes this for a safe function.
 */

static W32_WCHAR *loc_put_u64(W32_WCHAR *w, uint64_t v, int base, int upper,
                              int width, int prec, int left, int zero,
                              int neg) {
    W32_WCHAR dig[64];
    int n = 0;
    int i;
    int body;
    int pad;
    if (prec > 64)
        prec = 64;
    if (v == 0) {
        dig[n++] = (W32_WCHAR)'0';
    } else {
        while (v > 0) {
            unsigned d = (unsigned)(v % (uint64_t)base);
            if (d < 10)
                dig[n++] = (W32_WCHAR)('0' + d);
            else
                dig[n++] = (W32_WCHAR)((upper ? 'A' : 'a') + d - 10);
            v /= (uint64_t)base;
        }
    }
    /* The sign rides inside the field: spaces pad outside it, zero-flag
     * zeros pad after it ("  -42", "-0042", "-42  "). */
    body = (prec > n) ? prec : n;
    pad = width - body - (neg ? 1 : 0);
    if (pad < 0)
        pad = 0;
    if (!left && !(zero && prec < 0)) {
        for (i = 0; i < pad; i++)
            *w++ = (W32_WCHAR)' ';
        pad = 0;
    }
    if (neg)
        *w++ = (W32_WCHAR)'-';
    if (!left) {
        for (i = 0; i < pad; i++)
            *w++ = (W32_WCHAR)'0';
    }
    for (i = 0; i < body - n; i++)
        *w++ = (W32_WCHAR)'0';
    for (i = n - 1; i >= 0; i--)
        *w++ = dig[i];
    if (left) {
        for (i = 0; i < pad; i++)
            *w++ = (W32_WCHAR)' ';
    }
    return w;
}

struct ws_spec {
    int star_w;         /* width arrives in a box */
    int star_p;         /* precision arrives in a box */
    int width;
    int prec;           /* -1 = absent */
    int left;
    int zero;
    int i64;
    int half;
    W32_WCHAR conv;     /* conversion letter; '%' for %%; 0 for trailing % */
    int consumes;       /* value boxes consumed (0 for %% and unknowns) */
    int vkind;          /* W32_WS_* when consumes */
};

/* Parse one conversion; *pp points AT the '%', past the letter after. */
static void ws_parse_spec(W32_LPCWSTR *pp, struct ws_spec *s) {
    W32_LPCWSTR p = *pp;

    memset(s, 0, sizeof(*s));
    s->prec = -1;
    p++;                        /* consume '%' */
    if (*p == 0) {
        s->conv = 0;            /* trailing %: a literal, ends the walk */
        *pp = p;
        return;
    }
    if (*p == (W32_WCHAR)'%') {
        s->conv = (W32_WCHAR)'%';
        *pp = p + 1;
        return;
    }
    while (*p == (W32_WCHAR)'-' || *p == (W32_WCHAR)'0' ||
           *p == (W32_WCHAR)'+' || *p == (W32_WCHAR)' ' ||
           *p == (W32_WCHAR)'#') {
        /* '+'/' '/'#' accepted and ignored (no signs/alt forms). */
        if (*p == (W32_WCHAR)'-')
            s->left = 1;
        if (*p == (W32_WCHAR)'0')
            s->zero = 1;
        p++;
    }
    if (*p == (W32_WCHAR)'*') {
        s->star_w = 1;
        p++;
    } else {
        while (*p >= (W32_WCHAR)'0' && *p <= (W32_WCHAR)'9') {
            s->width = s->width * 10 + (*p - (W32_WCHAR)'0');
            p++;
        }
    }
    if (*p == (W32_WCHAR)'.') {
        p++;
        if (*p == (W32_WCHAR)'*') {
            s->star_p = 1;
            p++;
        } else {
            s->prec = 0;
            while (*p >= (W32_WCHAR)'0' && *p <= (W32_WCHAR)'9') {
                s->prec = s->prec * 10 + (*p - (W32_WCHAR)'0');
                p++;
            }
        }
    }
    if (*p == (W32_WCHAR)'I' && p[1] == (W32_WCHAR)'6' &&
        p[2] == (W32_WCHAR)'4') {
        s->i64 = 1;
        p += 3;
    }
    if (*p == (W32_WCHAR)'h') {
        s->half = 1;
        p++;
        if (*p == (W32_WCHAR)'h')
            p++;
    }
    if (*p == (W32_WCHAR)'l') {
        p++;
        if (*p == (W32_WCHAR)'l') {
            s->i64 = 1;
            p++;
        }
    }
    s->conv = *p;
    *pp = (s->conv == 0) ? p : p + 1;
    switch (s->conv) {
    case (W32_WCHAR)'d':
    case (W32_WCHAR)'i':
        s->consumes = 1;
        s->vkind = W32_WS_S64;
        break;
    case (W32_WCHAR)'u':
    case (W32_WCHAR)'x':
    case (W32_WCHAR)'X':
        s->consumes = 1;
        s->vkind = W32_WS_U64;
        break;
    case (W32_WCHAR)'p':
        s->consumes = 1;
        s->vkind = W32_WS_PTR;
        break;
    case (W32_WCHAR)'c':
        s->consumes = 1;
        s->vkind = W32_WS_WCHAR;
        break;
    case (W32_WCHAR)'C':
        s->consumes = 1;
        s->vkind = W32_WS_ACHAR;
        break;
    case (W32_WCHAR)'s':
        s->consumes = 1;
        s->vkind = W32_WS_WSTR;
        break;
    case (W32_WCHAR)'S':
        s->consumes = 1;
        s->vkind = W32_WS_ASTR;
        break;
    case (W32_WCHAR)'n':
        s->consumes = 1;
        s->vkind = W32_WS_INTPTR;
        break;
    default:
        s->consumes = 0;
        break;
    }
}

W32ABI W32_INT w32_wsprintf_core(W32_LPWSTR buf, W32_LPCWSTR fmt,
                                const struct w32_ws_arg *args, int nargs) {
    W32_LPWSTR w = buf;
    W32_LPCWSTR p = fmt;
    int ai = 0;

    if (!buf || !fmt)
        return 0;
    if (!args)
        nargs = 0;
    while (*p) {
        struct ws_spec s;
        int width;
        int prec;
        struct w32_ws_arg v;
        if (*p != (W32_WCHAR)'%') {
            *w++ = *p++;
            continue;
        }
        ws_parse_spec(&p, &s);
        if (s.conv == 0) {
            *w++ = (W32_WCHAR)'%';
            break;
        }
        if (s.conv == (W32_WCHAR)'%') {
            *w++ = (W32_WCHAR)'%';
            continue;
        }
        width = s.width;
        prec = s.prec;
        if (s.star_w) {
            width = 0;
            if (ai < nargs && args[ai].kind == W32_WS_S64)
                width = (int)(int64_t)args[ai].u;
            ai++;
            if (width < 0) {
                s.left = 1;
                width = -width;
            }
        }
        if (s.star_p) {
            prec = -1;
            if (ai < nargs && args[ai].kind == W32_WS_S64)
                prec = (int)(int64_t)args[ai].u;
            ai++;
            if (prec < -1)
                prec = -1;
        }
        if (!s.consumes) {
            *w++ = (W32_WCHAR)'%';
            *w++ = s.conv;
            continue;
        }
        v.kind = s.vkind;
        v.u = 0;
        v.p = NULL;
        if (ai < nargs && args[ai].kind == s.vkind)
            v = args[ai];
        ai++;
        switch (s.conv) {
        case (W32_WCHAR)'d':
        case (W32_WCHAR)'i': {
            int64_t sv = (int64_t)v.u;
            if (sv < 0)
                w = loc_put_u64(w, (uint64_t)(-sv), 10, 0, width, prec,
                    s.left, s.zero, 1);
            else
                w = loc_put_u64(w, (uint64_t)sv, 10, 0, width, prec,
                    s.left, s.zero, 0);
            break;
        }
        case (W32_WCHAR)'u':
        case (W32_WCHAR)'x':
        case (W32_WCHAR)'X': {
            int base = (s.conv == (W32_WCHAR)'u') ? 10 : 16;
            w = loc_put_u64(w, v.u, base, s.conv == (W32_WCHAR)'X', width,
                prec, s.left, s.zero, 0);
            break;
        }
        case (W32_WCHAR)'p': {
            *w++ = (W32_WCHAR)'0';
            *w++ = (W32_WCHAR)'x';
            w = loc_put_u64(w, v.u, 16, 0, (width > 2) ? width - 2 : 0, 8,
                s.left, 1, 0);
            break;
        }
        case (W32_WCHAR)'c': {
            int pad = (width > 1) ? width - 1 : 0;
            int i;
            if (!s.left) {
                for (i = 0; i < pad; i++)
                    *w++ = (W32_WCHAR)' ';
            }
            *w++ = (W32_WCHAR)(uint16_t)v.u;
            if (s.left) {
                for (i = 0; i < pad; i++)
                    *w++ = (W32_WCHAR)' ';
            }
            break;
        }
        case (W32_WCHAR)'C': {
            unsigned char b = (unsigned char)v.u;
            W32_WCHAR u = (b < 0x80) ? (W32_WCHAR)b : loc_1252_hi[b - 0x80];
            int pad = (width > 1) ? width - 1 : 0;
            int i;
            if (!s.left) {
                for (i = 0; i < pad; i++)
                    *w++ = (W32_WCHAR)' ';
            }
            *w++ = (u == 0xFFFFu) ? (W32_WCHAR)'?' : u;
            if (s.left) {
                for (i = 0; i < pad; i++)
                    *w++ = (W32_WCHAR)' ';
            }
            break;
        }
        case (W32_WCHAR)'s': {
            const W32_WCHAR *str = (const W32_WCHAR *)v.p;
            W32_INT sl = 0;
            int pad;
            int i;
            if (!str) {
                static const W32_WCHAR nullw[7] = { '(', 'n', 'u', 'l',
                    'l', ')', 0 };
                str = nullw;
            }
            while (str[sl] != 0)
                sl++;
            if (prec >= 0 && sl > prec)
                sl = prec;
            pad = (width > sl) ? width - sl : 0;
            if (!s.left) {
                for (i = 0; i < pad; i++)
                    *w++ = (W32_WCHAR)' ';
            }
            for (i = 0; i < sl; i++)
                *w++ = str[i];
            if (s.left) {
                for (i = 0; i < pad; i++)
                    *w++ = (W32_WCHAR)' ';
            }
            break;
        }
        case (W32_WCHAR)'S': {
            const char *str = (const char *)v.p;
            size_t sl;
            size_t done = 0;
            int pad;
            int i;
            if (!str)
                str = "(null)";
            sl = strlen(str);
            if (prec >= 0 && sl > (size_t)prec)
                sl = (size_t)prec;
            pad = (width > (int)sl) ? width - (int)sl : 0;
            if (!s.left) {
                for (i = 0; i < pad; i++)
                    *w++ = (W32_WCHAR)' ';
            }
            /* ACP bytes through the UTF-8 decoder, strict; bad bytes
             * become '?' so a hostile %S cannot break the line. */
            while (done < sl) {
                unsigned char c0 = (unsigned char)str[done];
                if (c0 < 0x80) {
                    *w++ = (W32_WCHAR)c0;
                    done++;
                } else {
                    size_t seqlen =
                        (c0 >= 0xF0) ? 4 : (c0 >= 0xE0) ? 3 : 2;
                    uint16_t units[2];
                    size_t need = 0;
                    int rc;
                    if (done + seqlen > sl)
                        seqlen = sl - done;
                    rc = w32_utf8_to_utf16(str + done, seqlen, units, 2,
                        &need);
                    if (rc == W32_UTF_OK && need >= 1 && need <= 2) {
                        size_t k;
                        for (k = 0; k < need; k++)
                            *w++ = units[k];
                        done += seqlen;
                    } else {
                        *w++ = (W32_WCHAR)'?';
                        done++;
                    }
                }
            }
            if (s.left) {
                for (i = 0; i < pad; i++)
                    *w++ = (W32_WCHAR)' ';
            }
            break;
        }
        case (W32_WCHAR)'n': {
            int *to = (int *)v.p;
            if (to)
                *to = (int)(w - buf);
            break;
        }
        default:
            break;              /* unreachable: consumes == 0 handled above */
        }
    }
    *w = 0;
    return (W32_INT)(w - buf);
}

/* The C shell: sysv varargs in, boxes out.  See the banner above for why
 * W32ABI is deliberately absent here. */
W32_INT wsprintfW(W32_LPWSTR buf, W32_LPCWSTR fmt, ...) {
    va_list ap;
    struct w32_ws_arg args[64];
    int nargs = 0;
    W32_LPCWSTR p;

    if (!buf || !fmt)
        return 0;
    va_start(ap, fmt);
    p = fmt;
    while (*p && nargs < 64) {
        struct ws_spec s;
        if (*p != (W32_WCHAR)'%') {
            p++;
            continue;
        }
        ws_parse_spec(&p, &s);
        if (s.conv == 0)
            break;
        if (s.conv == (W32_WCHAR)'%')
            continue;
        if (s.star_w && nargs < 64) {
            args[nargs].kind = W32_WS_S64;
            args[nargs].u = (uint64_t)(int64_t)va_arg(ap, int);
            args[nargs].p = NULL;
            nargs++;
        }
        if (s.star_p && nargs < 64) {
            args[nargs].kind = W32_WS_S64;
            args[nargs].u = (uint64_t)(int64_t)va_arg(ap, int);
            args[nargs].p = NULL;
            nargs++;
        }
        if (!s.consumes || nargs >= 64)
            continue;
        args[nargs].kind = s.vkind;
        args[nargs].u = 0;
        args[nargs].p = NULL;
        switch (s.vkind) {
        case W32_WS_S64:
            if (s.i64)
                args[nargs].u = (uint64_t)va_arg(ap, int64_t);
            else if (s.half)
                args[nargs].u =
                    (uint64_t)(int64_t)(int16_t)va_arg(ap, int);
            else
                args[nargs].u = (uint64_t)(int64_t)va_arg(ap, int);
            break;
        case W32_WS_U64:
            if (s.i64)
                args[nargs].u = va_arg(ap, uint64_t);
            else if (s.half)
                args[nargs].u = (uint64_t)(uint16_t)va_arg(ap, unsigned);
            else
                args[nargs].u = (uint64_t)va_arg(ap, unsigned);
            break;
        case W32_WS_WCHAR:
        case W32_WS_ACHAR:
            args[nargs].u = (uint64_t)(unsigned)va_arg(ap, int);
            break;
        case W32_WS_WSTR:
            args[nargs].p = va_arg(ap, const W32_WCHAR *);
            break;
        case W32_WS_ASTR:
            args[nargs].p = va_arg(ap, const char *);
            break;
        case W32_WS_PTR:
            args[nargs].u = (uint64_t)(uintptr_t)va_arg(ap, void *);
            break;
        case W32_WS_INTPTR:
            args[nargs].p = va_arg(ap, int *);
            break;
        default:
            break;
        }
        nargs++;
    }
    va_end(ap);
    return w32_wsprintf_core(buf, fmt, args, nargs);
}

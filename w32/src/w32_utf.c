/* w32_utf.c — UTF-16 <-> UTF-8, WIN32_PLAN.md phase W32-1.
 *
 * Strictness is the whole point of this file; see the header for why
 * substitution is refused.  The validity rules implemented here are the ones
 * from the Unicode standard's definition of well-formed UTF-8 (D92):
 *
 *   U+0000   .. U+007F    1 byte    0xxxxxxx
 *   U+0080   .. U+07FF    2 bytes   110xxxxx 10xxxxxx
 *   U+0800   .. U+FFFF    3 bytes   1110xxxx 10xxxxxx 10xxxxxx
 *   U+10000  .. U+10FFFF  4 bytes   11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
 *
 * with three extra refusals that a naive decoder gets wrong:
 *   - overlong forms (e.g. C0 80 for NUL), the classic path-check bypass;
 *   - surrogate code points U+D800..U+DFFF, which are not characters and
 *     cannot appear in UTF-8 at all;
 *   - anything above U+10FFFF, which UTF-16 cannot represent.
 */

#include "w32/w32_utf.h"

size_t w32_utf16_len(const uint16_t *s, size_t max) {
    size_t n = 0;
    if (!s) return 0;
    while (n < max && s[n] != 0) n++;
    return n;
}

/* Append one byte, counting even when there is nowhere to put it, so the
 * caller's *needed is right in the measuring and the too-small cases. */
static void emit(char *dst, size_t dstcap, size_t *pos, uint8_t b) {
    if (dst && *pos < dstcap) dst[*pos] = (char)b;
    (*pos)++;
}

int w32_utf16_to_utf8(const uint16_t *src, size_t srclen,
                      char *dst, size_t dstcap, size_t *needed) {
    size_t out = 0;

    if (needed) *needed = 0;
    if (!src && srclen) return W32_UTF_ERR_ARG;
    if (!dst && dstcap) return W32_UTF_ERR_ARG;

    for (size_t i = 0; i < srclen; i++) {
        uint32_t cp = src[i];

        if (cp >= W32_SUR_HIGH_FIRST && cp <= W32_SUR_HIGH_LAST) {
            /* High surrogate: must be followed by a low surrogate. */
            if (i + 1 >= srclen) return W32_UTF_ERR_TRUNCATED;
            uint32_t lo = src[i + 1];
            if (lo < W32_SUR_LOW_FIRST || lo > W32_SUR_LOW_LAST)
                return W32_UTF_ERR_ENCODING;
            cp = 0x10000u + ((cp - W32_SUR_HIGH_FIRST) << 10)
                          + (lo - W32_SUR_LOW_FIRST);
            i++;
        } else if (cp >= W32_SUR_LOW_FIRST && cp <= W32_SUR_LOW_LAST) {
            /* Low surrogate without a preceding high one. */
            return W32_UTF_ERR_ENCODING;
        }

        if (cp < 0x80u) {
            emit(dst, dstcap, &out, (uint8_t)cp);
        } else if (cp < 0x800u) {
            emit(dst, dstcap, &out, (uint8_t)(0xC0u | (cp >> 6)));
            emit(dst, dstcap, &out, (uint8_t)(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            emit(dst, dstcap, &out, (uint8_t)(0xE0u | (cp >> 12)));
            emit(dst, dstcap, &out, (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu)));
            emit(dst, dstcap, &out, (uint8_t)(0x80u | (cp & 0x3Fu)));
        } else {
            emit(dst, dstcap, &out, (uint8_t)(0xF0u | (cp >> 18)));
            emit(dst, dstcap, &out, (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu)));
            emit(dst, dstcap, &out, (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu)));
            emit(dst, dstcap, &out, (uint8_t)(0x80u | (cp & 0x3Fu)));
        }
    }

    if (needed) *needed = out;
    /* A measuring call (dst == NULL, dstcap == 0) is a success: the caller
     * asked how much space it needs and now knows.  Only a real buffer that
     * turned out to be too small is W32_UTF_ERR_SPACE. */
    if (!dst) return W32_UTF_OK;
    if (out > dstcap) return W32_UTF_ERR_SPACE;
    return W32_UTF_OK;
}

static void emit16(uint16_t *dst, size_t dstcap, size_t *pos, uint16_t u) {
    if (dst && *pos < dstcap) dst[*pos] = u;
    (*pos)++;
}

int w32_utf8_to_utf16(const char *src, size_t srclen,
                      uint16_t *dst, size_t dstcap, size_t *needed) {
    const uint8_t *p = (const uint8_t *)src;
    size_t out = 0;

    if (needed) *needed = 0;
    if (!src && srclen) return W32_UTF_ERR_ARG;
    if (!dst && dstcap) return W32_UTF_ERR_ARG;

    for (size_t i = 0; i < srclen; ) {
        uint8_t b0 = p[i];
        uint32_t cp;
        size_t extra;
        uint32_t lowest;   /* smallest code point this length may encode */

        if (b0 < 0x80u)              { cp = b0;          extra = 0; lowest = 0; }
        else if ((b0 & 0xE0u) == 0xC0u) { cp = b0 & 0x1Fu; extra = 1; lowest = 0x80u; }
        else if ((b0 & 0xF0u) == 0xE0u) { cp = b0 & 0x0Fu; extra = 2; lowest = 0x800u; }
        else if ((b0 & 0xF8u) == 0xF0u) { cp = b0 & 0x07u; extra = 3; lowest = 0x10000u; }
        else return W32_UTF_ERR_ENCODING;   /* 0x80..0xBF stray, or 0xF8+ */

        /* Need `extra` continuation bytes after i.  Written as a subtraction
         * on the remaining count so it cannot overflow for a huge srclen. */
        if (extra > srclen - i - 1) return W32_UTF_ERR_TRUNCATED;

        for (size_t k = 1; k <= extra; k++) {
            uint8_t bn = p[i + k];
            if ((bn & 0xC0u) != 0x80u) return W32_UTF_ERR_ENCODING;
            cp = (cp << 6) | (bn & 0x3Fu);
        }

        if (cp < lowest)                      return W32_UTF_ERR_ENCODING; /* overlong */
        if (cp > W32_UNI_MAX)                 return W32_UTF_ERR_ENCODING;
        if (cp >= W32_SUR_HIGH_FIRST && cp <= W32_SUR_LOW_LAST)
            return W32_UTF_ERR_ENCODING;      /* surrogate encoded in UTF-8 */

        if (cp < 0x10000u) {
            emit16(dst, dstcap, &out, (uint16_t)cp);
        } else {
            uint32_t v = cp - 0x10000u;
            emit16(dst, dstcap, &out, (uint16_t)(W32_SUR_HIGH_FIRST + (v >> 10)));
            emit16(dst, dstcap, &out, (uint16_t)(W32_SUR_LOW_FIRST + (v & 0x3FFu)));
        }
        i += extra + 1;
    }

    if (needed) *needed = out;
    if (!dst) return W32_UTF_OK;          /* measuring call; see above */
    if (out > dstcap) return W32_UTF_ERR_SPACE;
    return W32_UTF_OK;
}

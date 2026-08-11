/* w32_utf.h — UTF-16 <-> UTF-8 conversion for the w32 personality.
 *
 * WIN32_PLAN.md phase W32-1.
 *
 * Win32 stores strings as UTF-16 and AuraLite's GUI/libc take UTF-8, so every
 * `W` entry point crosses this boundary (see WIN32_PLAN.md D6).  The
 * conversion is therefore on the hot path of the whole personality and is
 * treated as attacker-facing: the input is whatever a foreign binary handed
 * us.
 *
 * Deliberate choices:
 *
 *  - UTF-16 is `uint16_t`, never `wchar_t`.  `wchar_t` is 32-bit on this
 *    toolchain and 16-bit under `-fshort-wchar`; pinning the core to a fixed
 *    type keeps that build flag from silently changing what the code means.
 *  - Lengths are explicit, never NUL-derived.  Callers with a NUL-terminated
 *    string use w32_utf16_len()/strlen() first.  Embedded NULs convert
 *    cleanly, because a PE resource string may legitimately contain one.
 *  - Malformed input is *rejected*, never substituted with U+FFFD.  A
 *    replacement character would silently turn a hostile filename into a
 *    different valid one, which is how path checks get bypassed.
 *  - The functions never write more than `dstcap` and always report the
 *    space they needed, so a caller can size a buffer with a first pass.
 */

#ifndef AURALITE_W32_UTF_H
#define AURALITE_W32_UTF_H

#include <stddef.h>
#include <stdint.h>

/* Return codes.  Negative on failure, W32_UTF_OK on success. */
#define W32_UTF_OK              0
#define W32_UTF_ERR_ARG        (-1)  /* NULL where a pointer was required */
#define W32_UTF_ERR_TRUNCATED  (-2)  /* input ends mid-sequence */
#define W32_UTF_ERR_ENCODING   (-3)  /* malformed: bad continuation, overlong,
                                      * lone/misordered surrogate, > U+10FFFF */
#define W32_UTF_ERR_SPACE      (-4)  /* dst too small; *needed is still set */

/* Unicode limits, named so the checks below read as intent. */
#define W32_UNI_MAX            0x10FFFFu
#define W32_SUR_HIGH_FIRST     0xD800u
#define W32_SUR_HIGH_LAST      0xDBFFu
#define W32_SUR_LOW_FIRST      0xDC00u
#define W32_SUR_LOW_LAST       0xDFFFu

/* Length of a NUL-terminated UTF-16 string, in code units, bounded by `max`
 * so an unterminated buffer cannot run away.  Returns `max` if no NUL is
 * found within it. */
size_t w32_utf16_len(const uint16_t *s, size_t max);

/* Convert UTF-16 -> UTF-8.
 *
 * `src`/`srclen` is in code units; `dst`/`dstcap` is in bytes.  On success
 * writes exactly *needed bytes (never NUL-terminated — the caller knows the
 * length).  `dst` may be NULL when `dstcap` is 0, to measure only; that is a
 * success returning W32_UTF_OK with *needed set, not an error.
 *
 * *needed is set whenever the input is well-formed, including the
 * W32_UTF_ERR_SPACE case, so a two-pass caller works. */
int w32_utf16_to_utf8(const uint16_t *src, size_t srclen,
                      char *dst, size_t dstcap, size_t *needed);

/* Convert UTF-8 -> UTF-16.  Mirror image of the above; `srclen` is bytes,
 * while `dstcap` and the reported `needed` are in code units. */
int w32_utf8_to_utf16(const char *src, size_t srclen,
                      uint16_t *dst, size_t dstcap, size_t *needed);

#endif /* AURALITE_W32_UTF_H */

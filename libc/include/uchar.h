#ifndef AURALITE_LIBC_UCHAR_H
#define AURALITE_LIBC_UCHAR_H

/*
 * uchar.h — POSIX.1-2024 <uchar.h>: 16/32-bit Unicode character types and
 * conversion functions.
 *
 * AuraLite's multibyte encoding is UTF-8 in the "C" locale.  The conversion
 * functions handle plain ASCII exactly and report EILSEQ for anything
 * requiring real multibyte decoding; full UTF-8 decoding is future work
 * (tracked alongside the rest of the locale subsystem).
 */

#include <stddef.h>
#include <stdint.h>

typedef uint_least16_t char16_t;
typedef uint_least32_t char32_t;

#ifndef AURALITE_MBSTATE_T_DEFINED
#define AURALITE_MBSTATE_T_DEFINED
typedef struct {
    unsigned int __count;
    unsigned int __value;
} mbstate_t;
#endif

size_t mbrtoc16(char16_t *pc16, const char *s, size_t n, mbstate_t *ps);
size_t c16rtomb(char *s, char16_t c16, mbstate_t *ps);
size_t mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps);
size_t c32rtomb(char *s, char32_t c32, mbstate_t *ps);

#endif /* AURALITE_LIBC_UCHAR_H */

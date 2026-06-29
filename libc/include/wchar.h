#ifndef AURALITE_LIBC_WCHAR_H
#define AURALITE_LIBC_WCHAR_H

#include <stddef.h>   /* wchar_t (compiler-provided) */
#include <stdint.h>

typedef uint32_t wint_t;

#define WEOF ((wint_t)-1)

size_t   wcslen(const wchar_t *s);
wchar_t *wcscpy(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wcscat(wchar_t *dst, const wchar_t *src);
int      wcscmp(const wchar_t *s1, const wchar_t *s2);
int      wcsncmp(const wchar_t *s1, const wchar_t *s2, size_t n);
size_t   mbstowcs(wchar_t *dest, const char *src, size_t n);
size_t   wcstombs(char *dest, const wchar_t *src, size_t n);
wint_t   btowc(int c);
int      wctob(wint_t c);

#endif /* AURALITE_LIBC_WCHAR_H */

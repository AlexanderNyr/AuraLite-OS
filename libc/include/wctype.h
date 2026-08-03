#ifndef AURALITE_LIBC_WCTYPE_H
#define AURALITE_LIBC_WCTYPE_H

/*
 * wctype.h — POSIX.1-2024 <wctype.h> for AuraLite user programs.
 *
 * AuraLite only ships the "C" locale, so wide-character classification is
 * simply ASCII classification widened to wint_t: iswalpha(c) is defined the
 * same as isalpha((int)c) for c < 128 and false otherwise.  Implementations
 * live in libc/src/compat.c.
 */

#include "wchar.h"   /* wint_t, WEOF */

typedef unsigned long wctype_t;
typedef unsigned long wctrans_t;

int iswalnum(wint_t c);
int iswalpha(wint_t c);
int iswblank(wint_t c);
int iswcntrl(wint_t c);
int iswdigit(wint_t c);
int iswgraph(wint_t c);
int iswlower(wint_t c);
int iswprint(wint_t c);
int iswpunct(wint_t c);
int iswspace(wint_t c);
int iswupper(wint_t c);
int iswxdigit(wint_t c);

wint_t towlower(wint_t c);
wint_t towupper(wint_t c);

/* C-locale-only stubs: no named wctype/wctrans classes beyond "the basics"
 * are supported, so these always report "unknown class". */
wctype_t  wctype(const char *property);
int       iswctype(wint_t c, wctype_t desc);
wctrans_t wctrans(const char *property);
wint_t    towctrans(wint_t c, wctrans_t desc);

#endif /* AURALITE_LIBC_WCTYPE_H */

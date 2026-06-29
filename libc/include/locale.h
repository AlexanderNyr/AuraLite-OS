#ifndef AURALITE_LIBC_LOCALE_H
#define AURALITE_LIBC_LOCALE_H

#include <stddef.h>

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5
#define LC_MESSAGES 6

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *currency_symbol;
};

char         *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

#endif /* AURALITE_LIBC_LOCALE_H */

#ifndef _LANGINFO_H
#define _LANGINFO_H

#include <locale.h>

#define CODESET    14
#define D_T_FMT     0
#define D_FMT       1
#define T_FMT       2
#define AM_STR      5
#define PM_STR      6
#define RADIXCHAR   7
#define THOUSEP     8
#define ABDAY_1    15
#define ABMON_1    33

char *nl_langinfo(int item);
char *nl_langinfo_l(int item, locale_t loc);

#endif /* _LANGINFO_H */

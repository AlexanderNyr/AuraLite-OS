#ifndef AURALITE_LIBC_STRINGS_H
#define AURALITE_LIBC_STRINGS_H

/*
 * strings.h — POSIX.1-2024 <strings.h>: legacy BSD string/memory functions
 * plus the case-insensitive comparisons and "find first set bit" helpers.
 * strcasecmp()/strncasecmp() already live in <string.h> (P10); this header
 * re-declares them per POSIX, which requires both headers to expose them.
 */

#include <string.h>

int   bcmp(const void *s1, const void *s2, size_t n);
void  bcopy(const void *src, void *dst, size_t n);
void  bzero(void *s, size_t n);
char *index(const char *s, int c);
char *rindex(const char *s, int c);

int ffs(int i);
int ffsl(long i);
int ffsll(long long i);

/* strcasecmp/strncasecmp are declared in <string.h> already; POSIX requires
 * <strings.h> to also make them visible, so no additional declaration is
 * needed here beyond the #include above. */

#endif /* AURALITE_LIBC_STRINGS_H */

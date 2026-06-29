#ifndef AURALITE_LIBC_STRING_H
#define AURALITE_LIBC_STRING_H

#include <stddef.h>

/* Freestanding string/memory functions for AuraLite OS user programs. */

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);

/* Tokenize a string (standard strtok semantics: modifies `s`). */
char  *strtok(char *s, const char *delim);

/* Extended string helpers (libc/src/string_extra.c). */
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strtok_r(char *str, const char *delim, char **saveptr);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);
int    strcasecmp(const char *s1, const char *s2);
int    strncasecmp(const char *s1, const char *s2, size_t n);

/* Map an errno value to a human-readable message (POSIX.1-2017 strerror()).
 * Returns a pointer to a static string; not required to be thread-safe and
 * may be overwritten by a subsequent strerror() call for unknown codes. */
char  *strerror(int errnum);

#endif /* AURALITE_LIBC_STRING_H */

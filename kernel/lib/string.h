#ifndef AURALITE_LIB_STRING_H
#define AURALITE_LIB_STRING_H

#include <stddef.h>
#include <stdint.h>

/* Freestanding subset of the C string/memory interface used by the kernel. */

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);

#endif /* AURALITE_LIB_STRING_H */

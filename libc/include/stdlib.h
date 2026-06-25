#ifndef AURALITE_LIBC_STDLIB_H
#define AURALITE_LIBC_STDLIB_H

#include <stddef.h>

/* Parse a string to int. */
int    atoi(const char *s);

/* Parse a string to long with optional base. */
long   strtol(const char *s, char **end, int base);

/* Simple pseudo-random number generator. */
void   srand(unsigned int seed);
int    rand(void);

void*  malloc(size_t size);
void   free(void* ptr);

#endif /* AURALITE_LIBC_STDLIB_H */

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

#endif /* AURALITE_LIBC_STDLIB_H */

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

/* Normal process termination (flushes nothing yet; calls _exit). */
void   exit(int status) __attribute__((noreturn));

/* Abnormal termination.  POSIX abort() raises SIGABRT; signals do not exist
 * until Phase P4, so for now it prints a diagnostic and _exit(134)s
 * (128 + SIGABRT=6).  assert() failures route through here. */
void   abort(void) __attribute__((noreturn));

/* Exit-status convenience macros. */
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#endif /* AURALITE_LIBC_STDLIB_H */

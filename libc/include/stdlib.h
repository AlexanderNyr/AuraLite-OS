#ifndef AURALITE_LIBC_STDLIB_H
#define AURALITE_LIBC_STDLIB_H

#include <stddef.h>

/* Parse a string to int. */
int    atoi(const char *s);

/* Parse a string to long with optional base. */
long               strtol(const char *s, char **end, int base);
unsigned long      strtoul(const char *s, char **end, int base);
long long          strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);

/* Floating-point parsers. */
double             strtod(const char *s, char **end);
float              strtof(const char *s, char **end);
long double        strtold(const char *s, char **end);
double             atof(const char *s);

/* Simple pseudo-random number generator. */
void   srand(unsigned int seed);
int    rand(void);

void*  malloc(size_t size);
void   free(void* ptr);
void*  calloc(size_t nmemb, size_t size);
void*  realloc(void *ptr, size_t size);

/* Normal process termination (flushes nothing yet; calls _exit). */
void   exit(int status) __attribute__((noreturn));

/* Abnormal termination.  POSIX abort() raises SIGABRT; signals do not exist
 * until Phase P4, so for now it prints a diagnostic and _exit(134)s
 * (128 + SIGABRT=6).  assert() failures route through here. */
void   abort(void) __attribute__((noreturn));

/* Exit-status convenience macros. */
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* ---- Environment (libc/src/env.c) ---- */
extern char **environ;
char  *getenv(const char *name);
int    setenv(const char *name, const char *value, int overwrite);
int    unsetenv(const char *name);
int    putenv(char *string);

/* ---- Sorting / searching / atexit (libc/src/stdlib_extra.c) ---- */
void   qsort(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *));
void  *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
               int (*compar)(const void *, const void *));
int    atexit(void (*func)(void));
void   __run_atexit(void);

#endif /* AURALITE_LIBC_STDLIB_H */

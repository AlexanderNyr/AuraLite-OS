#ifndef AURALITE_LIBC_STDIO_H
#define AURALITE_LIBC_STDIO_H

#include <stddef.h>

/* Minimal user-space stdio for AuraLite OS. */

/* Write a single character to stdout (fd 1). */
int putchar(int c);

/* Write a string + newline to stdout. */
int puts(const char *s);

/* Formatted output to stdout. Supports %s, %d, %u, %x, %c, %%. */
int printf(const char *fmt, ...);

/* Write "<s>: <strerror(errno)>\n" (or just "<strerror(errno)>\n" when s is
 * NULL/empty) to stderr (fd 2).  POSIX.1-2017 perror().  Does not modify the
 * observable value of errno on success. */
void perror(const char *s);

#endif /* AURALITE_LIBC_STDIO_H */

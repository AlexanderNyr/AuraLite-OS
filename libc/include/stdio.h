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

#endif /* AURALITE_LIBC_STDIO_H */

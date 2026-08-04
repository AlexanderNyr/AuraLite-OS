#ifndef _MONETARY_H
#define _MONETARY_H

#include <stddef.h>
#include <sys/types.h>   /* Q12: ssize_t; header must be self-contained */

ssize_t strfmon(char *s, size_t max, const char *format, ...);

#endif /* _MONETARY_H */

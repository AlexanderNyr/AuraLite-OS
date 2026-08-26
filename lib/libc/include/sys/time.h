#ifndef AURALITE_LIBC_SYS_TIME_H
#define AURALITE_LIBC_SYS_TIME_H

/* SELFHOST SH1: the POSIX <sys/time.h> spelling, which tcc's tcc.h
 * includes unconditionally on non-Windows hosts.
 *
 * This libc keeps `struct timeval` and the time functions in <time.h>;
 * this header is a thin self-contained wrapper so that compiler sources
 * (and any POSIX code that says <sys/time.h>) get the same declarations
 * without carrying a second definition of the struct.
 */

#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

int gettimeofday(struct timeval *tv, void *tz);

#ifdef __cplusplus
}
#endif

#endif /* AURALITE_LIBC_SYS_TIME_H */

#ifndef AURALITE_LIBC_ISO646_H
#define AURALITE_LIBC_ISO646_H

/*
 * iso646.h — POSIX.1-2024 <iso646.h>: alternative spellings of the C
 * operators (C95/Amendment 1).  No-op in C++, where these are keywords.
 */

#ifndef __cplusplus

#define and    &&
#define and_eq &=
#define bitand &
#define bitor  |
#define compl  ~
#define not    !
#define not_eq !=
#define or     ||
#define or_eq  |=
#define xor    ^
#define xor_eq ^=

#endif /* __cplusplus */

#endif /* AURALITE_LIBC_ISO646_H */

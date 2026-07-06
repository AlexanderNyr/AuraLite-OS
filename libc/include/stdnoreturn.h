#ifndef AURALITE_LIBC_STDNORETURN_H
#define AURALITE_LIBC_STDNORETURN_H

/*
 * stdnoreturn.h — POSIX.1-2024 <stdnoreturn.h> (C11 noreturn convenience
 * macro).  `noreturn`/`_Noreturn` were folded into the `[[noreturn]]`
 * attribute model in C23; guard against redefinition on newer toolchains.
 */

#if !defined(__cplusplus) && \
    (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
#define noreturn _Noreturn
#define __noreturn_is_defined 1
#endif

#endif /* AURALITE_LIBC_STDNORETURN_H */

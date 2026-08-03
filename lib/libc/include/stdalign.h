#ifndef AURALITE_LIBC_STDALIGN_H
#define AURALITE_LIBC_STDALIGN_H

/*
 * stdalign.h — POSIX.1-2024 <stdalign.h> (C11 alignment convenience macros).
 * `alignas`/`alignof` became keywords in C23; guard against redefinition
 * when building with a C23-or-later toolchain.
 */

#ifndef __cplusplus
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#define alignas _Alignas
#define alignof _Alignof
#endif
#endif

#define __alignas_is_defined 1
#define __alignof_is_defined 1

#endif /* AURALITE_LIBC_STDALIGN_H */

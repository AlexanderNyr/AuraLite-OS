#ifndef AURALITE_LIBC_STDDEF_H
#define AURALITE_LIBC_STDDEF_H

/*
 * stddef.h — POSIX.1-2024 <stddef.h> for AuraLite user programs.
 *
 * AuraLite headers have historically relied on the compiler's own bundled
 * <stddef.h> (Clang and GCC both ship a self-contained, freestanding-safe
 * version that defines size_t/ptrdiff_t/wchar_t/NULL/offsetof from compiler
 * builtins with no host libc dependency).  This thin wrapper keeps that
 * behaviour while giving AuraLite a stable, versionable header of its own
 * in libc/include, matching the rest of the tree's header layout.
 */

#include_next <stddef.h>

/* Provide max_align_t if the underlying compiler header did not (older
 * freestanding <stddef.h> variants only guarantee C11 features when the
 * compiler is invoked with a new-enough -std=). */
#ifndef __CLANG_MAX_ALIGN_T_DEFINED
#ifndef _GCC_MAX_ALIGN_T
#ifndef AURALITE_MAX_ALIGN_T_DEFINED
#define AURALITE_MAX_ALIGN_T_DEFINED
typedef struct {
    long long __ll;
    long double __ld;
} max_align_t;
#endif
#endif
#endif

#endif /* AURALITE_LIBC_STDDEF_H */

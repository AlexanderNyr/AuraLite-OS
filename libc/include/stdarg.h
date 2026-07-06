#ifndef AURALITE_LIBC_STDARG_H
#define AURALITE_LIBC_STDARG_H

/*
 * stdarg.h — POSIX.1-2024 <stdarg.h> for AuraLite user programs.
 *
 * The compiler provides __builtin_va_* intrinsics for variadic argument
 * access; this header just exposes them under the standard names. Both
 * Clang (cross-compiled kernel/user code) and the host GCC used to build
 * host-side unit tests implement these builtins identically.
 */

typedef __builtin_va_list va_list;

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)
#define va_copy(d, s)      __builtin_va_copy(d, s)

#endif /* AURALITE_LIBC_STDARG_H */

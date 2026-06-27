/*
 * assert.h — C standard diagnostics for AuraLite user programs.
 *
 * Per the C standard this header has no include guard for its functional part:
 * the definition of assert() is re-evaluated on every inclusion according to
 * the current state of NDEBUG.
 */

#include <stddef.h>   /* for the helper prototype's types if needed later */

/* The failure handler lives in libc.c; it prints a diagnostic and abort()s. */
#ifndef AURALITE_LIBC_ASSERT_FAIL_DECLARED
#define AURALITE_LIBC_ASSERT_FAIL_DECLARED
void __assert_fail(const char *expr, const char *file, int line,
                   const char *func) __attribute__((noreturn));
#endif

#undef assert

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
    ((expr) ? (void)0 \
            : __assert_fail(#expr, __FILE__, __LINE__, __func__))
#endif

#ifndef AURALITE_LIBC_STDATOMIC_H
#define AURALITE_LIBC_STDATOMIC_H

/*
 * stdatomic.h — POSIX.1-2024 <stdatomic.h> (C11 atomics).
 *
 * Implemented directly on top of compiler atomic builtins, which lower to
 * lock-free x86_64 instructions (LOCK CMPXCHG/XADD/...) for every type used
 * here.  No runtime support code is needed; everything below is either a
 * typedef or a macro.
 *
 * Clang requires the `__c11_atomic_*` builtin family for operands of
 * `_Atomic`-qualified type (its plain `__atomic_*` builtins reject them),
 * while GCC's `__atomic_*` builtins accept `_Atomic`-qualified operands
 * directly.  We branch on `__clang__` to pick the right builtin family for
 * each of AuraLite's two supported compilers (Clang for kernel/user
 * cross-builds, GCC for host-side unit tests).
 */

#include <stddef.h>
#include <stdint.h>

#define ATOMIC_BOOL_LOCK_FREE     2
#define ATOMIC_CHAR_LOCK_FREE     2
#define ATOMIC_CHAR16_T_LOCK_FREE 2
#define ATOMIC_CHAR32_T_LOCK_FREE 2
#define ATOMIC_WCHAR_T_LOCK_FREE  2
#define ATOMIC_SHORT_LOCK_FREE    2
#define ATOMIC_INT_LOCK_FREE      2
#define ATOMIC_LONG_LOCK_FREE     2
#define ATOMIC_LLONG_LOCK_FREE    2
#define ATOMIC_POINTER_LOCK_FREE  2

typedef enum {
    memory_order_relaxed = __ATOMIC_RELAXED,
    memory_order_consume = __ATOMIC_CONSUME,
    memory_order_acquire = __ATOMIC_ACQUIRE,
    memory_order_release = __ATOMIC_RELEASE,
    memory_order_acq_rel = __ATOMIC_ACQ_REL,
    memory_order_seq_cst = __ATOMIC_SEQ_CST
} memory_order;

typedef _Atomic _Bool               atomic_bool;
typedef _Atomic char                atomic_char;
typedef _Atomic signed char         atomic_schar;
typedef _Atomic unsigned char       atomic_uchar;
typedef _Atomic short               atomic_short;
typedef _Atomic unsigned short      atomic_ushort;
typedef _Atomic int                 atomic_int;
typedef _Atomic unsigned int        atomic_uint;
typedef _Atomic long                atomic_long;
typedef _Atomic unsigned long       atomic_ulong;
typedef _Atomic long long           atomic_llong;
typedef _Atomic unsigned long long  atomic_ullong;
typedef _Atomic intptr_t            atomic_intptr_t;
typedef _Atomic uintptr_t           atomic_uintptr_t;
typedef _Atomic size_t              atomic_size_t;
typedef _Atomic ptrdiff_t           atomic_ptrdiff_t;
typedef _Atomic intmax_t            atomic_intmax_t;
typedef _Atomic uintmax_t           atomic_uintmax_t;

typedef struct {
    _Atomic _Bool __val;
} atomic_flag;

#define ATOMIC_FLAG_INIT { 0 }
#define ATOMIC_VAR_INIT(value) (value)

#define kill_dependency(y) (y)

#ifdef __clang__

#define atomic_init(obj, value) __c11_atomic_init((obj), (value))

#define atomic_thread_fence(order) __c11_atomic_thread_fence(order)
#define atomic_signal_fence(order) __c11_atomic_signal_fence(order)

#define atomic_is_lock_free(obj) __c11_atomic_is_lock_free(sizeof(*(obj)))

#define atomic_store(obj, val)               __c11_atomic_store((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_store_explicit(obj, val, mo)  __c11_atomic_store((obj), (val), (mo))

#define atomic_load(obj)              __c11_atomic_load((obj), __ATOMIC_SEQ_CST)
#define atomic_load_explicit(obj, mo) __c11_atomic_load((obj), (mo))

#define atomic_exchange(obj, val)               __c11_atomic_exchange((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_exchange_explicit(obj, val, mo)  __c11_atomic_exchange((obj), (val), (mo))

#define atomic_compare_exchange_strong(obj, exp, des) \
    __c11_atomic_compare_exchange_strong((obj), (exp), (des), __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_strong_explicit(obj, exp, des, s, f) \
    __c11_atomic_compare_exchange_strong((obj), (exp), (des), (s), (f))
#define atomic_compare_exchange_weak(obj, exp, des) \
    __c11_atomic_compare_exchange_weak((obj), (exp), (des), __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_weak_explicit(obj, exp, des, s, f) \
    __c11_atomic_compare_exchange_weak((obj), (exp), (des), (s), (f))

#define atomic_fetch_add(obj, val)              __c11_atomic_fetch_add((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_fetch_add_explicit(obj, val, mo)  __c11_atomic_fetch_add((obj), (val), (mo))
#define atomic_fetch_sub(obj, val)              __c11_atomic_fetch_sub((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_fetch_sub_explicit(obj, val, mo)  __c11_atomic_fetch_sub((obj), (val), (mo))
#define atomic_fetch_or(obj, val)               __c11_atomic_fetch_or((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_fetch_or_explicit(obj, val, mo)   __c11_atomic_fetch_or((obj), (val), (mo))
#define atomic_fetch_xor(obj, val)               __c11_atomic_fetch_xor((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_fetch_xor_explicit(obj, val, mo)  __c11_atomic_fetch_xor((obj), (val), (mo))
#define atomic_fetch_and(obj, val)               __c11_atomic_fetch_and((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_fetch_and_explicit(obj, val, mo)  __c11_atomic_fetch_and((obj), (val), (mo))

#define atomic_flag_test_and_set(flag) \
    __c11_atomic_exchange(&(flag)->__val, 1, __ATOMIC_SEQ_CST)
#define atomic_flag_test_and_set_explicit(flag, mo) \
    __c11_atomic_exchange(&(flag)->__val, 1, (mo))
#define atomic_flag_clear(flag) \
    __c11_atomic_store(&(flag)->__val, 0, __ATOMIC_SEQ_CST)
#define atomic_flag_clear_explicit(flag, mo) \
    __c11_atomic_store(&(flag)->__val, 0, (mo))

#else /* GCC: __atomic_* builtins accept _Atomic-qualified operands directly. */

#define atomic_init(obj, value) (*(obj) = (value))

#define atomic_thread_fence(order) __atomic_thread_fence(order)
#define atomic_signal_fence(order) __atomic_signal_fence(order)

#define atomic_is_lock_free(obj) __atomic_is_lock_free(sizeof(*(obj)), (obj))

#define atomic_store(obj, val)               __atomic_store_n((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_store_explicit(obj, val, mo)  __atomic_store_n((obj), (val), (mo))

#define atomic_load(obj)              __atomic_load_n((obj), __ATOMIC_SEQ_CST)
#define atomic_load_explicit(obj, mo) __atomic_load_n((obj), (mo))

#define atomic_exchange(obj, val)               __atomic_exchange_n((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_exchange_explicit(obj, val, mo)  __atomic_exchange_n((obj), (val), (mo))

#define atomic_compare_exchange_strong(obj, exp, des) \
    __atomic_compare_exchange_n((obj), (exp), (des), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_strong_explicit(obj, exp, des, s, f) \
    __atomic_compare_exchange_n((obj), (exp), (des), 0, (s), (f))
#define atomic_compare_exchange_weak(obj, exp, des) \
    __atomic_compare_exchange_n((obj), (exp), (des), 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_weak_explicit(obj, exp, des, s, f) \
    __atomic_compare_exchange_n((obj), (exp), (des), 1, (s), (f))

#define atomic_fetch_add(obj, val)              __atomic_fetch_add((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_fetch_add_explicit(obj, val, mo)  __atomic_fetch_add((obj), (val), (mo))
#define atomic_fetch_sub(obj, val)              __atomic_fetch_sub((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_fetch_sub_explicit(obj, val, mo)  __atomic_fetch_sub((obj), (val), (mo))
#define atomic_fetch_or(obj, val)               __atomic_fetch_or((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_fetch_or_explicit(obj, val, mo)   __atomic_fetch_or((obj), (val), (mo))
#define atomic_fetch_xor(obj, val)               __atomic_fetch_xor((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_fetch_xor_explicit(obj, val, mo)  __atomic_fetch_xor((obj), (val), (mo))
#define atomic_fetch_and(obj, val)               __atomic_fetch_and((obj), (val), __ATOMIC_SEQ_CST)
#define atomic_fetch_and_explicit(obj, val, mo)  __atomic_fetch_and((obj), (val), (mo))

#define atomic_flag_test_and_set(flag) \
    __atomic_test_and_set(&(flag)->__val, __ATOMIC_SEQ_CST)
#define atomic_flag_test_and_set_explicit(flag, mo) \
    __atomic_test_and_set(&(flag)->__val, (mo))
#define atomic_flag_clear(flag) \
    __atomic_clear(&(flag)->__val, __ATOMIC_SEQ_CST)
#define atomic_flag_clear_explicit(flag, mo) \
    __atomic_clear(&(flag)->__val, (mo))

#endif /* __clang__ */

#endif /* AURALITE_LIBC_STDATOMIC_H */

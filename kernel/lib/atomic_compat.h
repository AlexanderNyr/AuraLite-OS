/* atomic_compat.h — the legacy __sync_* builtins, spelled for two compilers.
 *
 * SELFHOST_PLAN.md SH5c: tcc (mob 2ba12e8) implements the __atomic_* family
 * as builtins but has NO __sync_* builtins at all — a __sync_fetch_and_add
 * compiles as an implicit external call (width-blind, and undefined at link
 * time).  The kernel's queue-claim protocol (thread.h: on_queue via
 * lock_test_and_set / lock_release) and the TID/refcount counters are spelled
 * with the legacy names; clang inlines them to lock-prefixed RMWs.
 *
 * This header re-spells the __sync_* forms the tree uses as macros onto the
 * __atomic_* builtins tcc has natively.  Guarded by __TINYC__, so the
 * clang/gcc build is untouched to the byte; the macro keeps each call site's
 * real pointee type, so the atomic width is per-site correct (a C shim
 * function could not be — the __sync_* symbols carry no width).
 *
 * Barrier mapping (gcc's documented model):
 *   __sync_fetch/add/sub_and_fetch : seq_cst  -> __atomic_*_fetch / __atomic_fetch_*
 *   __sync_lock_test_and_set       : acquire  -> __atomic_exchange (xchg on x86)
 *   __sync_lock_release            : release  -> __atomic_store_n(ptr, 0, RELEASE)
 */
#ifndef KERNEL_LIB_ATOMIC_COMPAT_H
#define KERNEL_LIB_ATOMIC_COMPAT_H

#include <stdatomic.h>   /* __ATOMIC_* orders: builtins in clang/gcc, macros in tcc */

#ifdef __TINYC__

#define __sync_add_and_fetch(p, v)     __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define __sync_sub_and_fetch(p, v)     __atomic_sub_fetch((p), (v), __ATOMIC_SEQ_CST)
#define __sync_fetch_and_add(p, v)     __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define __sync_fetch_and_sub(p, v)     __atomic_fetch_sub((p), (v), __ATOMIC_SEQ_CST)

/* tcc's <stdatomic.h> has __atomic_store_n/__atomic_load_n macros but no
 * __atomic_exchange_n; the 4-argument __atomic_exchange (result through the
 * third argument) is a builtin both tcc and clang/gcc implement. */
#define __sync_lock_test_and_set(p, v)                                    \
    ({ __typeof__(*(p)) __lts_v = (v), __lts_old;                         \
       __atomic_exchange((p), &__lts_v, &__lts_old, __ATOMIC_ACQ_REL);    \
       __lts_old; })

#define __sync_lock_release(p)         __atomic_store_n((p), 0, __ATOMIC_RELEASE)

#endif /* __TINYC__ */

#endif /* KERNEL_LIB_ATOMIC_COMPAT_H */

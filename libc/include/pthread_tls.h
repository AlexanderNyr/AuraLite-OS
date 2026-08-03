#ifndef AURALITE_LIBC_PTHREAD_TLS_H
#define AURALITE_LIBC_PTHREAD_TLS_H

/* pthread_tls.h — the per-thread control block shared by the pthread
 * clone path and libc's TLS-backed errno (FIX_R3).
 *
 * NOT a POSIX header: it lives in libc/include only because libc's
 * translation units resolve quoted includes from there.  User programs
 * must not include it; nothing here is API.
 *
 * Layout/update invariant: FS.base == the block itself, so reading %fs:0
 * yields `self`, which always points back at the block (musl-style).
 * The kernel makes FS.base equal the current thread's tls_base at every
 * context switch, at every arch_prctl, and at every first user-mode entry
 * (clone/fork/exec), so the block below is usable from the very first
 * instruction a thread runs in user mode.
 *
 * Who owns the block:
 *   - pthread children: placed at the top of the thread's own stack and
 *     passed to clone() via CLONE_SETTLS (libc/src/pthread/pthread.c);
 *   - the MAIN thread: the static __main_tcb inside libc, installed by
 *     __libc_start_main before main() runs (libc/src/libc.c), so even
 *     non-threaded programs get a per-thread errno and the pre-main
 *     fallback global is only used in the crt0 -> start_main window.
 */

#include <stdint.h>
#include <stddef.h>

struct pthread_tcb {
    struct pthread_tcb *self;          /* %fs:0 must point to itself */
    int                 errno_cell;    /* FIX_R3: this thread's errno   */
    void *(*start_routine)(void *);
    void  *arg;
    void  *retval;
    void  *stack_base;                 /* mmap base, for cleanup        */
    size_t stack_size;
    volatile int tid_futex;            /* CLONE_CHILD_CLEARTID target   */
};

static inline struct pthread_tcb *ptls_self(void) {
    struct pthread_tcb *p;
    __asm__ volatile ("mov %%fs:0, %0" : "=r"(p));
    return p;
}

/* arch_prctl codes (Linux-compatible values; the kernel implements these
 * two).  SYS_ARCH_PRCTL itself is 158; libc callers pass it numerically. */
#define PTLS_ARCH_SET_FS 0x1002
#define PTLS_ARCH_GET_FS 0x1003

#endif /* AURALITE_LIBC_PTHREAD_TLS_H */

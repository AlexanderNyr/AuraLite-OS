/* libc/src/pthread/barrier.c — pthread_barrier_* (Phase Q6) */

#include <pthread.h>
#include <errno.h>
#include <unistd.h>

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a,
                         unsigned n) {
    (void)a;
    if (n == 0) return EINVAL;
    b->threshold = (int)n;
    b->count = 0;
    b->phase = 0;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *b) {
    (void)b;
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *b) {
    int phase = __atomic_load_n(&b->phase, __ATOMIC_ACQUIRE);
    int n = __atomic_fetch_add(&b->count, 1, __ATOMIC_ACQ_REL) + 1;
    if (n == b->threshold) {
        b->count = 0;
        __atomic_fetch_add(&b->phase, 1, __ATOMIC_RELEASE);
        /* Wake all waiters. */
        syscall(530, (uint64_t)(uintptr_t)&b->phase,
                1 /*FUTEX_WAKE*/, 0x7fffffff, 0, 0, 0);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    /* Wait until phase changes. */
    while (__atomic_load_n(&b->phase, __ATOMIC_ACQUIRE) == phase)
        syscall(530, (uint64_t)(uintptr_t)&b->phase,
                0 /*FUTEX_WAIT*/, (uint64_t)phase, 0, 0, 0);
    return 0;
}

int pthread_barrierattr_init(pthread_barrierattr_t *a) {
    a->pshared = 0;
    return 0;
}

int pthread_barrierattr_destroy(pthread_barrierattr_t *a) {
    (void)a;
    return 0;
}

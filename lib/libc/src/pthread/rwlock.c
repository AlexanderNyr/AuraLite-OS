/* libc/src/pthread/rwlock.c — pthread_rwlock_* (Phase Q6) */

#include <pthread.h>
#include <errno.h>
#include <unistd.h>

/* Futex syscall (SYS_FUTEX = 530). */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

static long _futex(volatile int *u, int op, int val) {
    return syscall(530, (uint64_t)(uintptr_t)u, (uint64_t)op, (uint64_t)val, 0, 0, 0);
}

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a) {
    (void)a;
    l->state = 0;
    l->waiters = 0;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *l) {
    (void)l;
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *l) {
    for (;;) {
        int s = __atomic_load_n(&l->state, __ATOMIC_ACQUIRE);
        if (s >= 0 &&
            __atomic_compare_exchange_n(&l->state, &s, s + 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 0;
        _futex(&l->state, FUTEX_WAIT, s);
    }
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *l) {
    int s = __atomic_load_n(&l->state, __ATOMIC_ACQUIRE);
    if (s < 0) return EBUSY;
    return __atomic_compare_exchange_n(&l->state, &s, s + 1, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)
           ? 0 : EBUSY;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *l) {
    int zero = 0;
    while (!__atomic_compare_exchange_n(&l->state, &zero, -1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        _futex(&l->state, FUTEX_WAIT, zero);
        zero = 0;
    }
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *l) {
    int zero = 0;
    return __atomic_compare_exchange_n(&l->state, &zero, -1, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)
           ? 0 : EBUSY;
}

int pthread_rwlock_unlock(pthread_rwlock_t *l) {
    int old = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
    if (old == -1)
        __atomic_store_n(&l->state, 0, __ATOMIC_RELEASE);
    else
        __atomic_fetch_sub(&l->state, 1, __ATOMIC_RELEASE);
    _futex(&l->state, FUTEX_WAKE, 0x7fffffff);
    return 0;
}

int pthread_rwlockattr_init(pthread_rwlockattr_t *a) {
    a->pshared = 0;
    return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a) {
    (void)a;
    return 0;
}

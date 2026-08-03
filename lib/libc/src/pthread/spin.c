/* libc/src/pthread/spin.c — pthread_spin_* (Phase Q6) */

#include <pthread.h>
#include <errno.h>

int pthread_spin_init(pthread_spinlock_t *s, int pshared) {
    (void)pshared;
    s->lock = 0;
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *s) {
    (void)s;
    return 0;
}

int pthread_spin_lock(pthread_spinlock_t *s) {
    while (__atomic_test_and_set(&s->lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *s) {
    return __atomic_test_and_set(&s->lock, __ATOMIC_ACQUIRE) ? EBUSY : 0;
}

int pthread_spin_unlock(pthread_spinlock_t *s) {
    __atomic_clear(&s->lock, __ATOMIC_RELEASE);
    return 0;
}

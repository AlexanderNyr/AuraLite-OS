#ifndef _PTHREAD_H
#define _PTHREAD_H

#include <sys/types.h>
#include <stdint.h>

typedef uint64_t pthread_t;
typedef struct { uint32_t flags; void *stackaddr; size_t stacksize; } pthread_attr_t;

typedef struct {
    int lock;
    uint32_t owner;
    int recursive;
} pthread_mutex_t;

typedef struct { int pshared; } pthread_mutexattr_t;

typedef struct {
    uint32_t state;
    uint32_t waiters;
} pthread_cond_t;

typedef struct { int pshared; } pthread_condattr_t;

typedef uint32_t pthread_key_t;
typedef struct { int done; volatile int lock; } pthread_once_t;

#define PTHREAD_MUTEX_INITIALIZER {0,0,0}
#define PTHREAD_COND_INITIALIZER {0,0}

/* ---- Q6: Reader-writer lock ---- */
typedef struct {
    volatile int state;   /* 0=unlocked, >0=n readers, -1=writer */
    volatile int waiters;
} pthread_rwlock_t;

typedef struct { int pshared; } pthread_rwlockattr_t;

#define PTHREAD_RWLOCK_INITIALIZER {0, 0}

/* ---- Q6: Barrier ---- */
typedef struct {
    int threshold;
    volatile int count;
    volatile int phase;
} pthread_barrier_t;

typedef struct { int pshared; } pthread_barrierattr_t;

#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

/* ---- Q6: Spinlock ---- */
typedef struct { volatile int lock; } pthread_spinlock_t;

/* ---- Q6: Cancellation ---- */
#define PTHREAD_CANCEL_ENABLE       0
#define PTHREAD_CANCEL_DISABLE      1
#define PTHREAD_CANCEL_DEFERRED     0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED            ((void *)-1)

/* ---- Q6: Detach state ---- */
#define PTHREAD_CREATE_JOINABLE     0
#define PTHREAD_CREATE_DETACHED     1

/* ---- Existing functions ---- */
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
void pthread_exit(void *retval);
pthread_t pthread_self(void);

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void));

#define PTHREAD_ONCE_INIT {0, 0}

/* ---- Q6: Reader-writer lock functions ---- */
int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a);
int pthread_rwlock_destroy(pthread_rwlock_t *l);
int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l);
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
int pthread_rwlock_trywrlock(pthread_rwlock_t *l);
int pthread_rwlock_unlock(pthread_rwlock_t *l);
int pthread_rwlockattr_init(pthread_rwlockattr_t *a);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a);

/* ---- Q6: Barrier functions ---- */
int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned n);
int pthread_barrier_destroy(pthread_barrier_t *b);
int pthread_barrier_wait(pthread_barrier_t *b);
int pthread_barrierattr_init(pthread_barrierattr_t *a);
int pthread_barrierattr_destroy(pthread_barrierattr_t *a);

/* ---- Q6: Spinlock functions ---- */
int pthread_spin_init(pthread_spinlock_t *s, int pshared);
int pthread_spin_destroy(pthread_spinlock_t *s);
int pthread_spin_lock(pthread_spinlock_t *s);
int pthread_spin_trylock(pthread_spinlock_t *s);
int pthread_spin_unlock(pthread_spinlock_t *s);

/* ---- Q6: Cancellation ---- */
int pthread_cancel(pthread_t thread);
int pthread_setcancelstate(int state, int *oldstate);
int pthread_setcanceltype(int type, int *oldtype);
void pthread_testcancel(void);

/* ---- Q6: Extended attributes ---- */
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size);
int pthread_attr_setstack(pthread_attr_t *a, void *stackaddr, size_t stacksize);
int pthread_attr_getstack(const pthread_attr_t *a, void **stackaddr, size_t *stacksize);
int pthread_attr_setdetachstate(pthread_attr_t *a, int detachstate);
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *detachstate);

/* ---- Q6: Cleanup push/pop macros ---- */
#define pthread_cleanup_push(fn, arg) \
    do { void (*_cf)(void *) = (fn); void *_ca = (arg);
#define pthread_cleanup_pop(execute) \
        if (execute) _cf(_ca); } while (0)

#endif /* _PTHREAD_H */

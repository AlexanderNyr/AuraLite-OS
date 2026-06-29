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

#endif /* _PTHREAD_H */
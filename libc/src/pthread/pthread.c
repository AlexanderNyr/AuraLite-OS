/* libc/src/pthread/pthread.c — minimal pthread implementation (P9) */

#include "pthread.h"
#include "unistd.h"
#include "sys/mman.h"
#include "string.h"
#include "errno.h"

#define CLONE_VM            0x00000100
#define CLONE_FS            0x00000200
#define CLONE_FILES         0x00000400
#define CLONE_SIGHAND       0x00000800
#define CLONE_THREAD        0x00010000
#define CLONE_SETTLS        0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000

extern long syscall(long num, ...);

static pthread_key_t next_key = 1;
static void *key_destructor[64];

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    (void)attr;

    void *stack = mmap(NULL, 8*1024*1024, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return -1;

    uint64_t *stack_top = (uint64_t *)((char *)stack + 8*1024*1024);
    *--stack_top = (uint64_t)arg;
    *--stack_top = (uint64_t)start_routine;

    uint64_t tls = (uint64_t)stack; /* simple TLS pointer */

    long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                 CLONE_THREAD | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;

    long tid = syscall(56, flags, (uint64_t)stack_top, 0, 0, tls);
    if (tid < 0) {
        munmap(stack, 8*1024*1024);
        return -1;
    }

    if (thread) *thread = (pthread_t)tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    (void)retval;
    /* Real join uses futex on clear_tid_addr — simplified for now */
    while (1) {
        sched_yield();
    }
    return 0;
}

void pthread_exit(void *retval) {
    (void)retval;
    _exit(0);
}

pthread_t pthread_self(void) {
    return (pthread_t)getpid();
}

/* ---- mutex ---- */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void)a;
    m->lock = 0;
    m->owner = 0;
    m->recursive = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    while (__sync_lock_test_and_set(&m->lock, 1)) {
        syscall(530, (uint64_t)&m->lock, 0, 1, 0, 0, 0); /* FUTEX_WAIT */
    }
    m->owner = (uint32_t)pthread_self();
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    m->lock = 0;
    syscall(530, (uint64_t)&m->lock, 1, 1, 0, 0, 0); /* FUTEX_WAKE */
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (__sync_lock_test_and_set(&m->lock, 1) == 0) {
        m->owner = (uint32_t)pthread_self();
        return 0;
    }
    return EBUSY;
}

/* ---- cond ---- */
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    (void)a;
    c->state = 0;
    c->waiters = 0;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    pthread_mutex_unlock(m);
    syscall(530, (uint64_t)&c->state, 0, 1, 0, 0, 0); /* FUTEX_WAIT */
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c) {
    syscall(530, (uint64_t)&c->state, 1, 1, 0, 0, 0);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    syscall(530, (uint64_t)&c->state, 0x7FFFFFFF, 0x7FFFFFFF, 0, 0, 0);
    return 0;
}

/* ---- key ---- */
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    *key = next_key++;
    if (*key < 64) key_destructor[*key] = (void *)destructor;
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    /* TLS implementation omitted for brevity — return NULL */
    return NULL;
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    (void)key; (void)value;
    return 0;
}

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void)) {
    if (__sync_val_compare_and_swap(once_control, 0, 1) == 0) {
        init_routine();
    }
    return 0;
}
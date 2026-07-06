#ifndef AURALITE_LIBC_THREADS_H
#define AURALITE_LIBC_THREADS_H

/*
 * threads.h — POSIX.1-2024 <threads.h> (C11 threads), implemented as a thin
 * wrapper over AuraLite's pthreads (libc/src/pthread/pthread.c).
 *
 * thrd_yield() has no dedicated syscall yet (sched_yield() lands with
 * Phase Q8); until then it performs a zero-duration nanosleep(), which the
 * kernel's scheduler treats as "give up the rest of this timeslice".
 */

#include <pthread.h>
#include <time.h>

typedef pthread_t       thrd_t;
typedef pthread_mutex_t mtx_t;
typedef pthread_cond_t  cnd_t;
typedef pthread_key_t   tss_t;

typedef int (*thrd_start_t)(void *);
typedef void (*tss_dtor_t)(void *);

enum {
    thrd_success = 0,
    thrd_nomem   = 1,
    thrd_timedout = 2,
    thrd_busy    = 3,
    thrd_error   = 4
};

enum {
    mtx_plain     = 0,
    mtx_recursive = 1,
    mtx_timed     = 2
};

/* ---- Thread management ---- */
int    thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
int    thrd_join(thrd_t thr, int *res);
void   thrd_exit(int res) __attribute__((__noreturn__));
int    thrd_detach(thrd_t thr);
int    thrd_equal(thrd_t a, thrd_t b);
thrd_t thrd_current(void);
void   thrd_yield(void);
int    thrd_sleep(const struct timespec *duration, struct timespec *remaining);

/* ---- Mutexes ---- */
int  mtx_init(mtx_t *mtx, int type);
void mtx_destroy(mtx_t *mtx);
int  mtx_lock(mtx_t *mtx);
int  mtx_trylock(mtx_t *mtx);
int  mtx_timedlock(mtx_t *mtx, const struct timespec *ts);
int  mtx_unlock(mtx_t *mtx);

/* ---- Condition variables ---- */
int  cnd_init(cnd_t *cond);
void cnd_destroy(cnd_t *cond);
int  cnd_signal(cnd_t *cond);
int  cnd_broadcast(cnd_t *cond);
int  cnd_wait(cnd_t *cond, mtx_t *mtx);
int  cnd_timedwait(cnd_t *cond, mtx_t *mtx, const struct timespec *ts);

/* ---- Thread-specific storage ---- */
int   tss_create(tss_t *key, tss_dtor_t dtor);
void  tss_delete(tss_t key);
void *tss_get(tss_t key);
int   tss_set(tss_t key, void *val);

/* ---- One-time initialization ---- */
typedef pthread_once_t once_flag;
#define ONCE_FLAG_INIT PTHREAD_ONCE_INIT

void call_once(once_flag *flag, void (*func)(void));

#endif /* AURALITE_LIBC_THREADS_H */

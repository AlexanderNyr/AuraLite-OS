/* libc/src/pthread/pthread.c — pthread runtime (P9)
 *
 * Threading model on AuraLite: SYS_CLONE (56) creates a task that shares the
 * caller's address space and resumes in user mode at the clone call site with
 * RAX == 0 (child) or RAX == tid (parent) — analogous to fork().  Because the
 * child resumes on a *fresh* user stack, it cannot see the parent's local
 * variables, so we hand the thread's (start_routine, arg) to the child through
 * its TLS base: the child reads them back via %fs and then runs the routine.
 */

#include "pthread.h"
#include "unistd.h"
#include "sys/mman.h"
#include "string.h"
#include "errno.h"
#include "stdlib.h"

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000

#define PTHREAD_STACK_SIZE (1024 * 1024)   /* 1 MiB per thread */

/* syscall() is declared in <unistd.h> with a fixed 7-argument prototype. */

/* Per-thread control block, pointed to by FS.base (%fs:0 == self). */
struct pthread_tcb {
    struct pthread_tcb *self;          /* %fs:0 must point to itself */
    void *(*start_routine)(void *);
    void  *arg;
    void  *retval;
    void  *stack_base;                 /* mmap base, for cleanup            */
    size_t stack_size;
    volatile int tid_futex;            /* CLONE_CHILD_CLEARTID target       */
};

static pthread_key_t next_key = 1;
static void *key_destructor[64];

static inline struct pthread_tcb *tcb_self(void) {
    struct pthread_tcb *p;
    __asm__ volatile ("mov %%fs:0, %0" : "=r"(p));
    return p;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    (void)attr;

    void *stack = mmap(NULL, PTHREAD_STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED)
        return EAGAIN;

    /* Place the TCB at the top of the new thread's stack region. */
    char *top = (char *)stack + PTHREAD_STACK_SIZE;
    top -= sizeof(struct pthread_tcb);
    top = (char *)((uintptr_t)top & ~0xFULL);   /* 16-byte align */
    struct pthread_tcb *tcb = (struct pthread_tcb *)top;
    tcb->self          = tcb;
    tcb->start_routine = start_routine;
    tcb->arg           = arg;
    tcb->retval        = NULL;
    tcb->stack_base    = stack;
    tcb->stack_size    = PTHREAD_STACK_SIZE;
    tcb->tid_futex     = 0;

    /* New stack pointer grows down from just below the TCB, 16-byte aligned. */
    uint64_t child_sp = ((uint64_t)tcb) & ~0xFULL;

    long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                 CLONE_THREAD | CLONE_SETTLS | CLONE_CHILD_CLEARTID;

    long tid = (long)syscall(56, (uint64_t)flags, child_sp,
                             /*ptid*/ 0,
                             /*ctid*/ (uint64_t)&tcb->tid_futex,
                             /*tls */ (uint64_t)tcb, 0);

    if (tid == 0) {
        /* ---- child ---- runs the user routine, then exits the thread. */
        struct pthread_tcb *me = tcb_self();
        void *rv = me->start_routine(me->arg);
        me->retval = rv;
        pthread_exit(rv);
        /* not reached */
        for (;;) { }
    }

    if (tid < 0) {
        munmap(stack, PTHREAD_STACK_SIZE);
        return EAGAIN;
    }

    if (thread) *thread = (pthread_t)tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    (void)thread;
    /* Without a thread registry keyed by pthread_t we cannot recover the
     * joined thread's TCB here; a full implementation would look it up and
     * FUTEX_WAIT on its tid_futex until CLONE_CHILD_CLEARTID zeroes it.
     * For now, spin-wait cooperatively so the joiner makes progress. */
    if (retval) *retval = NULL;
    for (volatile int i = 0; i < 1000000; i++) { }
    return 0;
}

void pthread_exit(void *retval) {
    (void)retval;
    /* SYS_EXIT terminates this task; the kernel performs CLONE_CHILD_CLEARTID
     * (zero *ctid + FUTEX_WAKE) so a joiner can observe completion. */
    _exit(0);
}

pthread_t pthread_self(void) {
    return (pthread_t)getpid();
}

/* ---- mutex (futex-backed) ---- */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void)a;
    m->lock = 0;
    m->owner = 0;
    m->recursive = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }

int pthread_mutex_lock(pthread_mutex_t *m) {
    while (__sync_lock_test_and_set(&m->lock, 1)) {
        syscall(530, (uint64_t)&m->lock, 0 /*FUTEX_WAIT*/, 1, 0, 0, 0);
    }
    m->owner = (uint32_t)pthread_self();
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    __sync_lock_release(&m->lock);
    syscall(530, (uint64_t)&m->lock, 1 /*FUTEX_WAKE*/, 1, 0, 0, 0);
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

int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    uint32_t seq = c->state;
    pthread_mutex_unlock(m);
    syscall(530, (uint64_t)&c->state, 0 /*FUTEX_WAIT*/, seq, 0, 0, 0);
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c) {
    __sync_fetch_and_add(&c->state, 1);
    syscall(530, (uint64_t)&c->state, 1 /*FUTEX_WAKE*/, 1, 0, 0, 0);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    __sync_fetch_and_add(&c->state, 1);
    syscall(530, (uint64_t)&c->state, 1 /*FUTEX_WAKE*/, 0x7FFFFFFF, 0, 0, 0);
    return 0;
}

/* ---- key ---- */
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    *key = next_key++;
    if (*key < 64) key_destructor[*key] = (void *)destructor;
    return 0;
}

int pthread_key_delete(pthread_key_t key) { (void)key; return 0; }

void *pthread_getspecific(pthread_key_t key) { (void)key; return NULL; }

int pthread_setspecific(pthread_key_t key, const void *value) {
    (void)key; (void)value;
    return 0;
}

/* ---- once ---- */
int pthread_once(pthread_once_t *once_control, void (*init_routine)(void)) {
    if (__sync_val_compare_and_swap(&once_control->done, 0, 1) == 0) {
        init_routine();
    }
    return 0;
}

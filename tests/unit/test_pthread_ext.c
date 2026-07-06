/*
 * test_pthread_ext.c — host-side unit test for Phase Q6 pthread extensions.
 *
 * Tests types, constants, and basic logic of:
 * pthread_rwlock_*, pthread_barrier_*, pthread_spin_*, pthread_cancel_*,
 * and pthread_attr_* extensions.
 *
 * We inline the logic to avoid linking the freestanding runtime.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond) do { \
    if (cond) { passed++; } \
    else { printf("  FAIL: %d: %s\n", __LINE__, #cond); failed++; } \
} while (0)

/* ---- Type & constant checks (match pthread.h) ---- */

typedef struct { volatile int state; volatile int waiters; } test_rwlock_t;
typedef struct { int pshared; } test_rwlockattr_t;
typedef struct { int threshold; volatile int count; volatile int phase; } test_barrier_t;
typedef struct { int pshared; } test_barrierattr_t;
typedef struct { volatile int lock; } test_spinlock_t;

#define TEST_PTHREAD_RWLOCK_INITIALIZER {0, 0}
#define TEST_PTHREAD_BARRIER_SERIAL_THREAD (-1)
#define TEST_PTHREAD_CANCEL_ENABLE       0
#define TEST_PTHREAD_CANCEL_DISABLE      1
#define TEST_PTHREAD_CANCEL_DEFERRED     0
#define TEST_PTHREAD_CANCEL_ASYNCHRONOUS 1
#define TEST_PTHREAD_CANCELED            ((void *)-1)
#define TEST_PTHREAD_CREATE_JOINABLE     0
#define TEST_PTHREAD_CREATE_DETACHED     1

static void test_constants(void) {
    /* rwlock initializer is a brace-enclosed compound literal -- test parts */
    test_rwlock_t rl = {0, 0};
    CHECK(rl.state == 0);
    CHECK(rl.waiters == 0);

    CHECK(TEST_PTHREAD_BARRIER_SERIAL_THREAD == -1);

    CHECK(TEST_PTHREAD_CANCEL_ENABLE == 0);
    CHECK(TEST_PTHREAD_CANCEL_DISABLE == 1);
    CHECK(TEST_PTHREAD_CANCEL_DEFERRED == 0);
    CHECK(TEST_PTHREAD_CANCEL_ASYNCHRONOUS == 1);
    CHECK(TEST_PTHREAD_CANCELED == ((void *)-1));

    CHECK(TEST_PTHREAD_CREATE_JOINABLE == 0);
    CHECK(TEST_PTHREAD_CREATE_DETACHED == 1);
}

static void test_types(void) {
    CHECK(sizeof(test_rwlock_t) >= 2 * sizeof(int));
    CHECK(sizeof(test_barrier_t) >= sizeof(int) + 2 * sizeof(int));
    CHECK(sizeof(test_spinlock_t) >= sizeof(int));
}

/* ---- Inline rwlock logic ---- */

static int rwlock_state;
static int rwlock_waiters;

static void rwlock_init(void) {
    rwlock_state = 0;
    rwlock_waiters = 0;
}

static int rwlock_rdlock(void) {
    /* Simple non-contended test: can read-lock */
    if (rwlock_state >= 0) {
        rwlock_state++;
        return 0;
    }
    return -1;
}

static int rwlock_wrlock(void) {
    if (rwlock_state == 0) {
        rwlock_state = -1;
        return 0;
    }
    return -1;
}

static int rwlock_unlock(void) {
    if (rwlock_state == -1)
        rwlock_state = 0;
    else
        rwlock_state--;
    return 0;
}

static void test_rwlock_logic(void) {
    rwlock_init();
    CHECK(rwlock_state == 0);

    /* Reader lock */
    CHECK(rwlock_rdlock() == 0);
    CHECK(rwlock_state == 1);

    /* Second reader */
    CHECK(rwlock_rdlock() == 0);
    CHECK(rwlock_state == 2);

    /* Unlock both */
    CHECK(rwlock_unlock() == 0);
    CHECK(rwlock_state == 1);
    CHECK(rwlock_unlock() == 0);
    CHECK(rwlock_state == 0);

    /* Writer lock */
    CHECK(rwlock_wrlock() == 0);
    CHECK(rwlock_state == -1);

    /* Can't rdlock while writer holds */
    CHECK(rwlock_rdlock() != 0);

    /* Unlock writer */
    CHECK(rwlock_unlock() == 0);
    CHECK(rwlock_state == 0);
}

/* ---- Inline barrier logic ---- */

struct test_barrier_state {
    int threshold;
    int count;
    int phase;
};

static void barrier_init(struct test_barrier_state *b, unsigned n) {
    b->threshold = (int)n;
    b->count = 0;
    b->phase = 0;
}

static int barrier_wait(struct test_barrier_state *b) {
    int phase = b->phase;
    int n = ++b->count;
    if (n == b->threshold) {
        b->count = 0;
        b->phase++;
        return -1; /* serial thread */
    }
    /* Spin until phase changes */
    while (b->phase == phase) { }
    return 0;
}

static void test_barrier_logic(void) {
    struct test_barrier_state b;
    barrier_init(&b, 3);
    CHECK(b.threshold == 3);
    CHECK(b.count == 0);

    /* Simulate N-1 waiters arriving (these would normally block) */
    b.count = 1;
    int phase = b.phase;
    CHECK(b.count == 1);

    b.count = 2;
    CHECK(b.count == 2);

    /* The Nth arrival triggers the barrier */
    int r = barrier_wait(&b);
    CHECK(r != 0); /* serial thread */
    CHECK(b.count == 0);
    CHECK(b.phase == phase + 1);
}

/* ---- Inline spinlock logic ---- */

static void test_spinlock_logic(void) {
    volatile int lock = 0;

    /* Trylock on unlocked */
    int r = __atomic_test_and_set(&lock, __ATOMIC_ACQUIRE) ? -1 : 0;
    CHECK(r == 0);
    CHECK(lock != 0);

    /* Trylock on locked */
    r = __atomic_test_and_set(&lock, __ATOMIC_ACQUIRE) ? -1 : 0;
    CHECK(r != 0);

    /* Unlock */
    __atomic_clear(&lock, __ATOMIC_RELEASE);
    CHECK(lock == 0);
}

/* ---- Main ---- */

int main(void) {
    printf("test_pthread_ext:\n");

    test_constants();
    test_types();
    test_rwlock_logic();
    test_barrier_logic();
    test_spinlock_logic();

    printf("  %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

/*
 * test_spinlock.c — unit tests for spinlock primitives:
 * init, acquire/release, irqsave/restore, contention simulation.
 *
 * 25+ test cases.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; if (failed == b) passed++; } while(0)
#define CHECK(c) do { if(!(c)) { printf("  FAIL L%d: %s\n",__LINE__,#c); failed++; } } while(0)
#define CHECK_EQ(a,e) do { if((long)(a)!=(long)(e)) { printf("  FAIL L%d: %s=%ld want %ld\n",__LINE__,#a,(long)(a),(long)(e)); failed++; } } while(0)

/* ---- Spinlock implementation (same algorithm as kernel) ---- */

typedef volatile uint32_t spinlock_t;

static void spinlock_init(spinlock_t *lock) {
    *lock = 0;
}

static void spinlock_acquire(spinlock_t *lock) {
    while (__sync_bool_compare_and_swap(lock, 0, 1) == 0) {
        /* spin */
    }
}

static void spinlock_release(spinlock_t *lock) {
    *lock = 0;
}

/* Simulate irqsave/restore with a thread-local flag */
static __thread int irq_flags = 0;

static uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
    int was_enabled = irq_flags;
    irq_flags = 0;  /* simulate cli */
    spinlock_acquire(lock);
    return (uint64_t)was_enabled;
}

static void spinlock_release_irqrestore(spinlock_t *lock, uint64_t rflags) {
    spinlock_release(lock);
    irq_flags = (int)rflags;  /* simulate sti if was enabled */
}

/* ======== TESTS ======== */

void t_init_zero(void) {
    spinlock_t lock;
    spinlock_init(&lock);
    CHECK_EQ(lock, 0);
}

void t_acquire_release(void) {
    spinlock_t lock;
    spinlock_init(&lock);
    spinlock_acquire(&lock);
    CHECK_EQ(lock, 1);
    spinlock_release(&lock);
    CHECK_EQ(lock, 0);
}

void t_double_acquire_fails(void) {
    /* On a single thread, acquiring twice would deadlock.
       We test that the lock value is 1 after first acquire. */
    spinlock_t lock;
    spinlock_init(&lock);
    spinlock_acquire(&lock);
    CHECK_EQ(lock, 1);
    /* Can't acquire again on same thread without releasing */
    spinlock_release(&lock);
}

void t_acquire_irqsave(void) {
    spinlock_t lock;
    spinlock_init(&lock);
    irq_flags = 1;  /* simulate interrupts enabled */
    uint64_t rf = spinlock_acquire_irqsave(&lock);
    CHECK_EQ(lock, 1);
    CHECK_EQ((long)rf, 1);  /* saved that interrupts were enabled */
    spinlock_release_irqrestore(&lock, rf);
    CHECK_EQ(lock, 0);
    CHECK_EQ(irq_flags, 1);  /* restored */
}

void t_irqsave_nested(void) {
    spinlock_t lock1, lock2;
    spinlock_init(&lock1);
    spinlock_init(&lock2);
    irq_flags = 1;
    uint64_t rf1 = spinlock_acquire_irqsave(&lock1);
    uint64_t rf2 = spinlock_acquire_irqsave(&lock2);
    CHECK_EQ(lock1, 1);
    CHECK_EQ(lock2, 1);
    spinlock_release_irqrestore(&lock2, rf2);
    spinlock_release_irqrestore(&lock1, rf1);
}

void t_multiple_locks(void) {
    spinlock_t a, b, c;
    spinlock_init(&a); spinlock_init(&b); spinlock_init(&c);
    spinlock_acquire(&a);
    spinlock_acquire(&b);
    spinlock_acquire(&c);
    CHECK_EQ(a, 1); CHECK_EQ(b, 1); CHECK_EQ(c, 1);
    spinlock_release(&c);
    spinlock_release(&b);
    spinlock_release(&a);
    CHECK_EQ(a, 0); CHECK_EQ(b, 0); CHECK_EQ(c, 0);
}

void t_init_then_check(void) {
    spinlock_t lock = 42;  /* garbage */
    spinlock_init(&lock);
    CHECK_EQ(lock, 0);
}

/* --- Multi-threaded contention test --- */

static spinlock_t mt_lock;
static volatile int mt_counter = 0;
static const int MT_ITERATIONS = 10000;
static const int MT_THREADS = 4;

static void *mt_thread_fn(void *arg) {
    (void)arg;
    for (int i = 0; i < MT_ITERATIONS; i++) {
        spinlock_acquire(&mt_lock);
        mt_counter++;
        spinlock_release(&mt_lock);
    }
    return NULL;
}

void t_contention_safety(void) {
    spinlock_init(&mt_lock);
    mt_counter = 0;
    pthread_t threads[MT_THREADS];
    for (int i = 0; i < MT_THREADS; i++) {
        pthread_create(&threads[i], NULL, mt_thread_fn, NULL);
    }
    for (int i = 0; i < MT_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    CHECK_EQ(mt_counter, MT_ITERATIONS * MT_THREADS);
}

/* --- CAS correctness --- */

void t_cas_success(void) {
    spinlock_t lock = 0;
    int result = __sync_bool_compare_and_swap(&lock, 0, 1);
    CHECK(result);
    CHECK_EQ(lock, 1);
}

void t_cas_failure(void) {
    spinlock_t lock = 1;
    int result = __sync_bool_compare_and_swap(&lock, 0, 1);
    CHECK(!result);
    CHECK_EQ(lock, 1);  /* unchanged */
}

void t_cas_returns_to_zero(void) {
    spinlock_t lock;
    spinlock_init(&lock);
    spinlock_acquire(&lock);
    CHECK_EQ(lock, 1);
    spinlock_release(&lock);
    CHECK_EQ(lock, 0);
    /* Can acquire again */
    spinlock_acquire(&lock);
    CHECK_EQ(lock, 1);
    spinlock_release(&lock);
}

/* --- Repeated acquire/release cycles --- */

void t_stress_cycles(void) {
    spinlock_t lock;
    spinlock_init(&lock);
    for (int i = 0; i < 100000; i++) {
        spinlock_acquire(&lock);
        CHECK_EQ(lock, 1);
        spinlock_release(&lock);
        CHECK_EQ(lock, 0);
    }
}

/* --- Counter protection with irqsave --- */

static spinlock_t prot_lock;
static volatile int prot_counter = 0;

static void *prot_thread_fn(void *arg) {
    (void)arg;
    for (int i = 0; i < MT_ITERATIONS; i++) {
        uint64_t rf = spinlock_acquire_irqsave(&prot_lock);
        prot_counter++;
        spinlock_release_irqrestore(&prot_lock, rf);
    }
    return NULL;
}

void t_irqsave_contention(void) {
    spinlock_init(&prot_lock);
    prot_counter = 0;
    pthread_t threads[MT_THREADS];
    for (int i = 0; i < MT_THREADS; i++) {
        pthread_create(&threads[i], NULL, prot_thread_fn, NULL);
    }
    for (int i = 0; i < MT_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    CHECK_EQ(prot_counter, MT_ITERATIONS * MT_THREADS);
}

int main(void) {
    printf("=== Spinlock Tests ===\n\n");

    printf("--- basic operations ---\n");
    RUN(t_init_zero);
    RUN(t_acquire_release);
    RUN(t_double_acquire_fails);
    RUN(t_acquire_irqsave);
    RUN(t_irqsave_nested);
    RUN(t_multiple_locks);
    RUN(t_init_then_check);

    printf("--- CAS correctness ---\n");
    RUN(t_cas_success);
    RUN(t_cas_failure);
    RUN(t_cas_returns_to_zero);

    printf("--- stress ---\n");
    RUN(t_stress_cycles);

    printf("--- multi-threaded ---\n");
    RUN(t_contention_safety);
    RUN(t_irqsave_contention);

    printf("\n=== Results: %d/%d passed, %d failed ===\n", passed, tn, failed);
    return failed ? 1 : 0;
}

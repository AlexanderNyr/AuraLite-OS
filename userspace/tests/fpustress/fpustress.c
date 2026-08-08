/* fpustress.c — M1 (MATURITY_PLAN.md) FPU/SSE context-switch regression test.
 *
 * The H8 scheduler is real SMP, but until M1 the context switch saved no
 * FPU/SSE state, so a thread resumed on another CPU continued its floating-
 * point computation with that CPU's stale xmm registers.  The visible
 * symptom was gltest's 373 FP checks failing randomly under -smp 2.
 *
 * This program forces the failure directly: four threads each keep four
 * double accumulators live in xmm registers across hundreds of preemptions,
 * with a DISTINCT base per thread so that cross-contamination produces a
 * wrong value rather than a coincidentally-right one.  Each thread computes
 * its result with the SAME noinline compute() the parent uses for the
 * reference, so the two are bit-identical unless a context switch lost FP
 * state -- which is exactly what M1 fixes.
 *
 * Prints "FPSTRESS PASS" / "FPSTRESS FAIL" to serial for the integration
 * gate test_fpu_smp.sh.
 */

#include "stdio.h"
#include "unistd.h"
#include "pthread.h"

#define NTHREADS 4
#define ITERS    200000          /* ~enough iterations to span many preemptions */

typedef struct {
    double base;
    double result;
} arg_t;

/*
 * Four interleaved accumulators kept live across the loop so the compiler
 * holds them in xmm registers (the workload is deliberately not reducible to
 * a closed form).  noinline guarantees the worker and the reference both run
 * the same compiled body, making the comparison bit-exact under correct FP
 * context switching.
 */
static double compute(double base) __attribute__((noinline));
static double compute(double base) {
    double a = 0.0, b = 0.0, c = 0.0, d = 0.0;
    for (int i = 1; i <= ITERS; i++) {
        double x = base + (double)i;
        a += 1.0 / (1.0 * x);
        b += 1.0 / (2.0 * x);
        c += 1.0 / (3.0 * x);
        d += 1.0 / (4.0 * x);
    }
    return a + b + c + d;
}

static void *worker(void *p) {
    arg_t *a = (arg_t *)p;
    a->result = compute(a->base);
    return NULL;
}

int main(void) {
    pthread_t th[NTHREADS];
    arg_t     args[NTHREADS];

    for (int i = 0; i < NTHREADS; i++) {
        args[i].base   = 10.0 * (double)(i + 1);   /* distinct per thread */
        args[i].result = 0.0;
    }

    printf("[fpustress] spawning %d FP-heavy threads...\n", NTHREADS);
    fflush(stdout);

    for (int i = 0; i < NTHREADS; i++) {
        if (pthread_create(&th[i], NULL, worker, &args[i]) != 0) {
            printf("[fpustress] pthread_create %d FAILED\n", i);
            printf("FPSTRESS FAIL\n");
            return 1;
        }
    }
    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(th[i], NULL);
    }

    int fails = 0;
    for (int i = 0; i < NTHREADS; i++) {
        double ref = compute(args[i].base);
        double got = args[i].result;
        /*
         * Reference and worker run the SAME function, so under correct FP
         * context switching they are bit-identical.  A missing FPU switch
         * injects another thread's partial sums, producing errors many
         * orders of magnitude larger than this epsilon; a tiny tolerance
         * absorbs any last-ULP compiler difference between the concurrent
         * and sequential calls without masking corruption.
         */
        double diff = got - ref;
        if (diff < 0.0) diff = -diff;
        double scale = ref > 1.0 ? ref : 1.0;
        if (diff > 1e-9 * scale) {
            /* libc printf has no %f/%g yet, so report the raw IEEE-754 bits. */
            union { double d; unsigned long long u; } gr, rr;
            gr.d = got; rr.d = ref;
            printf("[fpustress] thread %d MISMATCH got=0x%016llx ref=0x%016llx\n",
                   i, gr.u, rr.u);
            fails++;
        } else {
            printf("[fpustress] thread %d ok (base=%d)\n", i, (int)args[i].base);
        }
    }

    printf("[fpustress] %d/%d threads correct\n", NTHREADS - fails, NTHREADS);
    fflush(stdout);
    if (fails == 0) {
        printf("FPSTRESS PASS\n");
        return 0;
    }
    printf("FPSTRESS FAIL\n");
    return 1;
}

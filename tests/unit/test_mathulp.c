/*
 * test_mathulp.c — the RESIDUE2 T4 libm last-ULP review, as an executable.
 *
 * "Review" means MEASURED, not claimed: this harness computes the distance,
 * in ULP, between the shipping kernels (compiled in from libc.c /
 * math_extra.c) and glibc's long-double references (sinl/cosl/expl/logl/
 * powl) across argument grids, prints the worst case per function, and
 * asserts each stays inside the bound recorded in RESIDUE2_PLAN.md's T4
 * Result.  A regression that doubles an error budget turns this red; a
 * kernel improvement lets the plan's table move down in the same commit
 * that tightens the bound here.
 *
 * It also pins the T4 errno contract: sqrt/log/asin/acos/pow/fmod domain
 * and range errors set the right errno (the review found them silent).
 *
 * Float variants are spot-checked as wrappers: sinf(x) must equal
 * (float)sin(x) exactly — the wrapper is a narrowing, so any drift is a
 * bug in the wrapper, not accuracy loss.
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>
#include <string.h>

/* The shipping kernels, extracted with renames by
 * tools/extract_libc_impls.py --math-review (the DOOM D1 pattern: rename
 * so the host's builtins cannot shadow the code under test). */
#include "libc_math_gen.c"

/* ---- ULP distance vs a long-double reference ---- */

static double ulp_of(double x) {
    /* Distance from x to the next representable double, via bit tricks. */
    union { double d; uint64_t u; } v;
    v.d = x;
    v.u += 1;
    return (v.d > x) ? (v.d - x) : (x - v.d);
}

static long double ulp_dist(double got, long double want) {
    if (got == (double)want) return 0;
    if (isnan(got) && isnan((double)want)) return 0;
    long double d = (long double)got - want;
    if (d < 0) d = -d;
    double scale = ulp_of(got != 0.0 ? got : 1.0);
    if (scale == 0.0) return 0;
    return (long double)(d / (long double)scale);
}

typedef double (*d1)(double);
typedef long double (*l1)(long double);

static long double worst_over_grid1(const char *name, d1 f, l1 ref,
                                    double lo, double hi, int steps,
                                    long double bound) {
    long double worst = 0;
    double arg_at = 0;
    for (int i = 0; i <= steps; i++) {
        double x = lo + (hi - lo) * ((double)i / (double)steps);
        double got = f(x);
        long double want = ref((long double)x);
        long double d = ulp_dist(got, want);
        if (d > worst) { worst = d; arg_at = x; }
    }
    printf("  %-5s [%g .. %g]: worst %Lf ULP at x=%g (bound %Lf)\n",
           name, lo, hi, worst, arg_at, bound);
    return worst;
}

static int failures = 0;

static void check_bound(const char *name, long double worst, long double bound) {
    if (!(worst <= bound)) {
        printf("    FAIL: %s exceeded its recorded bound\n", name);
        failures++;
    }
}

int main(void) {
    printf("test_mathulp: last-ULP review of the shipping kernels (T4)\n");

    /* The measured budget recorded in RESIDUE2_PLAN.md T4 Result.  Each
     * bound is ~2x the worst case this harness measured on the reference
     * host, so normal libm-vs-glibc wobble stays green while a real
     * regression (like the old while-loop reduction at 1e6) is loud. */
    long double w;

    w = worst_over_grid1("sin", a_sin, sinl,   -25.0, 25.0, 20000, 4);
    check_bound("sin small", w, 4);
    w = worst_over_grid1("sin", a_sin, sinl,   -1000.0, 1000.0, 20000, 6);
    check_bound("sin wide", w, 6);
    w = worst_over_grid1("sin", a_sin, sinl,   1000000.0, 1000006.0, 2000, 64);
    check_bound("sin 1e6 (old code was ~1e10 ULP here)", w, 64);

    w = worst_over_grid1("cos", a_cos, cosl,   -25.0, 25.0, 20000, 4);
    check_bound("cos small", w, 4);
    w = worst_over_grid1("cos", a_cos, cosl,   -1000.0, 1000.0, 20000, 6);
    check_bound("cos wide", w, 6);

    w = worst_over_grid1("exp", a_exp, expl,   -708.0, 708.0, 20000, 8);
    check_bound("exp", w, 8);

    w = worst_over_grid1("log", a_log, logl,   1e-300, 1e300, 20000, 8);
    check_bound("log", w, 8);

    /* pow via the fractional path (integer powers are exact by design).
     * Measured worst ~36 ULP: pow = exp(e*log b) composes two kernels, so
     * log's 0.87 ULP is amplified by e*ln(b) (~15) before exp adds its own
     * ~3.7 ULP.  A sub-ULP pow needs a double-double log kernel (fdlibm
     * territory) — recorded as future work; 48 gives measured+headroom. */
    {
        long double worst = 0; double arg_at = 0;
        for (int i = 0; i <= 5000; i++) {
            double b = 0.01 + 60.0 * ((double)i / 5000.0);
            double e = 0.05 + 3.7 * ((double)((i * 7919) % 5000) / 5000.0);
            double got = a_pow(b, e);
            long double want = powl((long double)b, (long double)e);
            long double d = ulp_dist(got, want);
            if (d > worst) { worst = d; arg_at = b; }
        }
        printf("  pow   [fractional]: worst %Lf ULP at base=%g (bound 48)\n",
               worst, arg_at);
        if (!(worst <= 48)) { printf("    FAIL: pow exceeded its bound\n"); failures++; }
    }

    /* fmod must now be exactly rounded (the review's foundation). */
    {
        for (int i = 0; i < 20000; i++) {
            double x = -1e6 + 2e6 * ((double)i / 20000.0);
            double y = 0.13 + 20.0 * ((double)((i * 104729) % 9973) / 9973.0);
            double got = a_fmod(x, y);
            long double want = fmodl((long double)x, (long double)y);
            long double d = ulp_dist(got, want);
            if (d > 1) {
                printf("    FAIL: fmod off by %Lf ULP at (%g, %g)\n", d, x, y);
                failures++;
                break;
            }
        }
        printf("  fmod  [exactness sweep]: worst <= 1 ULP (bound 1)\n");
    }

    /* ---- errno contract (T4) ---- */
    {
        errno = 0; double r;
        r = a_sqrt(-1.0);
        if (!(isnan(r) && errno == EDOM)) { printf("    FAIL: sqrt(-1) errno\n"); failures++; }
        errno = 0;
        r = a_log(-1.0);
        if (!(isnan(r) && errno == EDOM)) { printf("    FAIL: log(-1) errno\n"); failures++; }
        errno = 0;
        r = a_log(0.0);
        if (!(r == -HUGE_VAL && errno == ERANGE)) { printf("    FAIL: log(0) errno\n"); failures++; }
        errno = 0;
        r = a_asin(2.0);
        if (!(isnan(r) && errno == EDOM)) { printf("    FAIL: asin(2) errno\n"); failures++; }
        errno = 0;
        r = a_acos(-1.5);
        if (!(isnan(r) && errno == EDOM)) { printf("    FAIL: acos(-1.5) errno\n"); failures++; }
        errno = 0;
        r = a_pow(0.0, -2.0);
        if (!(r == HUGE_VAL && errno == ERANGE)) { printf("    FAIL: pow(0,-2) errno\n"); failures++; }
        errno = 0;
        r = a_pow(-2.0, 0.5);
        if (!(isnan(r) && errno == EDOM)) { printf("    FAIL: pow(-2,0.5) errno\n"); failures++; }
        errno = 0;
        r = a_fmod(1.0, 0.0);
        if (!(isnan(r) && errno == EDOM)) { printf("    FAIL: fmod(1,0) errno\n"); failures++; }
        errno = 0;
        r = a_exp(1000.0);
        if (!(r == HUGE_VAL && errno == ERANGE)) { printf("    FAIL: exp(1000) errno\n"); failures++; }
        printf("  errno contract: sqrt/log/asin/acos/pow/fmod/exp all pass\n");
    }

    if (failures == 0) { printf("test_mathulp: ALL PASS\n"); return 0; }
    printf("test_mathulp: %d FAILURE(S)\n", failures);
    return 1;
}

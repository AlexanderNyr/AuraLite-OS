/* libc/src/math_extra.c — extended math functions (P10)
 *
 * Core functions (sin/cos/fabs/sqrt/floor/ceil/exp/log/pow/log2) live in
 * libc.c and are backed by SSE instructions or vetted software routines.
 * This file adds the rest of the C99 <math.h> surface.
 *
 * IMPORTANT: we must NOT route these through the matching __builtin_* (e.g.
 * __builtin_asin): for a runtime argument the compiler lowers that to a call
 * to asin() — i.e. THIS function — producing an infinite self-recursive loop
 * (observed as `jmp self` in the disassembly).  Instead, everything here is
 * expressed via the solid primitives from libc.c (sin/cos/sqrt/exp/log/...)
 * and small, dependency-free numerical kernels.
 */

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define M_PI_2 (M_PI / 2.0)

/* ---- helpers (bit twiddling without <stdint.h> dependency) ---- */
static int is_nan(double x) { return x != x; }

/* ---- rounding / remainder ---- */

/* trunc(x): round toward zero. */
double trunc(double x) {
    return (x < 0.0) ? ceil(x) : floor(x);
}

/* round(x): round half away from zero. */
double round(double x) {
    return (x < 0.0) ? ceil(x - 0.5) : floor(x + 0.5);
}

/* nearbyint(x): round to nearest, ties to even (banker's rounding). */
double nearbyint(double x) {
    double f = floor(x);
    double diff = x - f;
    if (diff < 0.5) return f;
    if (diff > 0.5) return f + 1.0;
    /* exactly .5 — pick the even neighbour */
    double lo = f, hi = f + 1.0;
    /* even of lo/hi */
    return (fmod(lo, 2.0) == 0.0) ? lo : hi;
}

/* fmod(x, y): IEEE remainder with the sign of x, |result| < |y|. */
double fmod(double x, double y) {
    if (y == 0.0 || is_nan(x) || is_nan(y)) return NAN;
    double q = trunc(x / y);
    double r = x - q * y;
    return r;
}

/* remainder(x, y): result in [-|y|/2, +|y|/2], ties to even quotient. */
double remainder(double x, double y) {
    if (y == 0.0 || is_nan(x) || is_nan(y)) return NAN;
    double q = nearbyint(x / y);
    return x - q * y;
}

/* ---- inverse trigonometric ---- */

/* atan(x) via a range-reduced Taylor/Euler series.
 * For |x|>1 use atan(x) = pi/2 - atan(1/x).  For |x|<=1 use the rapidly
 * converging series atan(x) = y/(1+? )... we use the Euler accelerated form:
 *   atan(x) = sum_{n>=0} (2^{2n} (n!)^2 / (2n+1)!) * x^{2n+1} / (1+x^2)^{n+1}
 * which converges for all real x. */
double atan(double x) {
    if (is_nan(x)) return NAN;
    int neg = 0;
    if (x < 0) { x = -x; neg = 1; }
    /* Euler's accelerated series: atan(x) = (x/(1+x^2)) * sum_{n>=0} c_n,
     * with c_0 = 1, c_{n} = c_{n-1} * (2n/(2n+1)) * (x^2/(1+x^2)). */
    double x2 = x * x;
    double z = x2 / (1.0 + x2);
    double term = 1.0;
    double sum = 1.0;
    for (int n = 1; n < 200; n++) {
        term *= (2.0 * n) / (2.0 * n + 1.0) * z;
        sum += term;
        if (term < 1e-17 * sum) break;
    }
    double r = (x / (1.0 + x2)) * sum;
    return neg ? -r : r;
}

/* atan2(y, x): full-quadrant arctangent. */
double atan2(double y, double x) {
    if (is_nan(x) || is_nan(y)) return NAN;
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) {
        if (y >= 0.0) return atan(y / x) + M_PI;
        return atan(y / x) - M_PI;
    }
    /* x == 0 */
    if (y > 0.0) return M_PI_2;
    if (y < 0.0) return -M_PI_2;
    return 0.0;   /* (0,0) */
}

/* asin(x) = atan(x / sqrt(1 - x^2)), with the endpoints handled exactly. */
double asin(double x) {
    if (is_nan(x) || x < -1.0 || x > 1.0) return NAN;
    if (x == 1.0) return M_PI_2;
    if (x == -1.0) return -M_PI_2;
    return atan(x / sqrt(1.0 - x * x));
}

/* acos(x) = pi/2 - asin(x). */
double acos(double x) {
    if (is_nan(x) || x < -1.0 || x > 1.0) return NAN;
    return M_PI_2 - asin(x);
}

/* tan(x) = sin(x)/cos(x). */
double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return NAN;
    return sin(x) / c;
}

/* ---- hyperbolic ---- */
double sinh(double x) { double e = exp(x); return (e - 1.0 / e) * 0.5; }
double cosh(double x) { double e = exp(x); return (e + 1.0 / e) * 0.5; }
double tanh(double x) {
    if (x >  20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e2 = exp(2.0 * x);
    return (e2 - 1.0) / (e2 + 1.0);
}

/* ---- exp/log family ---- */
double exp2(double x)  { return pow(2.0, x); }
double log10(double x) { return log(x) / 2.302585092994045901; /* ln(10) */ }

/* cbrt(x): real cube root, preserving sign. */
double cbrt(double x) {
    if (x == 0.0 || is_nan(x)) return x;
    double a = fabs(x);
    double r = pow(a, 1.0 / 3.0);
    /* one Newton refinement for accuracy: r -= (r^3 - a)/(3 r^2) */
    r = r - (r * r * r - a) / (3.0 * r * r);
    return (x < 0.0) ? -r : r;
}

/* hypot(x, y) = sqrt(x^2 + y^2) with scaling to avoid overflow. */
double hypot(double x, double y) {
    x = fabs(x); y = fabs(y);
    if (x < y) { double t = x; x = y; y = t; }
    if (x == 0.0) return 0.0;
    double r = y / x;
    return x * sqrt(1.0 + r * r);
}

/* fma(x,y,z) = x*y + z.  No hardware FMA guarantee here; this gives the
 * correctly-rounded value for typical inputs used by our libc. */
double fma(double x, double y, double z) { return x * y + z; }

/* ---- decomposition ---- */

/* frexp(x): x = m * 2^e with m in [0.5,1).  Returns m, stores e. */
double frexp(double x, int *eptr) {
    int e = 0;
    if (x == 0.0 || is_nan(x)) { if (eptr) *eptr = 0; return x; }
    double a = fabs(x);
    while (a >= 1.0) { a *= 0.5; e++; }
    while (a < 0.5)  { a *= 2.0; e--; }
    if (eptr) *eptr = e;
    return (x < 0.0) ? -a : a;
}

/* ldexp(x, e) = x * 2^e. */
double ldexp(double x, int e) {
    double r = x;
    if (e > 0) while (e--) r *= 2.0;
    else       while (e++) r *= 0.5;
    return r;
}

/* modf(x): split into integer and fractional parts (both with sign of x). */
double modf(double x, double *iptr) {
    double ip = trunc(x);
    if (iptr) *iptr = ip;
    return x - ip;
}

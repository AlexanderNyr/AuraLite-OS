#ifndef AURALITE_LIBC_TGMATH_H
#define AURALITE_LIBC_TGMATH_H

/*
 * tgmath.h — POSIX.1-2024 <tgmath.h>: type-generic math macros.
 *
 * AuraLite's <math.h> only implements the `double` precision entry points
 * (no `float`/`long double` overloads), and <complex.h> is a stub with no
 * working arithmetic.  Under those constraints, the only useful type-generic
 * behaviour is: real arguments (any real type) always dispatch to the
 * `double` function, matching C's usual arithmetic conversions; complex
 * arguments dispatch to the small set of stub functions in <complex.h>.
 * This keeps `tgmath.h` usable for real-valued code (the overwhelming
 * majority of programs) while being honest that complex support is a stub.
 */

#include <math.h>
#include <complex.h>

#define AURALITE_TGMATH_REAL1(func, x) (func)(x)
#define AURALITE_TGMATH_REAL2(func, x, y) (func)((x), (y))

#define sin(x)    AURALITE_TGMATH_REAL1(sin, x)
#define cos(x)    AURALITE_TGMATH_REAL1(cos, x)
#define tan(x)    AURALITE_TGMATH_REAL1(tan, x)
#define asin(x)   AURALITE_TGMATH_REAL1(asin, x)
#define acos(x)   AURALITE_TGMATH_REAL1(acos, x)
#define atan(x)   AURALITE_TGMATH_REAL1(atan, x)
#define atan2(y,x) AURALITE_TGMATH_REAL2(atan2, y, x)
#define sinh(x)   AURALITE_TGMATH_REAL1(sinh, x)
#define cosh(x)   AURALITE_TGMATH_REAL1(cosh, x)
#define tanh(x)   AURALITE_TGMATH_REAL1(tanh, x)
#define exp(x)    AURALITE_TGMATH_REAL1(exp, x)
#define exp2(x)   AURALITE_TGMATH_REAL1(exp2, x)
#define log(x)    AURALITE_TGMATH_REAL1(log, x)
#define log10(x)  AURALITE_TGMATH_REAL1(log10, x)
#define log2(x)   AURALITE_TGMATH_REAL1(log2, x)
#define pow(x,y)  AURALITE_TGMATH_REAL2(pow, x, y)
#define sqrt(x)   AURALITE_TGMATH_REAL1(sqrt, x)
#define cbrt(x)   AURALITE_TGMATH_REAL1(cbrt, x)
#define fabs(x)   AURALITE_TGMATH_REAL1(fabs, x)
#define hypot(x,y) AURALITE_TGMATH_REAL2(hypot, x, y)
#define fmod(x,y) AURALITE_TGMATH_REAL2(fmod, x, y)
#define floor(x)  AURALITE_TGMATH_REAL1(floor, x)
#define ceil(x)   AURALITE_TGMATH_REAL1(ceil, x)
#define round(x)  AURALITE_TGMATH_REAL1(round, x)
#define trunc(x)  AURALITE_TGMATH_REAL1(trunc, x)
#define nearbyint(x) AURALITE_TGMATH_REAL1(nearbyint, x)
#define remainder(x,y) AURALITE_TGMATH_REAL2(remainder, x, y)

/* Complex-only entry points: no real-valued overload makes sense for these. */
#define creal(z) (creal)(z)
#define cimag(z) (cimag)(z)
#define cabs(z)  (cabs)(z)
#define conj(z)  (conj)(z)

#endif /* AURALITE_LIBC_TGMATH_H */

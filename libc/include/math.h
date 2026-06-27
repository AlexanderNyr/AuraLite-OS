#ifndef AURALITE_LIBC_MATH_H
#define AURALITE_LIBC_MATH_H

/*
 * math.h — minimal double-precision math for AuraLite user programs.
 *
 * Implementations live in libc/src/libc.c.  They favour clarity and small code
 * size over last-ULP accuracy: fabs/floor/ceil/sqrt are exact (sqrt uses the
 * SSE2 hardware instruction), while exp/log/sin/cos/pow use range-reduced
 * series and are accurate to roughly 1e-10 for typical arguments.
 */

#define M_PI    3.14159265358979323846
#define M_E     2.71828182845904523536
#define M_SQRT2 1.41421356237309504880

#define HUGE_VAL (__builtin_huge_val())
#define NAN      (__builtin_nanf(""))
#define INFINITY (__builtin_inff())

double fabs(double x);
double floor(double x);
double ceil(double x);
double sqrt(double x);
double pow(double base, double exp);
double exp(double x);
double log(double x);     /* natural logarithm */
double log2(double x);
double sin(double x);
double cos(double x);

#endif /* AURALITE_LIBC_MATH_H */

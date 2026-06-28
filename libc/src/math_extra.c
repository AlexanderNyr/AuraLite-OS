/* libc/src/math_extra.c — дополнительные математические функции (P10) */

#include <math.h>

double sin(double x) { return __builtin_sin(x); }
double cos(double x) { return __builtin_cos(x); }
double tan(double x) { return __builtin_tan(x); }
double fabs(double x) { return __builtin_fabs(x); }
double sqrt(double x) { return __builtin_sqrt(x); }
double floor(double x) { return __builtin_floor(x); }
double ceil(double x)  { return __builtin_ceil(x); }
double pow(double x, double y) { return __builtin_pow(x, y); }
double log(double x)   { return __builtin_log(x); }
double exp(double x)   { return __builtin_exp(x); }
double fmod(double x, double y) { return __builtin_fmod(x, y); }
double round(double x) { return __builtin_round(x); }
double trunc(double x) { return __builtin_trunc(x); }
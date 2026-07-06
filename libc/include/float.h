#ifndef AURALITE_LIBC_FLOAT_H
#define AURALITE_LIBC_FLOAT_H

/*
 * float.h — POSIX.1-2024 <float.h> for AuraLite user programs (x86_64).
 *
 * IEEE 754 binary32/binary64 characteristics plus the x87 80-bit extended
 * format used for `long double` on this target.  AuraLite never changes the
 * FPU rounding mode at runtime, so FLT_ROUNDS is fixed at round-to-nearest.
 */

#define FLT_RADIX       2
#define FLT_ROUNDS      1   /* round to nearest */
#define FLT_EVAL_METHOD 0

#define DECIMAL_DIG     21  /* long double decimal digits (80-bit extended) */

/* ---- float (IEEE 754 binary32) ---- */
#define FLT_MANT_DIG    24
#define FLT_DIG          6
#define FLT_MIN_EXP   (-125)
#define FLT_MIN_10_EXP (-37)
#define FLT_MAX_EXP    128
#define FLT_MAX_10_EXP  38
#define FLT_MIN         1.17549435e-38F
#define FLT_MAX         3.40282347e+38F
#define FLT_EPSILON     1.19209290e-07F
#define FLT_TRUE_MIN    1.40129846e-45F
#define FLT_HAS_SUBNORM 1

/* ---- double (IEEE 754 binary64) ---- */
#define DBL_MANT_DIG    53
#define DBL_DIG         15
#define DBL_MIN_EXP   (-1021)
#define DBL_MIN_10_EXP (-307)
#define DBL_MAX_EXP    1024
#define DBL_MAX_10_EXP  308
#define DBL_MIN         2.2250738585072014e-308
#define DBL_MAX         1.7976931348623157e+308
#define DBL_EPSILON     2.2204460492503131e-16
#define DBL_TRUE_MIN    4.9406564584124654e-324
#define DBL_HAS_SUBNORM 1

/* ---- long double (x87 80-bit extended) ---- */
#define LDBL_MANT_DIG   64
#define LDBL_DIG        18
#define LDBL_MIN_EXP  (-16381)
#define LDBL_MIN_10_EXP (-4931)
#define LDBL_MAX_EXP   16384
#define LDBL_MAX_10_EXP 4932
#define LDBL_MIN        3.36210314311209350626e-4932L
#define LDBL_MAX        1.18973149535723176502e+4932L
#define LDBL_EPSILON    1.08420217248550443401e-19L
#define LDBL_TRUE_MIN   3.64519953188247460253e-4951L
#define LDBL_HAS_SUBNORM 1

#endif /* AURALITE_LIBC_FLOAT_H */

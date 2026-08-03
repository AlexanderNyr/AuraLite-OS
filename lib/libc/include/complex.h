#ifndef AURALITE_LIBC_COMPLEX_H
#define AURALITE_LIBC_COMPLEX_H

/*
 * complex.h — POSIX.1-2024 <complex.h>.
 *
 * AuraLite does not implement complex arithmetic.  This header exists so
 * that strictly-conforming translation units which merely #include it (or
 * check for the `complex`/`_Complex_I` macros) still compile; the few
 * functions declared here are stubs implemented in libc/src/compat.c and
 * documented as such in docs/posix2024_compliance.md.  Programs that need
 * real complex math are not yet supported on AuraLite.
 */

#define complex _Complex
#define _Complex_I (__extension__ 1.0iF)
#define I _Complex_I

double creal(double _Complex z);
double cimag(double _Complex z);
double cabs(double _Complex z);
double _Complex conj(double _Complex z);

#endif /* AURALITE_LIBC_COMPLEX_H */

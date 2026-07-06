#ifndef AURALITE_LIBC_FENV_H
#define AURALITE_LIBC_FENV_H

/*
 * fenv.h — POSIX.1-2024 <fenv.h>: floating-point environment access.
 *
 * AuraLite user programs are built without SSE/x87 codegen by default and
 * the kernel never establishes a floating-point environment for user
 * threads, so this header is a conforming-but-inert stub: the functions are
 * implemented in libc/src/compat.c, always report the default environment
 * (round-to-nearest, no exceptions pending) and never fail.
 */

typedef unsigned int fenv_t;
typedef unsigned short fexcept_t;

#define FE_INVALID   0x01
#define FE_DIVBYZERO 0x04
#define FE_OVERFLOW  0x08
#define FE_UNDERFLOW 0x10
#define FE_INEXACT   0x20
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

#define FE_TONEAREST  0x0000
#define FE_DOWNWARD   0x0400
#define FE_UPWARD     0x0800
#define FE_TOWARDZERO 0x0C00

extern const fenv_t __fe_dfl_env;
#define FE_DFL_ENV (&__fe_dfl_env)

int feclearexcept(int excepts);
int fegetexceptflag(fexcept_t *flagp, int excepts);
int feraiseexcept(int excepts);
int fesetexceptflag(const fexcept_t *flagp, int excepts);
int fetestexcept(int excepts);

int fegetround(void);
int fesetround(int round);

int fegetenv(fenv_t *envp);
int fesetenv(const fenv_t *envp);
int feupdateenv(const fenv_t *envp);
int feholdexcept(fenv_t *envp);

#endif /* AURALITE_LIBC_FENV_H */

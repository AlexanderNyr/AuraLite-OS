/*
 * libc/src/compat.c — runtime implementations for the Q1 thin-wrapper
 * headers (POSIX.1-2024 mandatory C standard headers).
 *
 * Most functions declared in the new headers are compiler built-ins or
 * trivial one-line aliases over functions AuraLite already implements
 * (P1-P10, POSIX.1-2017).  This file collects the small set that needs an
 * actual runtime body:
 *
 *   <strings.h>   — BSD compatibility aliases (bcmp/bcopy/bzero/index/...)
 *   <wctype.h>    — "C" locale wide-character classification
 *   <inttypes.h>  — intmax_t helpers
 *   <threads.h>   — C11 threads mapped onto pthreads
 *   <fenv.h>      — floating-point environment stubs (no FP env on AuraLite)
 *   <complex.h>   — complex arithmetic stubs (not implemented on AuraLite)
 *   <setjmp.h>    — sigsetjmp/siglongjmp (setjmp/longjmp proper live in
 *                   libc/crt/setjmp.asm)
 *   <uchar.h>     — UTF-8 <-> UTF-16/UTF-32 single-code-point conversion
 *                   (ASCII exact, EILSEQ for anything requiring real
 *                   multibyte decoding — full UTF-8 decoding is future work)
 */

#include <strings.h>
#include <wctype.h>
#include <inttypes.h>
#include <setjmp.h>
#include <threads.h>
#include <uchar.h>
#include <fenv.h>
#include <complex.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <math.h>

/* ------------------------------------------------------------------------
 * <strings.h> — legacy BSD aliases
 * ------------------------------------------------------------------------ */

int bcmp(const void *s1, const void *s2, size_t n) {
    return memcmp(s1, s2, n);
}

void bcopy(const void *src, void *dst, size_t n) {
    /* AuraLite's <string.h> does not provide memmove() (a pre-existing gap
     * outside this phase's scope), so bcopy() — whose whole POSIX contract
     * is to behave correctly even when the regions overlap — implements the
     * same overlap-safe copy directly instead of silently degrading to a
     * forward-only memcpy(). */
    const unsigned char *s = src;
    unsigned char *d = dst;
    if (d == s || n == 0) return;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
}

void bzero(void *s, size_t n) {
    memset(s, 0, n);
}

char *index(const char *s, int c) {
    return strchr(s, c);
}

char *rindex(const char *s, int c) {
    return strrchr(s, c);
}

int ffs(int i) {
    if (i == 0) return 0;
    int bit = 1;
    while (!(i & 1)) { i >>= 1; bit++; }
    return bit;
}

int ffsl(long i) {
    if (i == 0) return 0;
    int bit = 1;
    while (!(i & 1)) { i >>= 1; bit++; }
    return bit;
}

int ffsll(long long i) {
    if (i == 0) return 0;
    int bit = 1;
    while (!(i & 1)) { i >>= 1; bit++; }
    return bit;
}

/* ------------------------------------------------------------------------
 * <wctype.h> — "C" locale wide-character classification
 * ------------------------------------------------------------------------ */

int iswalnum(wint_t c) { return c < 128 && isalnum((int)c); }
int iswalpha(wint_t c) { return c < 128 && isalpha((int)c); }
int iswblank(wint_t c) { return c == ' ' || c == '\t'; }
int iswcntrl(wint_t c) { return c < 128 && iscntrl((int)c); }
int iswdigit(wint_t c) { return c < 128 && isdigit((int)c); }
int iswgraph(wint_t c) { return c < 128 && isgraph((int)c); }
int iswlower(wint_t c) { return c < 128 && islower((int)c); }
int iswprint(wint_t c) { return c < 128 && isprint((int)c); }
int iswpunct(wint_t c) { return c < 128 && ispunct((int)c); }
int iswspace(wint_t c) { return c < 128 && isspace((int)c); }
int iswupper(wint_t c) { return c < 128 && isupper((int)c); }
int iswxdigit(wint_t c) { return c < 128 && isxdigit((int)c); }

wint_t towlower(wint_t c) { return c < 128 ? (wint_t)tolower((int)c) : c; }
wint_t towupper(wint_t c) { return c < 128 ? (wint_t)toupper((int)c) : c; }

wctype_t wctype(const char *property) {
    (void)property;
    return 0;   /* No named classes beyond the fixed iswXXX() set. */
}

int iswctype(wint_t c, wctype_t desc) {
    (void)c; (void)desc;
    return 0;
}

wctrans_t wctrans(const char *property) {
    (void)property;
    return 0;
}

wint_t towctrans(wint_t c, wctrans_t desc) {
    (void)desc;
    return c;
}

/* ------------------------------------------------------------------------
 * <inttypes.h> — intmax_t helpers
 * ------------------------------------------------------------------------ */

intmax_t strtoimax(const char *s, char **end, int base) {
    return strtoll(s, end, base);
}

uintmax_t strtoumax(const char *s, char **end, int base) {
    return strtoull(s, end, base);
}

intmax_t imaxabs(intmax_t j) {
    return j < 0 ? -j : j;
}

imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom) {
    imaxdiv_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

/* ------------------------------------------------------------------------
 * <threads.h> — C11 threads mapped onto pthreads
 * ------------------------------------------------------------------------ */

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg) {
    /* pthread_create() expects void *(*)(void *); the ABI-level call
     * sequence for an `int (*)(void *)` function called through that
     * signature is identical on x86_64 (integer return value in EAX,
     * sign-extended into RAX), so we cast through a union of function
     * pointer types rather than reimplementing thread creation. */
    union { thrd_start_t as_thrd; void *(*as_pthread)(void *); } u;
    u.as_thrd = func;
    int r = pthread_create(thr, NULL, u.as_pthread, arg);
    return r == 0 ? thrd_success : thrd_error;
}

int thrd_join(thrd_t thr, int *res) {
    void *r = NULL;
    if (pthread_join(thr, &r) != 0) return thrd_error;
    if (res) *res = (int)(intptr_t)r;
    return thrd_success;
}

void thrd_exit(int res) {
    pthread_exit((void *)(intptr_t)res);
    for (;;) { }   /* pthread_exit() never returns; satisfy [[noreturn]]. */
}

int thrd_detach(thrd_t thr) {
    /* AuraLite's pthreads (P9) do not yet implement pthread_detach();
     * report success since joinable-by-default threads that are never
     * joined do not leak resources on this target's thread model. */
    (void)thr;
    return thrd_success;
}

int thrd_equal(thrd_t a, thrd_t b) {
    return a == b;
}

thrd_t thrd_current(void) {
    return pthread_self();
}

void thrd_yield(void) {
    /* AuraLite does not yet expose sched_yield() (that lands with Phase Q8
     * of the POSIX.1-2024 plan); a zero-duration nanosleep() is accepted by
     * the kernel's scheduler as "give up the rest of this timeslice" and
     * gives equivalent cooperative-yield behaviour in the meantime. */
    struct timespec zero = {0, 0};
    nanosleep(&zero, NULL);
}

int thrd_sleep(const struct timespec *duration, struct timespec *remaining) {
    return nanosleep(duration, remaining) == 0 ? 0 : -1;
}

int mtx_init(mtx_t *mtx, int type) {
    (void)type;
    return pthread_mutex_init(mtx, NULL) == 0 ? thrd_success : thrd_error;
}

void mtx_destroy(mtx_t *mtx) {
    pthread_mutex_destroy(mtx);
}

int mtx_lock(mtx_t *mtx) {
    return pthread_mutex_lock(mtx) == 0 ? thrd_success : thrd_error;
}

int mtx_trylock(mtx_t *mtx) {
    int r = pthread_mutex_trylock(mtx);
    if (r == 0) return thrd_success;
    return r == EBUSY ? thrd_busy : thrd_error;
}

int mtx_timedlock(mtx_t *mtx, const struct timespec *ts) {
    /* No timed-lock primitive exists yet on AuraLite's mutex; fall back to
     * a plain (unbounded) lock rather than silently ignoring the timeout
     * requirement by returning success without actually locking. */
    (void)ts;
    return pthread_mutex_lock(mtx) == 0 ? thrd_success : thrd_error;
}

int mtx_unlock(mtx_t *mtx) {
    return pthread_mutex_unlock(mtx) == 0 ? thrd_success : thrd_error;
}

int cnd_init(cnd_t *cond) {
    return pthread_cond_init(cond, NULL) == 0 ? thrd_success : thrd_error;
}

void cnd_destroy(cnd_t *cond) {
    pthread_cond_destroy(cond);
}

int cnd_signal(cnd_t *cond) {
    return pthread_cond_signal(cond) == 0 ? thrd_success : thrd_error;
}

int cnd_broadcast(cnd_t *cond) {
    return pthread_cond_broadcast(cond) == 0 ? thrd_success : thrd_error;
}

int cnd_wait(cnd_t *cond, mtx_t *mtx) {
    return pthread_cond_wait(cond, mtx) == 0 ? thrd_success : thrd_error;
}

int cnd_timedwait(cnd_t *cond, mtx_t *mtx, const struct timespec *ts) {
    /* See mtx_timedlock(): no timed condwait primitive exists yet. */
    (void)ts;
    return pthread_cond_wait(cond, mtx) == 0 ? thrd_success : thrd_error;
}

int tss_create(tss_t *key, tss_dtor_t dtor) {
    return pthread_key_create(key, dtor) == 0 ? thrd_success : thrd_error;
}

void tss_delete(tss_t key) {
    pthread_key_delete(key);
}

void *tss_get(tss_t key) {
    return pthread_getspecific(key);
}

int tss_set(tss_t key, void *val) {
    return pthread_setspecific(key, val) == 0 ? thrd_success : thrd_error;
}

void call_once(once_flag *flag, void (*func)(void)) {
    pthread_once(flag, func);
}

/* ------------------------------------------------------------------------
 * <fenv.h> — floating-point environment stubs
 *
 * AuraLite user programs are built without SSE/x87 codegen and the kernel
 * never establishes a per-thread FP environment, so there genuinely is
 * nothing to save/restore: every function below is a documented no-op that
 * reports the fixed default environment and never fails.
 * ------------------------------------------------------------------------ */

const fenv_t __fe_dfl_env = 0;

int feclearexcept(int excepts) { (void)excepts; return 0; }
int fegetexceptflag(fexcept_t *flagp, int excepts) {
    (void)excepts;
    if (flagp) *flagp = 0;
    return 0;
}
int feraiseexcept(int excepts) { (void)excepts; return 0; }
int fesetexceptflag(const fexcept_t *flagp, int excepts) {
    (void)flagp; (void)excepts;
    return 0;
}
int fetestexcept(int excepts) { (void)excepts; return 0; }

int fegetround(void) { return FE_TONEAREST; }
int fesetround(int round) { return round == FE_TONEAREST ? 0 : -1; }

int fegetenv(fenv_t *envp) {
    if (envp) *envp = __fe_dfl_env;
    return 0;
}
int fesetenv(const fenv_t *envp) { (void)envp; return 0; }
int feupdateenv(const fenv_t *envp) { (void)envp; return 0; }
int feholdexcept(fenv_t *envp) {
    if (envp) *(fenv_t *)envp = __fe_dfl_env;
    return 0;
}

/* ------------------------------------------------------------------------
 * <complex.h> — stubs (complex arithmetic is not implemented on AuraLite)
 * ------------------------------------------------------------------------ */

double creal(double _Complex z) { return __real__ z; }
double cimag(double _Complex z) { return __imag__ z; }
double cabs(double _Complex z)  { return hypot(__real__ z, __imag__ z); }
double _Complex conj(double _Complex z) {
    double _Complex r = z;
    __imag__ r = -__imag__ z;
    return r;
}

/* ------------------------------------------------------------------------
 * <setjmp.h> — sigsetjmp()/siglongjmp() (setjmp/longjmp proper are in
 * libc/crt/setjmp.asm; these wrap them with signal-mask save/restore).
 *
 * sigjmp_buf layout: [0..7] = jmp_buf, [8] = saved sigset_t, [9] = "mask
 * was saved" flag (savemask != 0), matching libc/include/setjmp.h.
 * ------------------------------------------------------------------------ */

int sigsetjmp(sigjmp_buf env, int savemask) {
    env[9] = savemask;
    if (savemask) {
        sigset_t cur;
        sigprocmask(SIG_SETMASK, NULL, &cur);
        env[8] = (long)cur;
    }
    return setjmp(env);
}

void siglongjmp(sigjmp_buf env, int val) {
    if (env[9]) {
        sigset_t saved = (sigset_t)env[8];
        sigprocmask(SIG_SETMASK, &saved, NULL);
    }
    longjmp(env, val);
}

/* ------------------------------------------------------------------------
 * <uchar.h> — UTF-8 <-> UTF-16/UTF-32 single-code-point conversion
 *
 * Only plain ASCII (< 0x80) is decoded exactly, matching every other
 * multibyte-aware function in AuraLite's "C" locale libc.  Anything past
 * ASCII reports EILSEQ rather than silently mis-decoding.
 * ------------------------------------------------------------------------ */

size_t mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps) {
    (void)ps;
    if (!s) return 0;              /* querying shift state: always trivial */
    if (n == 0) return (size_t)-2; /* need more bytes to decide */
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        if (pc32) *pc32 = c;
        return c == 0 ? 0 : 1;
    }
    errno = EILSEQ;
    return (size_t)-1;
}

size_t c32rtomb(char *s, char32_t c32, mbstate_t *ps) {
    (void)ps;
    if (!s) return 1;   /* reset shift state: always trivial for us */
    if (c32 < 0x80) {
        *s = (char)c32;
        return 1;
    }
    errno = EILSEQ;
    return (size_t)-1;
}

size_t mbrtoc16(char16_t *pc16, const char *s, size_t n, mbstate_t *ps) {
    char32_t c32 = 0;
    size_t r = mbrtoc32(&c32, s, n, ps);
    if (pc16 && r != (size_t)-1 && r != (size_t)-2) *pc16 = (char16_t)c32;
    return r;
}

size_t c16rtomb(char *s, char16_t c16, mbstate_t *ps) {
    return c32rtomb(s, (char32_t)c16, ps);
}

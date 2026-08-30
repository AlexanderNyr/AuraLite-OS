/* tools/selfhost/tcc_closure_runtime.c -- SH8: minimal runtime shims so the
 * in-guest tcc chain links cleanly.
 *
 * The seed tcc0 is /bin/tcc (build/selfhost/tcc.elf, linked against the full
 * host-built libaurac.a), so it never leaves these symbols behind.  But when
 * tcc0 compiles the tcc sources, two of tcc's own objects (libtcc.o and
 * tccelf.o) reference the POSIX dynamic-loader and semaphore APIs.  tcc only
 * touches those paths when it is still acting as an embedding library, and the
 * self-host build never calls them -- but aulink has no --gc-sections and no
 * per-symbol archive selection, so the *symbols* must resolve for the link to
 * succeed.  These are correct-if-inert, single-threaded implementations.
 *
 * This file is staged into /src and compiled by the guest tcc into the closure
 * libc set; it is NOT part of the shipped userland.
 *
 * We do NOT define the __builtin_* math/bswap helpers here: host tcc folds
 * fabs()/sqrt()/NAN/HUGE_VAL/byte-swaps at codegen time, and libtcc1.a already
 * carries the clz/ctz/ffs family, so the narrowed closure links them cleanly.
 */

/* ---- Pthreads semaphore (single-threaded, inert) -------------------------
 * tcc's libtcc.c includes <semaphore.h> and calls sem_init/sem_wait/sem_post
 * around the worker-thread load.  The self-host build starts no threads, so a
 * lock-free no-op is functionally correct; the type width must match the
 * libc's sem_t (set of unsigned longs, guaranteed >= 32 bits). */
#include <stddef.h>

typedef struct { unsigned long __seq[1]; } _sh8_sem_t;

int sem_init(void *sem, int pshared, unsigned int value)
{
    if (!sem) return -1;
    (void)pshared;
    ((_sh8_sem_t *)sem)->__seq[0] = value;
    return 0;
}
int sem_destroy(void *sem) { (void)sem; return 0; }
int sem_wait(void *sem)    { (void)sem; return 0; }
int sem_post(void *sem)    { (void)sem; return 0; }
int sem_trywait(void *sem) { (void)sem; return 0; }

/* ---- Dynamic loader (versioned stubs; never invoked by the build) -------- */
void *dlopen(const char *name, int flag) { (void)name; (void)flag; return 0; }
void *dlsym(void *handle, const char *name) { (void)handle; (void)name; return 0; }
int   dlclose(void *handle) { (void)handle; return 0; }
char *dlerror(void) { static char m[]="self-host tcc: dynamic loading unsupported"; return m; }

// w32a4_cxx.cpp — real C++ unwinding through libgcc's personality.  W32A-4.
//
// Three frames with destructors; the innermost throws.  libgcc's SEH
// personality (_GCC_specific_handler, statically linked) drives the
// cleanup pass through OUR RtlVirtualUnwind/RtlUnwindEx/LookupFunctionEntry,
// so the destructor order on stdout is the assertion that the whole loop
// interoperates with compiler-generated tables.  The throw is uncaught on
// purpose: the harness abort (below) exits 3, MSVC's own abort code --
// an Itanium-ABI throw can never reach our MSVC-shaped _CxxThrowException,
// and a silent catch would be the D7 lie.
//
// Built like the W32A-2 probes: -nostdlib, --entry=winstart, kernel32-only
// imports (no msvcrt.dll at all).  Everything libstdc++/libgcc wants from
// the CRT is a static shim in this TU, kept minimal and documented; the
// linker enumerates the set, so nothing hides.

#include <windows.h>
#include <stdint.h>

// --- compiler-emitted helpers (the W32A-2 idiom) ---------------------------
void *memset(void *d, int c, unsigned long long n) {
    volatile unsigned char *p = (volatile unsigned char *)d;
    while (n-- > 0)
        *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned long long n) {
    volatile unsigned char *p = (volatile unsigned char *)d;
    volatile const unsigned char *q = (volatile const unsigned char *)s;
    while (n-- > 0)
        *p++ = *q++;
    return d;
}

// --- output (kernel32-only, like the NASM fixtures) -------------------------
static HANDLE out_h;

static void put(const char *s, unsigned n) {
    DWORD w = 0;
    WriteFile(out_h, s, n, &w, 0);
}

#define PUTLIT(s) put(s, (unsigned)(sizeof(s) - 1))

// --- CRT shims the static C++ runtime needs ---------------------------------
// HeapAlloc-backed: __cxa_allocate_exception is the only allocator client.
extern "C" void *malloc(unsigned long long n) {
    return HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n);
}

extern "C" void free(void *p) {
    if (p)
        HeapFree(GetProcessHeap(), 0, p);
}

// What libstdc++'s unwinder support wants beyond malloc/free/abort:
// single-threaded no-op mutexes (this TU never creates a thread, so
// uncontended locks are exactly correct), a NULL getenv (the emergency
// pool keeps its deterministic defaults), and three trivial string
// primitives for the env-tuning initializer (which never runs, but the
// linker still wants the symbols).
typedef unsigned long pthread_mutex_t;
extern "C" int pthread_mutex_init(pthread_mutex_t *m, const void *a) {
    (void)m;
    (void)a;
    return 0;
}

extern "C" int pthread_mutex_lock(pthread_mutex_t *m) {
    (void)m;
    return 0;
}

extern "C" int pthread_mutex_unlock(pthread_mutex_t *m) {
    (void)m;
    return 0;
}

extern "C" int pthread_mutex_destroy(pthread_mutex_t *m) {
    (void)m;
    return 0;
}

extern "C" char *getenv(const char *n) {
    (void)n;
    return 0;
}

extern "C" char *strchr(const char *s, int c) {
    volatile const char *p = (volatile const char *)s;
    while (*p && *p != (char)c)
        p++;
    return *p ? (char *)p : 0;
}

extern "C" int memcmp(const void *a, const void *b, unsigned long long n) {
    volatile const unsigned char *p = (volatile const unsigned char *)a;
    volatile const unsigned char *q = (volatile const unsigned char *)b;
    while (n-- > 0) {
        if (*p != *q)
            return *p < *q ? -1 : 1;
        p++;
        q++;
    }
    return 0;
}

extern "C" unsigned long strtoul(const char *s, char **e, int base) {
    volatile const char *p = (volatile const char *)s;
    unsigned long v = 0;
    (void)base;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (unsigned long)(*p++ - '0');
    if (e)
        *e = (char *)p;
    return v;
}

extern "C" unsigned long long strlen(const char *s) {
    volatile const char *p = (volatile const char *)s;
    unsigned long long n = 0;
    while (*p++)
        n++;
    return n;
}

extern "C" int strcmp(const char *a, const char *b) {
    volatile const char *p = (volatile const char *)a;
    volatile const char *q = (volatile const char *)b;
    while (*p && *p == *q) {
        p++;
        q++;
    }
    return (int)(unsigned char)*p - (int)(unsigned char)*q;
}

extern "C" int strncmp(const char *a, const char *b, unsigned long long n) {
    volatile const char *p = (volatile const char *)a;
    volatile const char *q = (volatile const char *)b;
    while (n-- > 0) {
        if (*p != *q || !*p)
            return (int)(unsigned char)*p - (int)(unsigned char)*q;
        p++;
        q++;
    }
    return 0;
}

extern "C" void *calloc(unsigned long long n, unsigned long long sz) {
    unsigned long long total = n * sz;
    void *p = malloc(total ? total : 1);
    if (p)
        memset(p, 0, total);
    return p;
}

extern "C" void *realloc(void *p, unsigned long long n) {
    void *q = malloc(n ? n : 1);
    if (q && p) {
        // No usable size query under -nostdlib; the runtime only grows
        // small blocks here, and over-copying a HeapAlloc block is safe
        // only up to its real size -- so copy the requested size capped
        // at what HeapSize reports.
        SIZE_T have = HeapSize(GetProcessHeap(), 0, p);
        SIZE_T cp = have < (SIZE_T)n ? have : (SIZE_T)n;
        memcpy(q, p, cp);
        free(p);
    }
    return q;
}

// Single-threaded pthreads: one process, one thread, so one TLS slot and
// run-once flags are exactly correct (no emulation, no lie).
typedef unsigned long pthread_key_t;
typedef int pthread_once_t;
#define PTHREAD_ONCE_INIT 0
static void *one_tls_slot = 0;

extern "C" int pthread_key_create(pthread_key_t *k, void (*d)(void *)) {
    (void)d;
    *k = 0;
    return 0;
}

extern "C" int pthread_setspecific(pthread_key_t k, const void *v) {
    (void)k;
    one_tls_slot = (void *)v;
    return 0;
}

extern "C" void *pthread_getspecific(pthread_key_t k) {
    (void)k;
    return one_tls_slot;
}

extern "C" int pthread_once(pthread_once_t *o, void (*fn)(void)) {
    if (!*o) {
        *o = 1;
        fn();
    }
    return 0;
}

// Newer mingw's libgcc/libstdc++ reference two more CRT bits: the
// thread-key destructor hook (emutls/gthr; single-threaded here, so a
// no-op is exactly correct) and the _CRT_MT flag (0: this TU never
// creates a thread, so the single-threaded shortcuts are valid).
extern "C" void __mingwthr_key_dtor(unsigned long k, void *v) {
    (void)k;
    (void)v;
}

extern "C" int _CRT_MT = 0;

// No static destructors in this TU; libstdc++ registers only its own
// freeres cleanup, which an ExitProcess makes moot.  Accept and drop.
extern "C" int atexit(void (*fn)(void)) {
    (void)fn;
    return 0;
}

// --- minimal stdio for the verbose terminate handler ----------------------
// libstdc++'s default terminate prints the exception type via fputs/fputc
// on stderr.  Three opaque FILE slots; the handle is chosen by slot, so
// the bytes land on the right console stream.
struct SHIM_FILE {
    int slot;
};

static SHIM_FILE shim_files[3] = { { 0 }, { 1 }, { 2 } };
static HANDLE shim_err_h = 0;

extern "C" void *__acrt_iob_func(void) {
    return shim_files;
}

// libmingwex compiled this TU's caller with dllimport: satisfy the
// __imp__ slot with a plain variable (no DLL involved).
extern "C" void *__imp___acrt_iob_func = (void *)__acrt_iob_func;

static HANDLE shim_handle(void *f) {
    if (f == &shim_files[2]) {
        if (!shim_err_h)
            shim_err_h = GetStdHandle((DWORD)-12);
        return shim_err_h;
    }
    return out_h;
}

extern "C" int fputc(int c, void *f) {
    DWORD w = 0;
    char ch = (char)c;
    WriteFile(shim_handle(f), &ch, 1, &w, 0);
    return c;
}

extern "C" int fputs(const char *s, void *f) {
    DWORD w = 0;
    WriteFile(shim_handle(f), s, (DWORD)strlen(s), &w, 0);
    return 0;
}

extern "C" unsigned long long fwrite(const void *p, unsigned long long sz,
                                     unsigned long long n, void *f) {
    DWORD w = 0;
    WriteFile(shim_handle(f), p, (DWORD)(sz * n), &w, 0);
    return n;
}

// Enough vsprintf for the verbose terminate line (%s, %d, %u, %x, %p, %c,
// %%): bounded, no float, no width.  mingw's va_list is a plain char*.
extern "C" int __mingw_vsprintf(char *out, const char *fmt, char *ap) {
    char *w = out;
    while (*fmt) {
        if (*fmt != '%') {
            *w++ = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            *w++ = '%';
            fmt++;
        } else if (*fmt == 's') {
            const char *s = *(const char **)ap;
            ap += 8;
            if (!s)
                s = "(null)";
            while (*s)
                *w++ = *s++;
            fmt++;
        } else if (*fmt == 'c') {
            *w++ = (char)*(int *)ap;
            ap += 8;
            fmt++;
        } else if (*fmt == 'd' || *fmt == 'u' || *fmt == 'x' ||
                   *fmt == 'p') {
            unsigned long long v = *(unsigned long long *)ap;
            ap += 8;
            int neg = 0;
            unsigned base = 10;
            if (*fmt == 'x' || *fmt == 'p')
                base = 16;
            if (*fmt == 'd' && (long long)v < 0) {
                neg = 1;
                v = (unsigned long long)(-(long long)v);
            }
            if (*fmt == 'p') {
                *w++ = '0';
                *w++ = 'x';
            }
            char tmp[24];
            int i = 0;
            do {
                unsigned d = (unsigned)(v % base);
                tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                v /= base;
            } while (v);
            if (neg)
                *w++ = '-';
            while (i-- > 0)
                *w++ = tmp[i];
            fmt++;
        } else {
            *w++ = '%';
        }
    }
    *w = 0;
    return (int)(w - out);
}

// Uncaught: terminate -> abort.  MSVC's abort exits 3 when SIGABRT has no
// handler; there is no signal machinery under -nostdlib, so that is what
// the harness does after printing the receipt.
extern "C" void abort(void) {
    PUTLIT("W32A4-CXX-ABORT\n");
    ExitProcess(3);
}

// --- the actual test --------------------------------------------------------
struct Guard {
    int id;
    explicit Guard(int i) : id(i) {}
    ~Guard() {
        char b[32];
        const char *h = "W32A4-CXX-DTOR ";
        unsigned i = 0, j = 0;
        while (h[i]) {
            b[j++] = h[i++];
        }
        b[j++] = (char)('0' + id);
        b[j++] = '\n';
        put(b, j);
    }
};

struct Boom {};

// Noinline: the dtor order is asserted frame by frame (each resume
// advances one frame), so inlining all three Guards into winstart would
// collapse the walk to a single cleanup.
__attribute__((noinline)) static void f3(void) {
    Guard g(3);
    throw Boom();
}

__attribute__((noinline)) static void f2(void) {
    Guard g(2);
    f3();
}

__attribute__((noinline)) static void f1(void) {
    Guard g(1);
    f2();
}

extern "C" void winstart(void) {
    out_h = GetStdHandle((DWORD)-11);
    PUTLIT("W32A4-CXX-THROW\n");
    f1();
    PUTLIT("W32A4-SURVIVED\n"); // NOTREACHED (uncaught: abort above)
    ExitProcess(44);
}

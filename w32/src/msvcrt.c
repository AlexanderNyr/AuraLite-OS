/* msvcrt.c — W32APP_PLAN.md phase W32A-13: the msvcrt bridge.
 *
 * msvcrt.dll is not reimplemented; it is BRIDGED.  Every symbol here forwards
 * onto a runtime AuraLite already owns:
 *
 *   - malloc/free/realloc  -> GetProcessHeap()/HeapAlloc/HeapFree/HeapReAlloc
 *     (W32A-2).  This is the phase's load-bearing invariant: a CRT pointer IS
 *     a process-heap pointer, so HeapSize(malloc(n)) == n and HeapReAlloc on a
 *     malloc'd block is legal.  Two heaps pretending to be one is the exact bug
 *     this forbids.
 *   - _beginthreadex        -> CreateThread (W32A-3), CREATE_SUSPENDED and the
 *     thread-id out-parameter preserved.
 *   - the C++ EH names       -> W32A-4's unwinder.  _CxxThrowException,
 *     ?terminate, _purecall, _XcptFilter and __C_specific_handler are bound in
 *     w32_bind.c to W32A-4's real implementations; this file adds
 *     __CxxFrameHandler (the honest D7 gap: continue-search so cleanups run and
 *     the death is NAMED, never a fabricated catch) and type_info's destructor.
 *   - the string/memory core is self-contained (freestanding-safe, volatile
 *     byte loops so the compiler cannot fold them back into a memcpy call).
 *   - startup/exit (__getmainargs/_initterm/_onexit/__dllonexit/exit family)
 *     is REAL, with the exit-code paths kept distinct (callbacks-vs-not,
 *     terminate-vs-return) and _acmdln/_fmode/_commode as REAL data exports.
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "w32/msvcrt.h"
#include "w32/kernel32.h"   /* Heap*, GetProcessHeap, CreateThread, ExitProcess,
                             * GetCommandLineA, W32_THREAD_START */
#include "w32/w32_seh.h"    /* W32_EXCEPTION_CONTINUE_SEARCH_NT */
#include "w32/w32_argv.h"   /* w32_cmdline_to_argv (the proven splitter) */

/* ======================================================================== *
 *  Heap unity — the phase invariant.                                        *
 * ======================================================================== */

W32ABI void *w32_msvcrt_malloc(size_t size) {
    /* HeapAlloc bumps a zero request to a unique 1-byte block, which is
     * exactly what MSVCRT malloc(0) promises: a distinct, freeable pointer. */
    return HeapAlloc(GetProcessHeap(), 0, (unsigned long long)size);
}

W32ABI void w32_msvcrt_free(void *ptr) {
    if (ptr) HeapFree(GetProcessHeap(), 0, ptr);   /* free(NULL) is a no-op */
}

W32ABI void *w32_msvcrt_realloc(void *ptr, size_t size) {
    W32_HANDLE h = GetProcessHeap();
    if (!ptr)      return HeapAlloc(h, 0, (unsigned long long)size);
    if (size == 0) { HeapFree(h, 0, ptr); return 0; }   /* realloc(p,0)==free */
    return HeapReAlloc(h, 0, ptr, (unsigned long long)size);
}

/* ======================================================================== *
 *  Strings / memory.  Volatile byte loops: -ffreestanding lets the compiler *
 *  recognise a plain copy loop and emit a call to memcpy/memset, which here  *
 *  would be a call back INTO this very function.  volatile forbids that.     *
 * ======================================================================== */

W32ABI void *w32_msvcrt_memset(void *d, int c, size_t n) {
    volatile unsigned char *p = (volatile unsigned char *)d;
    while (n-- > 0) *p++ = (unsigned char)c;
    return d;
}

W32ABI void *w32_msvcrt_memcpy(void *d, const void *s, size_t n) {
    volatile unsigned char *p = (volatile unsigned char *)d;
    volatile const unsigned char *q = (volatile const unsigned char *)s;
    while (n-- > 0) *p++ = *q++;
    return d;
}

W32ABI void *w32_msvcrt_memmove(void *d, const void *s, size_t n) {
    volatile unsigned char *p = (volatile unsigned char *)d;
    volatile const unsigned char *q = (volatile const unsigned char *)s;
    if (p == q || n == 0) return d;
    if (p < q) {
        while (n-- > 0) *p++ = *q++;
    } else {
        p += n; q += n;
        while (n-- > 0) *--p = *--q;
    }
    return d;
}

W32ABI int w32_msvcrt_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

W32ABI size_t w32_msvcrt_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

W32ABI int w32_msvcrt_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

W32ABI char *w32_msvcrt_strchr(const char *s, int c) {
    char ch = (char)c;
    for (;; s++) {
        if (*s == ch) return (char *)s;   /* c==0 => pointer to the NUL */
        if (!*s) return 0;
    }
}

W32ABI char *w32_msvcrt_strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *a = hay, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)hay;
    }
    return 0;
}

W32ABI size_t w32_msvcrt_wcslen(const uint16_t *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

W32ABI int w32_msvcrt_wcscmp(const uint16_t *a, const uint16_t *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)*a - (int)*b;
}

W32ABI uint16_t *w32_msvcrt_wcsstr(const uint16_t *hay, const uint16_t *needle) {
    if (!*needle) return (uint16_t *)hay;
    for (; *hay; hay++) {
        const uint16_t *a = hay, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (uint16_t *)hay;
    }
    return 0;
}

/* ======================================================================== *
 *  Pseudo-random.  The exact MSVCRT linear congruential generator so a       *
 *  seeded sequence matches Windows bit for bit (RAND_MAX == 0x7fff).         *
 * ======================================================================== */

static uint32_t g_rand_seed = 1;

W32ABI void w32_msvcrt_srand(unsigned int seed) { g_rand_seed = (uint32_t)seed; }

W32ABI int w32_msvcrt_rand(void) {
    g_rand_seed = g_rand_seed * 214013u + 2531011u;   /* wraps mod 2^32 */
    return (int)((g_rand_seed >> 16) & 0x7fff);
}

/* ======================================================================== *
 *  Startup / exit.                                                          *
 * ======================================================================== */

/* An empty, NULL-terminated environment block: __getmainargs hands *env this
 * so a caller that walks envp[] terminates immediately. */
static char *g_env0[1] = { 0 };

W32ABI int w32_msvcrt_getmainargs(int *argc, char ***argv, char ***env,
                                  int expand_wildcards, int *new_mode) {
    const char *cl = GetCommandLineA();
    size_t need_argc = 0, need_bytes = 0;
    char **vec;
    char  *buf;
    (void)expand_wildcards;   /* no globbing: the shell already expanded */
    (void)new_mode;
    if (!cl) cl = "";

    /* Measure, then allocate FROM THE PROCESS HEAP, then fill: identical
     * parsing on both passes, so the two sizes cannot disagree. */
    w32_cmdline_to_argv(cl, 0, 0, 0, 0, &need_argc, &need_bytes);
    vec = (char **)w32_msvcrt_malloc((need_argc + 1) * sizeof(char *));
    buf = (char  *)w32_msvcrt_malloc(need_bytes ? need_bytes : 1);
    if (!vec || !buf) {
        w32_msvcrt_free(vec);
        w32_msvcrt_free(buf);
        if (argc) *argc = 0;
        if (argv) *argv = 0;
        if (env)  *env  = g_env0;
        return -1;
    }
    w32_cmdline_to_argv(cl, vec, need_argc, buf, need_bytes,
                        &need_argc, &need_bytes);
    vec[need_argc] = 0;                 /* argv is NULL-terminated too */
    if (argc) *argc = (int)need_argc;
    if (argv) *argv = vec;
    if (env)  *env  = g_env0;
    return 0;
}

W32ABI void w32_msvcrt_initterm(w32_PVFV *first, w32_PVFV *last) {
    if (!first || !last) return;
    for (w32_PVFV *p = first; p < last; ++p)
        if (*p) (*p)();               /* skip the NULL padding cells */
}

/* The process-wide onexit/atexit chain.  LAST-registered runs FIRST. */
#define W32_ONEXIT_MAX 64
static w32_onexit_t g_onexit[W32_ONEXIT_MAX];
static int          g_onexit_n;

W32ABI w32_onexit_t w32_msvcrt_onexit(w32_onexit_t func) {
    if (!func || g_onexit_n >= W32_ONEXIT_MAX) return 0;
    g_onexit[g_onexit_n++] = func;
    return func;
}

int w32_msvcrt_run_atexit(void) {
    int ran = 0;
    while (g_onexit_n > 0) {
        w32_onexit_t f = g_onexit[--g_onexit_n];
        if (f) { f(); ran++; }
    }
    return ran;
}

/* __dllonexit: the CRT helper a per-DLL atexit table uses.  It appends func to
 * the caller-owned table delimited by [*pbegin, *pend) and returns func.  The
 * table itself lives on the process heap, so growth stays inside heap unity. */
W32ABI w32_onexit_t w32_msvcrt_dllonexit(w32_onexit_t func,
                                         w32_PVFV **pbegin, w32_PVFV **pend) {
    size_t   n;
    w32_PVFV *base;
    if (!func || !pbegin || !pend) return 0;
    n = (size_t)(*pend - *pbegin);
    base = (w32_PVFV *)w32_msvcrt_realloc(*pbegin, (n + 1) * sizeof *base);
    if (!base) return 0;
    base[n] = (w32_PVFV)func;
    *pbegin = base;
    *pend   = base + n + 1;
    return func;
}

W32ABI void w32_msvcrt_exit(int code) {
    w32_msvcrt_run_atexit();          /* callbacks + (a no-op) flush */
    ExitProcess((unsigned int)code);
    __builtin_unreachable();
}

W32ABI void w32_msvcrt__exit(int code) {
    ExitProcess((unsigned int)code);  /* no callbacks, no flush */
    __builtin_unreachable();
}

W32ABI void w32_msvcrt_cexit(void)  { w32_msvcrt_run_atexit(); } /* returns */
W32ABI void w32_msvcrt_c_exit(void) { /* no callbacks, no flush */ }

static int   g_app_type;
static void *g_usermatherr;
W32ABI void w32_msvcrt_set_app_type(int app_type)     { g_app_type = app_type; }
W32ABI void w32_msvcrt_setusermatherr(void *handler)  { g_usermatherr = handler; }

/* ======================================================================== *
 *  Threading — onto W32A-3's CreateThread.                                  *
 * ======================================================================== */

W32ABI uintptr_t w32_msvcrt_beginthreadex(void *security, unsigned stack_size,
                                          void *start, void *arglist,
                                          unsigned initflag, unsigned *thrdaddr) {
    /* _beginthreadex's start is `unsigned __stdcall(void*)`; CreateThread's is
     * `DWORD WINAPI(void*)` — same ms_abi shape, same 32-bit return. */
    W32_DWORD  tid = 0;
    W32_HANDLE h = CreateThread(security, (W32_SIZE_T)stack_size,
                                (W32_THREAD_START)start, arglist,
                                (W32_DWORD)initflag, &tid);
    if (!h) return 0;
    if (thrdaddr) *thrdaddr = (unsigned)tid;
    return (uintptr_t)h;
}

/* ======================================================================== *
 *  C++ EH residue (D7).  Honest: cleanups run, then the death is NAMED.     *
 * ======================================================================== */

/* __CxxFrameHandler: the language handler MSVC registers in a C++ frame's
 * xdata.  We never establish a catch (catch matching is the documented D7
 * gap — a wrong catch is worse than a named death), so we return
 * continue-search: the unwinder keeps sweeping cleanups upward until it hits
 * the unhandled path, which terminates with a NAMED reason. */
W32ABI int32_t w32_msvcrt_CxxFrameHandler(void *record, uint64_t frame,
                                          void *context, void *dispatch) {
    (void)record; (void)frame; (void)context; (void)dispatch;
    return W32_EXCEPTION_CONTINUE_SEARCH_NT;
}

/* type_info::~type_info(): a virtual no-op destructor.  Teardown of a typeinfo
 * object is not catch matching, so it is implemented (returns `this`, the
 * scalar-deleting-destructor convention; a void caller ignores rax). */
W32ABI void *w32_msvcrt_type_info_dtor(void *self) { return self; }

/* ======================================================================== *
 *  Data exports.  Bound as the ADDRESS of these cells (the importer reads    *
 *  them through __imp__<name>).                                              *
 * ======================================================================== */

char *w32_msvcrt_acmdln  = 0;   /* _acmdln:  the ANSI command line          */
int   w32_msvcrt_fmode   = 0;   /* _fmode:   _O_TEXT (0) — the CRT default  */
int   w32_msvcrt_commode = 0;   /* _commode: 0 — no commit-on-flush         */

void w32_msvcrt_init(void) {
    /* Called from w32_kernel32_init after the command line is built, so
     * _acmdln points at the same buffer GetCommandLineA returns. */
    w32_msvcrt_acmdln = (char *)GetCommandLineA();
}

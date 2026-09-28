/* W32A-13: host unit test for the msvcrt bridge's ABI-boundary logic.
 *
 * The in-guest QEMU gate (test_w32a13_msvcrt.sh) proves the bridge REAL over
 * the personality (heap unity against kernel32's own HeapSize, _beginthreadex
 * over CreateThread, the CRT startup/exit path).  This test amalgamates
 * msvcrt.c (and the real w32_argv.c splitter it uses) against hand-written
 * kernel32 heap/thread/exit doubles and exercises the parts that are pure
 * boundary logic:
 *
 *   - HEAP UNITY: malloc/free/realloc route through GetProcessHeap()/HeapAlloc/
 *     HeapFree/HeapReAlloc, so HeapSize(malloc(n)) == n and HeapReAlloc works
 *     on a malloc'd pointer -- the double tracks sizes exactly like kernel32.
 *   - the string/memory core (incl. overlapping memmove, strchr(...,0),
 *     empty-needle strstr, the wide variants);
 *   - the exact MSVCRT rand LCG (seeded reproducibility, RAND_MAX 0x7fff,
 *     the well-known srand(1)->41 first draw);
 *   - __getmainargs over GetCommandLineA + the real splitter (argc, argv
 *     pointing into one buffer, a NULL-terminated empty environment);
 *   - _initterm walking a function table (NULL cells skipped, in order);
 *   - the onexit/atexit chain LIFO and the four exit-code paths kept distinct
 *     (exit: callbacks+terminate; _exit: terminate only; _cexit: callbacks+
 *     return; _c_exit: neither) via an ExitProcess double that longjmps back;
 *   - __dllonexit growing a caller-owned table on the process heap;
 *   - _beginthreadex forwarding start/arg/flags/tid onto CreateThread;
 *   - the C++ EH residue: __CxxFrameHandler continue-search, type_info dtor.
 *
 * Sanitizers on.
 *
 * SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>

#include "w32/w32_abi.h"
#include "w32/kernel32.h"

/* ---- a size-tracking heap double, mirroring kernel32's k32_heap ----------- */
#define DBL_HEAP_TOKEN ((W32_HANDLE)(intptr_t)0x48454150)
#define DBL_SLOTS 512
static struct { int in_use; void *ptr; unsigned long long size; } dbl_heap[DBL_SLOTS];

static void dbl_rec(void *p, unsigned long long sz) {
    if (!p) return;
    for (int i = 0; i < DBL_SLOTS; i++)
        if (!dbl_heap[i].in_use) { dbl_heap[i].in_use = 1; dbl_heap[i].ptr = p;
                                   dbl_heap[i].size = sz; return; }
}
static void dbl_drop(void *p) {
    if (!p) return;
    for (int i = 0; i < DBL_SLOTS; i++)
        if (dbl_heap[i].in_use && dbl_heap[i].ptr == p) { dbl_heap[i].in_use = 0; return; }
}
static int dbl_find(const void *p, unsigned long long *out) {
    for (int i = 0; i < DBL_SLOTS; i++)
        if (dbl_heap[i].in_use && dbl_heap[i].ptr == p) { *out = dbl_heap[i].size; return 1; }
    return 0;
}

W32ABI W32_HANDLE GetProcessHeap(void) { return DBL_HEAP_TOKEN; }

W32ABI void *HeapAlloc(W32_HANDLE heap, W32_DWORD flags, unsigned long long size) {
    void *p;
    if (heap != DBL_HEAP_TOKEN) return 0;
    if (size == 0) size = 1;                 /* Win32 unique-block semantics */
    p = malloc((size_t)size);
    if (!p) return 0;
    if (flags & 0x8u) memset(p, 0, (size_t)size);
    dbl_rec(p, size);
    return p;
}
W32ABI W32_BOOL HeapFree(W32_HANDLE heap, W32_DWORD flags, void *mem) {
    (void)flags;
    if (heap != DBL_HEAP_TOKEN) return 0;
    if (mem) { dbl_drop(mem); free(mem); }
    return 1;
}
W32ABI void *HeapReAlloc(W32_HANDLE heap, W32_DWORD flags, void *mem,
                         unsigned long long size) {
    unsigned long long old = 0; void *p;
    if (heap != DBL_HEAP_TOKEN) return 0;
    if (!mem) return HeapAlloc(heap, flags, size);
    if (size == 0) { HeapFree(heap, flags, mem); return 0; }
    dbl_find(mem, &old); dbl_drop(mem);
    p = realloc(mem, (size_t)size);
    if (!p) { dbl_rec(mem, old); return 0; }
    dbl_rec(p, size);
    if ((flags & 0x8u) && size > old) memset((char *)p + old, 0, (size_t)(size - old));
    return p;
}
W32ABI W32_SIZE_T HeapSize(W32_HANDLE heap, W32_DWORD flags, const void *mem) {
    unsigned long long sz = 0; (void)flags;
    if (heap != DBL_HEAP_TOKEN || !mem) return (W32_SIZE_T)-1;
    if (!dbl_find(mem, &sz)) return (W32_SIZE_T)-1;
    return (W32_SIZE_T)sz;
}

/* ---- CreateThread double: runs the start routine unless suspended --------- */
static int      dbl_thr_ran;
static void    *dbl_thr_arg;
static W32_DWORD dbl_thr_flags;
static W32_DWORD dbl_thr_ret;
W32ABI W32_HANDLE CreateThread(void *security, W32_SIZE_T stack_size,
                               W32_THREAD_START start, void *param,
                               W32_DWORD flags, W32_DWORD *tid_out) {
    (void)security; (void)stack_size;
    dbl_thr_flags = flags;
    dbl_thr_arg   = param;
    if (tid_out) *tid_out = 4242;
    if (!(flags & W32_CREATE_SUSPENDED) && start) {
        dbl_thr_ret = start(param);
        dbl_thr_ran = 1;
    }
    return (W32_HANDLE)(intptr_t)0xC0FFEE;
}

/* ---- ExitProcess double: record the code and longjmp back ----------------- */
static jmp_buf  dbl_exit_jb;
static int      dbl_exit_code;
W32ABI void ExitProcess(unsigned int code) {
    dbl_exit_code = (int)code;
    longjmp(dbl_exit_jb, 1);
    __builtin_unreachable();
}

/* ---- GetCommandLineA double ----------------------------------------------- */
static const char *dbl_cmdline = "prog.exe alpha \"b c\" delta";
W32ABI const char *GetCommandLineA(void) { return dbl_cmdline; }

/* ---- the real command-line splitter + the unit under test ----------------- */
#include "../../w32/src/w32_argv.c"
#include "../../w32/src/msvcrt.c"

/* ---- test scaffolding ----------------------------------------------------- */
static int n, f;
#define CHECK(x) do { ++n; if (!(x)) { ++f; \
    fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

/* onexit / initterm observers */
static char g_log[32];
static int  g_log_n;
static void logc(char c) { if (g_log_n < (int)sizeof g_log) g_log[g_log_n++] = c; }
static int W32ABI cbA(void) { logc('A'); return 0; }
static int W32ABI cbB(void) { logc('B'); return 0; }
static int W32ABI cbC(void) { logc('C'); return 0; }
static void W32ABI itA(void) { logc('1'); }
static void W32ABI itB(void) { logc('2'); }

int main(void) {
    /* ---- 1. heap unity ---------------------------------------------------- */
    {
        char *p = (char *)w32_msvcrt_malloc(100);
        CHECK(p != 0);
        CHECK(HeapSize(GetProcessHeap(), 0, p) == 100);   /* the invariant */
        for (int i = 0; i < 100; i++) p[i] = (char)i;
        char *q = (char *)w32_msvcrt_realloc(p, 200);
        CHECK(q != 0);
        CHECK(HeapSize(GetProcessHeap(), 0, q) == 200);
        int preserved = 1;
        for (int i = 0; i < 100; i++) if (q[i] != (char)i) preserved = 0;
        CHECK(preserved);                                  /* realloc kept data */
        /* HeapReAlloc directly on a CRT pointer is legal (same heap). */
        char *r = (char *)HeapReAlloc(GetProcessHeap(), 0, q, 40);
        CHECK(r != 0);
        CHECK(HeapSize(GetProcessHeap(), 0, r) == 40);
        w32_msvcrt_free(r);

        void *z = w32_msvcrt_malloc(0);                    /* malloc(0): unique */
        CHECK(z != 0);
        w32_msvcrt_free(z);
        CHECK(w32_msvcrt_realloc(0, 50) != 0);             /* realloc(NULL)==malloc */
        void *rp = w32_msvcrt_malloc(8);
        CHECK(w32_msvcrt_realloc(rp, 0) == 0);             /* realloc(p,0)==free */
        w32_msvcrt_free(0);                                /* free(NULL) no-op */
    }

    /* ---- 2. strings / memory --------------------------------------------- */
    {
        char b[16];
        CHECK(w32_msvcrt_memset(b, 'x', 5) == b && b[0] == 'x' && b[4] == 'x');
        char src[8] = "abcdefg";
        w32_msvcrt_memcpy(b, src, 8);
        CHECK(memcmp(b, "abcdefg", 8) == 0);
        /* overlapping memmove: shift right by 1 */
        char ov[8] = "12345";
        w32_msvcrt_memmove(ov + 1, ov, 5);
        CHECK(memcmp(ov, "112345", 6) == 0);
        CHECK(w32_msvcrt_memcmp("abc", "abd", 3) < 0);
        CHECK(w32_msvcrt_memcmp("abc", "abc", 3) == 0);
        CHECK(w32_msvcrt_strlen("hello") == 5);
        CHECK(w32_msvcrt_strcmp("abc", "abc") == 0);
        CHECK(w32_msvcrt_strcmp("abc", "abd") < 0);
        const char *hp = "hello";
        CHECK(w32_msvcrt_strchr(hp, 'l') == hp + 2);
        CHECK(w32_msvcrt_strchr(hp, 0) == hp + 5);         /* c==0 -> the NUL */
        CHECK(w32_msvcrt_strchr(hp, 'z') == 0);
        const char *hw = "hello world";
        CHECK(w32_msvcrt_strstr(hw, "world") == hw + 6);
        CHECK(w32_msvcrt_strstr(hw, "") == hw);            /* empty needle */
        CHECK(w32_msvcrt_strstr(hw, "xyz") == 0);
        static const uint16_t wa[] = { 'h','i',0 };
        static const uint16_t wb[] = { 'h','i',0 };
        static const uint16_t wc[] = { 'h','o',0 };
        static const uint16_t whay[] = { 'a','b','c','d',0 };
        static const uint16_t wneed[] = { 'c','d',0 };
        CHECK(w32_msvcrt_wcslen(wa) == 2);
        CHECK(w32_msvcrt_wcscmp(wa, wb) == 0);
        CHECK(w32_msvcrt_wcscmp(wa, wc) < 0);
        CHECK(w32_msvcrt_wcsstr(whay, wneed) == whay + 2);
    }

    /* ---- 3. rand / srand ------------------------------------------------- */
    {
        w32_msvcrt_srand(1);
        int first = w32_msvcrt_rand();
        CHECK(first == 41);                                 /* the MSVCRT LCG */
        int seq1[5]; for (int i = 0; i < 5; i++) seq1[i] = w32_msvcrt_rand();
        w32_msvcrt_srand(1);
        CHECK(w32_msvcrt_rand() == 41);
        int ok = 1; for (int i = 0; i < 5; i++) if (w32_msvcrt_rand() != seq1[i]) ok = 0;
        CHECK(ok);                                          /* reproducible */
        int inrange = 1;
        for (int i = 0; i < 1000; i++) { int v = w32_msvcrt_rand();
            if (v < 0 || v > 0x7fff) inrange = 0; }
        CHECK(inrange);                                     /* RAND_MAX 0x7fff */
    }

    /* ---- 4. startup: __getmainargs / _acmdln / data exports -------------- */
    {
        int argc = -1; char **argv = 0; char **env = 0;
        int rc = w32_msvcrt_getmainargs(&argc, &argv, &env, 0, 0);
        CHECK(rc == 0);
        CHECK(argc == 4);
        CHECK(argv && strcmp(argv[0], "prog.exe") == 0);
        CHECK(argv && strcmp(argv[1], "alpha") == 0);
        CHECK(argv && strcmp(argv[2], "b c") == 0);         /* the quoted arg */
        CHECK(argv && strcmp(argv[3], "delta") == 0);
        CHECK(argv && argv[4] == 0);                        /* NULL-terminated */
        CHECK(env && env[0] == 0);                          /* empty environment */
        w32_msvcrt_init();
        CHECK(w32_msvcrt_acmdln == GetCommandLineA());      /* _acmdln wired */
        CHECK(w32_msvcrt_fmode == 0 && w32_msvcrt_commode == 0);
        w32_msvcrt_set_app_type(2);                         /* trivial, no crash */
        w32_msvcrt_setusermatherr((void *)0);
    }

    /* ---- 5. _initterm ---------------------------------------------------- */
    {
        g_log_n = 0;
        w32_PVFV tab[4] = { itA, 0, itB, 0 };               /* a NULL cell */
        w32_msvcrt_initterm(tab, tab + 4);
        g_log[g_log_n] = 0;
        CHECK(strcmp(g_log, "12") == 0);                    /* in order, NULL skipped */
    }

    /* ---- 6. onexit LIFO + the exit-code path matrix ---------------------- */
    {
        /* run_atexit drains LIFO */
        g_log_n = 0;
        CHECK(w32_msvcrt_onexit(cbA) == cbA);
        w32_msvcrt_onexit(cbB);
        w32_msvcrt_onexit(cbC);
        int ran = w32_msvcrt_run_atexit();
        g_log[g_log_n] = 0;
        CHECK(ran == 3);
        CHECK(strcmp(g_log, "CBA") == 0);                   /* last-in first-out */

        /* exit(): callbacks THEN terminate */
        g_log_n = 0; dbl_exit_code = -1;
        w32_msvcrt_onexit(cbA); w32_msvcrt_onexit(cbB);
        if (setjmp(dbl_exit_jb) == 0) { w32_msvcrt_exit(77); CHECK(0 /*noreturn*/); }
        g_log[g_log_n] = 0;
        CHECK(dbl_exit_code == 77);
        CHECK(strcmp(g_log, "BA") == 0);                    /* callbacks ran */

        /* _exit(): terminate, NO callbacks */
        g_log_n = 0; dbl_exit_code = -1;
        w32_msvcrt_onexit(cbC);                             /* left registered */
        if (setjmp(dbl_exit_jb) == 0) { w32_msvcrt__exit(9); CHECK(0); }
        CHECK(dbl_exit_code == 9);
        CHECK(g_log_n == 0);                                /* nothing ran */
        /* the callback survived _exit: draining now proves it was skipped */
        CHECK(w32_msvcrt_run_atexit() == 1);

        /* _cexit(): callbacks, then RETURN (no terminate) */
        g_log_n = 0; dbl_exit_code = -1;
        w32_msvcrt_onexit(cbA); w32_msvcrt_onexit(cbB);
        w32_msvcrt_cexit();                                 /* returns here */
        g_log[g_log_n] = 0;
        CHECK(strcmp(g_log, "BA") == 0);
        CHECK(dbl_exit_code == -1);                         /* did NOT terminate */

        /* _c_exit(): neither callbacks nor terminate */
        g_log_n = 0; dbl_exit_code = -1;
        w32_msvcrt_onexit(cbC);
        w32_msvcrt_c_exit();                                /* returns here */
        CHECK(g_log_n == 0);
        CHECK(dbl_exit_code == -1);
        CHECK(w32_msvcrt_run_atexit() == 1);               /* cbC still queued */
    }

    /* ---- 7. __dllonexit grows a caller table ---------------------------- */
    {
        w32_PVFV *begin = 0, *end = 0;
        CHECK(w32_msvcrt_dllonexit(cbA, &begin, &end) == cbA);
        CHECK(w32_msvcrt_dllonexit(cbB, &begin, &end) == cbB);
        CHECK(w32_msvcrt_dllonexit(cbC, &begin, &end) == cbC);
        CHECK((size_t)(end - begin) == 3);
        CHECK(begin[0] == (w32_PVFV)cbA);
        CHECK(begin[1] == (w32_PVFV)cbB);
        CHECK(begin[2] == (w32_PVFV)cbC);
        w32_msvcrt_free(begin);
    }

    /* ---- 8. _beginthreadex over CreateThread ---------------------------- */
    {
        dbl_thr_ran = 0; dbl_thr_ret = 0;
        unsigned tid = 0;
        uintptr_t h = w32_msvcrt_beginthreadex(0, 0, (void *)cbC,
                                               (void *)0x1234, 0, &tid);
        CHECK(h == (uintptr_t)0xC0FFEE);
        CHECK(tid == 4242);
        CHECK(dbl_thr_ran == 1);
        CHECK(dbl_thr_arg == (void *)0x1234);
        /* CREATE_SUSPENDED is passed straight through, not run here */
        dbl_thr_ran = 0;
        w32_msvcrt_beginthreadex(0, 0, (void *)cbC, 0, W32_CREATE_SUSPENDED, &tid);
        CHECK((dbl_thr_flags & W32_CREATE_SUSPENDED) != 0);
        CHECK(dbl_thr_ran == 0);
    }

    /* ---- 9. C++ EH residue ---------------------------------------------- */
    {
        CHECK(w32_msvcrt_CxxFrameHandler(0, 0, 0, 0) == 1); /* continue-search */
        int x = 5;
        CHECK(w32_msvcrt_type_info_dtor(&x) == &x);         /* dtor returns this */
    }

    fprintf(stderr, "w32a13-msvcrt: %d checks, %d failures\n", n, f);
    return f ? 1 : 0;
}

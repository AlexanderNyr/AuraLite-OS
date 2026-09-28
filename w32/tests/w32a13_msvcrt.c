/* w32/tests/w32a13_msvcrt.c — W32APP_PLAN.md phase W32A-13 guest fixture.
 *
 * A single mingw-w64 TU linked -nostdlib with --entry=winstart (no CRT, no
 * main): it imports msvcrt BY NAME (a real -lmsvcrt link) and KERNEL32 for
 * output/heap/threads, exercises the msvcrt bridge REAL over the personality,
 * and exits 78 on success / 1 on failure.
 *
 * The headline proof is HEAP UNITY across two DLLs: a block from msvcrt.dll!
 * malloc is measured by kernel32.dll!HeapSize and they AGREE, and
 * kernel32.dll!HeapReAlloc grows a msvcrt malloc'd pointer.  Two heaps
 * pretending to be one would fail here.  It also drives the string/memory
 * core, the seeded rand LCG, _beginthreadex over CreateThread (join +
 * exit code), and the _onexit chain: the exit(78) that ends the run flows
 * through msvcrt.dll!exit, which runs the LIFO callbacks and then ExitProcess.
 *
 * Markers greppable by tests/integration/cases/test_w32a13_msvcrt.sh.
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <windows.h>
#include <process.h>   /* _beginthreadex */
#include <stdlib.h>    /* malloc/free/realloc/exit/rand/srand/_onexit */
#include <string.h>    /* memcpy/memset/memcmp/strlen/strcmp/strchr/strstr */
#include <stdint.h>

static HANDLE out;
static DWORD  written;
static int    fails;

static void say(const char *s) {
    DWORD k = 0;
    while (s[k]) k++;
    WriteFile(out, s, k, &written, NULL);
}
static void mark_fail(const char *m) { say("FAIL-"); say(m); say("\r\n"); fails++; }
#define CHECK(cond, mark) do { if (!(cond)) mark_fail(mark); } while (0)

/* _onexit callbacks: emit a marker each so the chain is visible in the log. */
static int __cdecl oxA(void) { say("OX-A\r\n"); return 0; }
static int __cdecl oxB(void) { say("OX-B\r\n"); return 0; }

/* _beginthreadex start routine. */
static unsigned __stdcall worker(void *arg) {
    unsigned v = (unsigned)(uintptr_t)arg;
    return v + 1;   /* the join asserts this exit code */
}

void __stdcall winstart(void) {
    out = GetStdHandle((DWORD)-11);   /* STD_OUTPUT_HANDLE */
    fails = 0;

    say("W32A13: msvcrt fixture start\r\n");

    /* ---- heap unity across msvcrt(malloc) and kernel32(HeapSize) -------- */
    {
        HANDLE heap = GetProcessHeap();
        char *p = (char *)malloc(100);
        CHECK(p != NULL, "malloc");
        CHECK(HeapSize(heap, 0, p) == 100, "heapsize-agrees");   /* the invariant */
        for (int i = 0; i < 100; i++) p[i] = (char)i;
        char *q = (char *)realloc(p, 200);
        CHECK(q != NULL, "realloc");
        CHECK(HeapSize(heap, 0, q) == 200, "heapsize-after-realloc");
        int kept = 1;
        for (int i = 0; i < 100; i++) if (q[i] != (char)i) kept = 0;
        CHECK(kept, "realloc-preserved");
        /* kernel32 grows a msvcrt pointer: same heap, both directions. */
        char *r = (char *)HeapReAlloc(heap, 0, q, 40);
        CHECK(r != NULL, "heaprealloc-crt-ptr");
        CHECK(HeapSize(heap, 0, r) == 40, "heapsize-crossgrow");
        free(r);
        void *z = malloc(0);
        CHECK(z != NULL, "malloc-zero");     /* unique block, not NULL */
        free(z);
    }

    /* ---- string / memory core ------------------------------------------- */
    {
        char b[16];
        memset(b, 'x', 5);
        CHECK(b[0] == 'x' && b[4] == 'x', "memset");
        memcpy(b, "abcdefg", 8);
        CHECK(memcmp(b, "abcdefg", 8) == 0, "memcpy-memcmp");
        CHECK(strlen("hello") == 5, "strlen");
        CHECK(strcmp("abc", "abc") == 0 && strcmp("abc", "abd") < 0, "strcmp");
        const char *hw = "hello world";
        CHECK(strchr(hw, 'w') == hw + 6, "strchr");
        CHECK(strstr(hw, "world") == hw + 6, "strstr");
    }

    /* ---- rand / srand reproducibility ----------------------------------- */
    {
        srand(1);
        int first = rand();
        CHECK(first == 41, "rand-lcg");           /* the MSVCRT LCG, seed 1 */
        int seq[4]; for (int i = 0; i < 4; i++) seq[i] = rand();
        srand(1);
        CHECK(rand() == 41, "rand-reseed");
        int ok = 1; for (int i = 0; i < 4; i++) if (rand() != seq[i]) ok = 0;
        CHECK(ok, "rand-reproducible");
    }

    /* ---- _beginthreadex over CreateThread ------------------------------- */
    {
        unsigned tid = 0;
        uintptr_t h = _beginthreadex(NULL, 0, worker, (void *)(uintptr_t)54,
                                     0, &tid);
        CHECK(h != 0, "beginthreadex");
        CHECK(WaitForSingleObject((HANDLE)h, 5000) == 0 /*WAIT_OBJECT_0*/,
              "thread-join");
        DWORD code = 0;
        CHECK(GetExitCodeThread((HANDLE)h, &code) != 0, "thread-getexitcode");
        CHECK(code == 55, "thread-exit-value");   /* 54 + 1 */
        CloseHandle((HANDLE)h);
    }

    /* ---- _onexit chain, then exit(78) through msvcrt --------------------- */
    /* Registered A then B: exit() runs them LIFO (B, then A). */
    _onexit(oxA);
    _onexit(oxB);

    if (fails == 0) {
        say("W32A13-MSVCRT-OK\r\n");
        exit(78);                 /* msvcrt.dll!exit: onexit chain + ExitProcess */
    }
    say("W32A13-MSVCRT-FAIL\r\n");
    exit(1);
}

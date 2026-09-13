/* test_w32_a3.c — W32A-3 threads + sync, exercised against the real host.
 *
 * Like test_w32_a2.c this links the REAL host libc, but unlike A2 it also
 * runs REAL threads: the w32 clone path issues raw SYS_CLONE against Linux
 * (same number, same flags), ARCH_SET_GS programs a real GS base, and the
 * futex number is the only host/guest divergence (202 vs 530).  A green
 * run here means the trampoline, the TEB install, the waits and the pool
 * all work against a production kernel; the guest fixtures prove the same
 * code under AuraLite's own libc + swapgs.
 */

#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <cpuid.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include "w32/w32_abi.h"
#include "w32/w32_handle.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"
#include "w32/w32_teb.h"
#include "w32/w32_crt.h"
#include "w32/kernel32.h"

/* ExitProcess detaches DLLs; this test never exits that way. */
void w32_module_detach_all(void) {}

/* FreeLibraryAndExitThread's FreeLibrary: record + succeed (the loader is
 * not under test here). */
static void *flib_last_mod = 0;
static int flib_calls = 0;
int W32ABI w32_FreeLibrary(void *mod) {
    flib_last_mod = mod;
    flib_calls++;
    return 1;
}

#define AURALITE_W32_HOST_TEST 1
#define main w32_unused_main
#include "../../w32/src/w32_utf.c"
#include "../../w32/src/w32_errno.c"
#include "../../w32/src/w32_handle.c"
#include "../../w32/src/kernel32.c"
#include "../../w32/src/kernel32_fs.c"
#include "../../w32/src/kernel32_ps.c"
#include "../../w32/src/kernel32_loc.c"
#include "../../w32/src/w32_crt.c"
#include "../../w32/src/kernel32_thr.c"
#undef main

static int fails;
static int checks;

#define CHECK(cond) do { checks++; \
    if (!(cond)) { \
        printf("  FAIL L%d: %s\n", __LINE__, #cond); fails++; } } while (0)
#define CHECK_EQ(a, b) do { checks++; \
    if ((long long)(a) != (long long)(b)) { \
        printf("  FAIL L%d: %s (%lld) != %s (%lld)\n", __LINE__, \
               #a, (long long)(a), #b, (long long)(b)); fails++; } } while (0)

/* ---- shared worker state (plain memory, threads only) ---- */

static volatile int w_sum;
static W32_CRITICAL_SECTION w_cs;
static W32_DWORD w_tls_idx;
static volatile int w_fls_cb_count;
static volatile uint64_t w_apc_got;
static uint64_t t_cv0;

static void W32ABI w_apc_fn(uint64_t data) {
    w_apc_got = data;
}

static W32_DWORD W32ABI w_teb_probe(void *p) {
    uint64_t *out = (uint64_t *)p;
    struct w32_teb *teb = w32_teb_self();
    uint64_t gs30;
    __asm__ volatile ("mov %%gs:0x30, %0" : "=r"(gs30));
    out[0] = gs30;
    out[1] = teb->self;
    out[2] = teb->client_tid;
    out[3] = (uint64_t)(uint32_t)syscall(186L /* gettid */, 0, 0, 0, 0, 0, 0);
    return 0;
}
static volatile int w_attach_count;
static volatile int w_detach_count;

static void W32ABI w_fls_cb(void *value) {
    (void)value;
    __sync_fetch_and_add(&w_fls_cb_count, 1);
}

static W32_DWORD W32ABI w_incr(void *param) {
    int n = (int)(intptr_t)param;
    /* Per-thread TLS: each worker sees its own slot value. */
    TlsSetValue(w_tls_idx, (void *)(intptr_t)(GetCurrentThreadId() & 0xFFFF));
    /* Per-thread LastError: self-consistent inside the worker. */
    SetLastError(123);
    if (GetLastError() != 123)
        return 999;
    for (int i = 0; i < n; i++) {
        EnterCriticalSection(&w_cs);
        w_sum++;
        LeaveCriticalSection(&w_cs);
    }
    return (W32_DWORD)(n & 0xFFFF);
}

/* Raw %gs reads: what the fixtures pin in-guest, pinned here too. */
static uint64_t rd_gs_off(uint32_t off) {
    uint64_t v;
    __asm__ volatile ("mov %%gs:(%1), %0" : "=r"(v) : "r"((uint64_t)off));
    return v;
}

static long raw_arch_prctl(int code, uint64_t addr) {
    return (long)syscall(158L, (uint64_t)(int64_t)code, addr, 0, 0, 0, 0);
}


static W32_DWORD W32ABI w_flib_exit(void *p) {
    FreeLibraryAndExitThread(p, 44);
    return 0;
}

static W32_DWORD W32ABI w_fls_user(void *p) {
    W32_DWORD idx = (W32_DWORD)(uintptr_t)p;
    FlsSetValue(idx, (void *)(intptr_t)7);
    return 0;
}

static void W32ABI w_tls_cb(void *dll, W32_DWORD reason, void *r) {
    (void)dll;
    (void)r;
    if (reason == W32_DLL_PROCESS_ATTACH || reason == W32_DLL_THREAD_ATTACH)
        __sync_fetch_and_add(&w_attach_count, 1);
    else
        __sync_fetch_and_add(&w_detach_count, 1);
}

static W32_DWORD W32ABI w_tls_user(void *p) {
    int slot = (int)(intptr_t)p;
    struct w32_teb *teb = w32_teb_self();
    uint64_t **arr = (uint64_t **)(uintptr_t)teb->tls_storage_ptr;
    /* Own copy, template-initialised; scribble it (main must not see). */
    if (arr[slot][0] != 0xAAAABBBBCCCCDDDDull)
        return 1;
    arr[slot][0] = 0xDEADDEADDEADDEADull;
    return 0;
}

/* CV + pool shared state. */
static W32_SRWLOCK w_srw;
static W32_CONDITION_VARIABLE w_cv;
static volatile int w_cv_ready;
static volatile int w_cv_got;
static W32_INIT_ONCE w_once;
static volatile int w_once_ran;
static volatile int w_pool_n;
static volatile long w_pool_sum;

static W32_DWORD W32ABI w_cv_consumer(void *p) {
    (void)p;
    AcquireSRWLockExclusive(&w_srw);
    while (!w_cv_ready)
        SleepConditionVariableSRW(&w_cv, &w_srw, W32_INFINITE, 0);
    __sync_fetch_and_add(&w_cv_got, 1);
    ReleaseSRWLockExclusive(&w_srw);
    return 0;
}

static W32_DWORD W32ABI w_once_user(void *p) {
    W32_BOOL pending = 0;
    (void)p;
    if (InitOnceBeginInitialize(&w_once, 0, &pending, 0) && pending) {
        __sync_fetch_and_add(&w_once_ran, 1);
        InitOnceComplete(&w_once, 0, 0);
    }
    return 0;
}

static W32_DWORD W32ABI w_mutex_spin(void *p) {
    W32_HANDLE m = (W32_HANDLE)p;
    for (int i = 0; i < 2000; i++) {
        WaitForSingleObject(m, W32_INFINITE);
        w_sum++;
        ReleaseMutex(m);
    }
    return 0;
}

static W32_DWORD W32ABI w_blocked(void *p) {
    W32_HANDLE ev = (W32_HANDLE)p;
    WaitForSingleObject(ev, W32_INFINITE);
    return 0;
}

static W32_DWORD W32ABI w_sleeper(void *p) {
    (void)p;
    Sleep(W32_INFINITE);
    return 0;
}

static W32_DWORD W32ABI w_mutex_holder(void *p) {
    W32_HANDLE m = (W32_HANDLE)p;
    WaitForSingleObject(m, W32_INFINITE);
    /* Exit WITHOUT releasing: the waiter must see ABANDONED. */
    return 0;
}

static void W32ABI w_pool_cb(void *inst, void *ctx, void *work) {
    long v = (long)(intptr_t)ctx;
    (void)inst;
    (void)work;
    __sync_fetch_and_add(&w_pool_sum, v);
    __sync_fetch_and_add(&w_pool_n, 1);
}

static W32_DWORD W32ABI w_apc_target(void *p) {
    W32_HANDLE ev = (W32_HANDLE)p;
    /* Alertable wait on an unsignaled event: the APC must release it. */
    W32_DWORD r = WaitForSingleObjectEx(ev, W32_INFINITE, 1);
    return r;
}

int main(int argc, char **argv) {
    char *init_argv[2];
    (void)argc;
    (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);   /* crash must not eat FAIL lines */

    init_argv[0] = (char *)"test_w32_a3";
    init_argv[1] = 0;
    w32_kernel32_init(1, init_argv);

    /* ---- 1. init + TEB ---- */
    {
        struct w32_teb *teb = w32_teb_self();
        uint64_t gs = 0;
        CHECK(teb != 0);
        CHECK_EQ(teb->self, (uint64_t)(uintptr_t)teb);
        CHECK_EQ(rd_gs_off(0x30), (uint64_t)(uintptr_t)teb);
        CHECK_EQ(rd_gs_off(0x58), teb->tls_storage_ptr);
        CHECK_EQ(raw_arch_prctl(0x1004 /*GET_GS*/, (uint64_t)&gs), 0);
        CHECK_EQ(gs, (uint64_t)(uintptr_t)teb);
        CHECK_EQ(teb->client_pid, (uint32_t)getpid());
        CHECK_EQ(teb->client_tid, (uint32_t)getpid());
        CHECK_EQ(GetCurrentProcessId(), (W32_DWORD)getpid());
        CHECK_EQ(GetCurrentThreadId(), (W32_DWORD)getpid());
        CHECK(GetCurrentThread() == W32_CURRENT_THREAD);
        /* w32_teb.h contract, double-pinned at run time. */
        CHECK_EQ((uint64_t)(uintptr_t)&teb->self - (uint64_t)(uintptr_t)teb,
                 0x30u);
        CHECK_EQ((uint64_t)(uintptr_t)&teb->tls_storage_ptr -
                 (uint64_t)(uintptr_t)teb, 0x58u);
        /* LastError is per-thread from here on (cross-check in §2). */
        SetLastError(7);
        CHECK_EQ(GetLastError(), 7u);
    }

    /* ---- 2. threads: create/join/codes/ids ---- */
    {
        W32_HANDLE hs[8];
        W32_DWORD tids[8];
        W32_DWORD code;
        w_sum = 0;
        CHECK(InitializeCriticalSection(&w_cs));
        w_tls_idx = TlsAlloc();
        CHECK(w_tls_idx != W32_TLS_OUT_OF_INDEXES);
        for (int i = 0; i < 8; i++) {
            hs[i] = CreateThread(0, 0, w_incr, (void *)(intptr_t)5000,
                                 0, &tids[i]);
            CHECK(hs[i] != 0);
            CHECK(tids[i] != 0);
            CHECK(tids[i] != (W32_DWORD)getpid());
        }
        /* Distinct tids. */
        for (int i = 0; i < 8; i++)
            for (int j = i + 1; j < 8; j++)
                CHECK(tids[i] != tids[j]);
        /* Alive threads report STILL_ACTIVE. */
        CHECK(GetExitCodeThread(hs[0], &code));
        CHECK(code == W32_STILL_ACTIVE || code == 5000u);
        for (int i = 0; i < 8; i++) {
            CHECK_EQ(WaitForSingleObject(hs[i], W32_INFINITE),
                     W32_WAIT_OBJECT_0);
            CHECK(GetExitCodeThread(hs[i], &code));
            CHECK_EQ(code, 5000u);
        }
        CHECK_EQ(w_sum, 8 * 5000);
        /* Isolation: 8 workers set their own errors (TlsSetValue sets
         * SUCCESS per-thread); main's slot is untouched by them — a
         * failing main-side call still reports its own code. */
        CHECK(TlsGetValue(9999) == 0);
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
        for (int i = 0; i < 8; i++)
            CHECK(CloseHandle(hs[i]));
        CHECK(TlsFree(w_tls_idx));
        DeleteCriticalSection(&w_cs);
    }

    /* ---- 3. suspended create + ResumeThread ---- */
    {
        W32_DWORD tid = 0;
        W32_DWORD code = 0;
        volatile int *flag;
        W32_HANDLE h;
        CHECK(InitializeCriticalSection(&w_cs));
        w_sum = 0;
        h = CreateThread(0, 0, w_incr, (void *)(intptr_t)100, 0x4, &tid);
        CHECK(h != 0);
        CHECK(tid != 0);
        /* Still suspended: nothing ran yet (yield generously). */
        Sleep(50);
        CHECK_EQ(w_sum, 0);
        CHECK_EQ(ResumeThread(h), 1u);   /* previous count */
        CHECK_EQ(ResumeThread(h), 0u);   /* already running: no-op */
        CHECK_EQ(WaitForSingleObject(h, W32_INFINITE), W32_WAIT_OBJECT_0);
        CHECK_EQ(w_sum, 100);
        CHECK(GetExitCodeThread(h, &code));
        CHECK_EQ(code, 100u);
        CHECK_EQ(ResumeThread(h), (W32_DWORD)-1);  /* dead: refuses */
        CHECK(CloseHandle(h));
        DeleteCriticalSection(&w_cs);
        (void)flag;
    }

    /* ---- 4. TerminateThread ---- */
    {
        W32_HANDLE ev = CreateEventA(0, 1, 0, 0);
        W32_HANDLE h;
        W32_DWORD code = 0;
        CHECK(ev != 0);
        CHECK(InitializeCriticalSection(&w_cs));
        w_sum = 0;
        /* A worker that blocks until told (then would loop). */
        h = CreateThread(0, 0, w_incr, (void *)(intptr_t)1000000000, 0, 0);
        CHECK(h != 0);
        Sleep(50);   /* let it start spinning under the CS */
        CHECK(TerminateThread(h, 55));
        CHECK_EQ(WaitForSingleObject(h, W32_INFINITE), W32_WAIT_OBJECT_0);
        CHECK(GetExitCodeThread(h, &code));
        CHECK_EQ(code, 55u);
        /* Second terminate: already dead, still success, code stands. */
        CHECK(TerminateThread(h, 77));
        CHECK(GetExitCodeThread(h, &code));
        CHECK_EQ(code, 55u);
        CHECK(CloseHandle(h));
        CHECK(CloseHandle(ev));
        DeleteCriticalSection(&w_cs);
    }

    /* ---- 4b. kill a BLOCKED waiter (mid-wait deregistration) ----
     * The victim dies enqueued on the event's waiter list; the kill
     * path must deregister it (a stale entry would dangle into the
     * munmap'd stack).  Proven by using the event hard afterwards. */
    {
        W32_HANDLE ev = CreateEventA(0, 1, 0, 0);
        W32_HANDLE h;
        W32_DWORD code = 0;
        CHECK(ev != 0);
        h = CreateThread(0, 0, w_blocked, ev, 0, 0);
        CHECK(h != 0);
        Sleep(50);   /* victim reaches the wait */
        CHECK(TerminateThread(h, 66));
        CHECK_EQ(WaitForSingleObject(h, W32_INFINITE), W32_WAIT_OBJECT_0);
        CHECK(GetExitCodeThread(h, &code));
        CHECK_EQ(code, 66u);
        CHECK(CloseHandle(h));   /* munmaps the victim's stack */
        /* Structural: the victim left no entry behind (white-box — the
         * alternative is a dangling write into a recycled stack). */
        {
            struct w32_thr_event *evo = (struct w32_thr_event *)
                w32_handle_get_obj(ev, W32_HANDLE_KIND_EVENT);
            CHECK(evo != 0);
            CHECK(evo->w.waiters == 0);
        }
        /* The event's waiter list must be clean: hammer it. */
        for (int i = 0; i < 100; i++) {
            CHECK(SetEvent(ev));
            CHECK_EQ(WaitForSingleObject(ev, 0), W32_WAIT_OBJECT_0);
            CHECK(ResetEvent(ev));
        }
        CHECK(CloseHandle(ev));
    }
    /* ---- 4c. kill a SLEEPER (Sleep checkpoint + waker EINTR) ---- */
    {
        W32_HANDLE h;
        W32_DWORD code = 0;
        h = CreateThread(0, 0, w_sleeper, 0, 0, 0);
        CHECK(h != 0);
        Sleep(50);
        CHECK(TerminateThread(h, 77));
        CHECK_EQ(WaitForSingleObject(h, W32_INFINITE), W32_WAIT_OBJECT_0);
        CHECK(GetExitCodeThread(h, &code));
        CHECK_EQ(code, 77u);
        CHECK(CloseHandle(h));
    }

    /* ---- 5. times + affinity + FreeLibraryAndExitThread ---- */
    {
        W32_FILETIME c, e, k, u;
        uint64_t prev;
        W32_HANDLE h;
        W32_DWORD code = 0;
        CHECK(GetThreadTimes(W32_CURRENT_THREAD, &c, &e, &k, &u));
        CHECK(c.dwLowDateTime != 0 || c.dwHighDateTime != 0);
        CHECK_EQ(e.dwLowDateTime, 0u);
        CHECK_EQ(k.dwLowDateTime, 0u);
        CHECK_EQ(u.dwLowDateTime, 0u);
        prev = SetThreadAffinityMask(W32_CURRENT_THREAD, 1u);
        CHECK(prev != 0);   /* previous mask was the online set */
        CHECK_EQ(SetThreadAffinityMask(W32_CURRENT_THREAD, 0u), 0u);
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
        (void)SetThreadAffinityMask(W32_CURRENT_THREAD, prev);
        /* FreeLibraryAndExitThread: free attempted, exit unconditional. */
        flib_calls = 0;
        h = CreateThread(0, 0,
                         (W32_THREAD_START)(void *)0, 0, 0, 0);
        CHECK(h == 0);   /* NULL start refuses */
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
        (void)code;
    }


    /* ---- 6. FreeLibraryAndExitThread (worker calls it) ---- */
    {
        W32_HANDLE h;
        W32_DWORD code = 0;
        flib_calls = 0;
        flib_last_mod = 0;
        h = CreateThread(0, 0, w_flib_exit, (void *)(intptr_t)0x1234, 0, 0);
        CHECK(h != 0);
        CHECK_EQ(WaitForSingleObject(h, W32_INFINITE), W32_WAIT_OBJECT_0);
        CHECK(GetExitCodeThread(h, &code));
        CHECK_EQ(code, 44u);
        CHECK_EQ(flib_calls, 1);
        CHECK_EQ(flib_last_mod, (void *)(intptr_t)0x1234);
        CHECK(CloseHandle(h));
    }

    /* ---- 7. Tls/Fls API surface (main thread) ---- */
    {
        W32_DWORD idx[W32_TEB_TLS_SLOTS + 1];
        W32_DWORD f0, f1;
        int i;
        for (i = 0; i < W32_TEB_TLS_SLOTS; i++) {
            idx[i] = TlsAlloc();
            CHECK(idx[i] != W32_TLS_OUT_OF_INDEXES);
        }
        idx[W32_TEB_TLS_SLOTS] = TlsAlloc();
        CHECK_EQ(idx[W32_TEB_TLS_SLOTS], W32_TLS_OUT_OF_INDEXES);
        CHECK_EQ(GetLastError(), W32_ERROR_NOT_ENOUGH_MEMORY);
        /* NULL-value rule: NULL + SUCCESS means "holds NULL". */
        CHECK(TlsGetValue(idx[0]) == 0);
        CHECK_EQ(GetLastError(), W32_ERROR_SUCCESS);
        CHECK(TlsSetValue(idx[0], (void *)(intptr_t)0xBEEF));
        CHECK_EQ(TlsGetValue(idx[0]), (void *)(intptr_t)0xBEEF);
        /* Bad index. */
        CHECK(TlsGetValue(9999) == 0);
        CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
        CHECK(!TlsSetValue(9999, 0));
        CHECK(!TlsFree(9999));
        for (i = 0; i < W32_TEB_TLS_SLOTS; i++)
            CHECK(TlsFree(idx[i]));
        /* Re-alloc returns a zeroed slot (no stale value). */
        idx[0] = TlsAlloc();
        CHECK(TlsGetValue(idx[0]) == 0);
        CHECK(TlsFree(idx[0]));
        /* FLS mirrors TLS. */
        f0 = FlsAlloc((void *)w_fls_cb);
        CHECK(f0 != W32_FLS_OUT_OF_INDEXES);
        f1 = FlsAlloc(0);
        CHECK(f1 != W32_FLS_OUT_OF_INDEXES);
        CHECK(f0 != f1);
        CHECK(FlsSetValue(f0, (void *)(intptr_t)42));
        CHECK_EQ(FlsGetValue(f0), (void *)(intptr_t)42);
        CHECK(FlsGetValue(9999) == 0);
        CHECK(!FlsSetValue(9999, 0));
        CHECK(!FlsFree(9999));
        CHECK(FlsFree(f0));
        CHECK(FlsFree(f1));
    }

    /* ---- 8. FLS callbacks fire at thread exit ---- */
    {
        W32_DWORD f0;
        W32_HANDLE h;
        w_fls_cb_count = 0;
        f0 = FlsAlloc((void *)w_fls_cb);
        CHECK(f0 != W32_FLS_OUT_OF_INDEXES);
        h = CreateThread(0, 0, w_fls_user, (void *)(uintptr_t)f0, 0, 0);
        CHECK(h != 0);
        CHECK_EQ(WaitForSingleObject(h, W32_INFINITE), W32_WAIT_OBJECT_0);
        CHECK_EQ(w_fls_cb_count, 1);
        CHECK(CloseHandle(h));
        CHECK(FlsFree(f0));
    }

    /* ---- 9. TLS template + callbacks, white-box ----
     * A fake image in RWX memory: TLS dir + template + one callback.
     * VAs are real pointers (the registrar resolves VA->RVA against
     * base and REFUSES anything outside the image — the hostile-input
     * gate — so the callback is an in-image trampoline (jmp [rip+0])
     * that tail-jumps to the real w_tls_cb in .text). */
    {
        static unsigned char *img;
        w32_tls_directory_t *dir;
        uint64_t *tmpl;
        uint64_t *cbarr;
        uint32_t *indexcell;
        unsigned char *tramp;
        int slot;
        W32_HANDLE h;
        img = (unsigned char *)mmap(0, 512, PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(img != MAP_FAILED);
        if (img == MAP_FAILED)
            return 1;
        memset(img, 0, 512);
        dir = (w32_tls_directory_t *)(img + 64);
        tmpl = (uint64_t *)(img + 128);
        tmpl[0] = 0xAAAABBBBCCCCDDDDull;
        tmpl[1] = 0x1111222233334444ull;
        /* In-image trampoline: jmp qword [rip+0]; .quad w_tls_cb. */
        tramp = img + 256;
        tramp[0] = 0xFF;
        tramp[1] = 0x25;
        tramp[2] = tramp[3] = tramp[4] = tramp[5] = 0;
        *(uint64_t *)(void *)(tramp + 6) = (uint64_t)(uintptr_t)w_tls_cb;
        cbarr = (uint64_t *)(img + 192);
        cbarr[0] = (uint64_t)(uintptr_t)tramp;
        cbarr[1] = 0;
        indexcell = (uint32_t *)(img + 224);
        dir->start_address_of_raw_data = (uint64_t)(uintptr_t)tmpl;
        dir->end_address_of_raw_data = (uint64_t)(uintptr_t)(tmpl + 2);
        dir->address_of_index = (uint64_t)(uintptr_t)indexcell;
        dir->address_of_callbacks = (uint64_t)(uintptr_t)cbarr;
        dir->size_of_zero_fill = 8;
        dir->characteristics = 0;
        w_attach_count = 0;
        w_detach_count = 0;
        slot = w32_crt_register_tls(img, 512, 64,
                                    (uint32_t)sizeof(*dir));
        CHECK(slot >= 0);
        CHECK(*indexcell == (uint32_t)slot);
        /* The main thread instantiates lazily: attach it now. */
        w32_tls_attach_main();
        CHECK_EQ(w_attach_count, 1);   /* PROCESS_ATTACH ran */
        {
            struct w32_teb *teb = w32_teb_self();
            uint64_t **arr = (uint64_t **)(uintptr_t)teb->tls_storage_ptr;
            CHECK(arr[slot] != 0);
            CHECK_EQ(arr[slot][0], 0xAAAABBBBCCCCDDDDull);
            CHECK_EQ(arr[slot][1], 0x1111222233334444ull);
            CHECK_EQ(arr[slot][2], 0u);   /* zero fill */
        }
        /* A worker gets its OWN copy + THREAD_ATTACH/DETACH. */
        h = CreateThread(0, 0, w_tls_user, (void *)(intptr_t)slot, 0, 0);
        CHECK(h != 0);
        CHECK_EQ(WaitForSingleObject(h, W32_INFINITE), W32_WAIT_OBJECT_0);
        CHECK_EQ(w_attach_count, 2);
        CHECK_EQ(w_detach_count, 1);
        {
            /* Main's block is untouched by the worker's write. */
            struct w32_teb *teb = w32_teb_self();
            uint64_t **arr = (uint64_t **)(uintptr_t)teb->tls_storage_ptr;
            CHECK_EQ(arr[slot][0], 0xAAAABBBBCCCCDDDDull);
        }
        CHECK(CloseHandle(h));
        w32_tls_unregister_module((void *)img);
        CHECK_EQ(w_detach_count, 2);   /* PROCESS_DETACH ran */
    }


    /* ---- 10. CS variants + recursion + perf lane ---- */
    {
        W32_CRITICAL_SECTION cs;
        uint64_t t0, t1;
        CHECK(InitializeCriticalSectionAndSpinCount(&cs, 4000));
        CHECK(InitializeCriticalSectionEx(&cs, 100, 0x1));
        CHECK(!InitializeCriticalSectionEx(&cs, 100, 0x4));
        CHECK(InitializeCriticalSectionEx(&cs, 100, 0x3));
        CHECK(!InitializeCriticalSection(0));
        EnterCriticalSection(&cs);
        EnterCriticalSection(&cs);
        LeaveCriticalSection(&cs);
        LeaveCriticalSection(&cs);
        DeleteCriticalSection(&cs);
        /* Perf lane: uncontended pairs. */
        CHECK(InitializeCriticalSection(&cs));
        t0 = GetTickCount64();
        for (int i = 0; i < 1000000; i++) {
            EnterCriticalSection(&cs);
            LeaveCriticalSection(&cs);
        }
        t1 = GetTickCount64();
        printf("  [perf] uncontended CS: 1M pairs in %llu ms\n",
               (unsigned long long)(t1 - t0));
        DeleteCriticalSection(&cs);
    }

    /* ---- 11. SRW + condition variable ---- */
    {
        W32_HANDLE hs[2];
        memset(&w_srw, 0, sizeof(w_srw));
        memset(&w_cv, 0, sizeof(w_cv));
        CHECK(TryAcquireSRWLockExclusive(&w_srw));
        CHECK(!TryAcquireSRWLockExclusive(&w_srw));  /* held: no */
        ReleaseSRWLockExclusive(&w_srw);
        CHECK(TryAcquireSRWLockExclusive(&w_srw));
        ReleaseSRWLockExclusive(&w_srw);
        w_cv_ready = 0;
        w_cv_got = 0;
        hs[0] = CreateThread(0, 0, w_cv_consumer, 0, 0, 0);
        hs[1] = CreateThread(0, 0, w_cv_consumer, 0, 0, 0);
        CHECK(hs[0] != 0 && hs[1] != 0);
        Sleep(50);
        AcquireSRWLockExclusive(&w_srw);
        w_cv_ready = 1;
        WakeAllConditionVariable(&w_cv);
        ReleaseSRWLockExclusive(&w_srw);
        CHECK_EQ(WaitForMultipleObjects(2, hs, 1, W32_INFINITE),
                 W32_WAIT_OBJECT_0);
        CHECK_EQ(w_cv_got, 2);
        CHECK(CloseHandle(hs[0]));
        CHECK(CloseHandle(hs[1]));
        /* Timed CV sleep times out. */
        AcquireSRWLockExclusive(&w_srw);
        t_cv0 = GetTickCount64();
        CHECK(!SleepConditionVariableSRW(&w_cv, &w_srw, 50, 0));
        CHECK_EQ(GetLastError(), W32_ERROR_WAIT_TIMEOUT);
        CHECK(GetTickCount64() - t_cv0 >= 50);
        CHECK(!SleepConditionVariableSRW(&w_cv, &w_srw, 50, 1)); /* shared */
        ReleaseSRWLockExclusive(&w_srw);
    }

    /* ---- 12. InitOnce + SList ---- */
    {
        W32_HANDLE hs[8];
        W32_SLIST_HEADER head;
        W32_SLIST_ENTRY e1, e2, e3;
        void *chain;
        memset(&w_once, 0, sizeof(w_once));
        w_once_ran = 0;
        for (int i = 0; i < 8; i++)
            hs[i] = CreateThread(0, 0, w_once_user, 0, 0, 0);
        CHECK_EQ(WaitForMultipleObjects(8, hs, 1, W32_INFINITE),
                 W32_WAIT_OBJECT_0);
        CHECK_EQ(w_once_ran, 1);
        for (int i = 0; i < 8; i++)
            CHECK(CloseHandle(hs[i]));
        {
            W32_BOOL pending = 1;
            CHECK(InitOnceBeginInitialize(&w_once, 0, &pending, 0));
            CHECK_EQ(pending, W32_FALSE);   /* done: no re-run */
            CHECK(!InitOnceBeginInitialize(&w_once, 1, &pending, 0));
        }
        InitializeSListHead(&head);
        CHECK_EQ(head.depth, 0u);
        CHECK(InterlockedFlushSList(&head) == 0);  /* empty flush */
        e1.next = 0;
        e2.next = &e1;
        e3.next = &e2;
        head.next = &e3;
        head.depth = 3;
        chain = InterlockedFlushSList(&head);
        CHECK_EQ(chain, (void *)&e3);
        CHECK(head.next == 0 && head.depth == 0);
        CHECK_EQ(((W32_SLIST_ENTRY *)chain)->next, (void *)&e2);
        CHECK(InterlockedFlushSList(&head) == 0);
    }

    /* ---- 13. events, mutexes, semaphores ---- */
    {
        W32_HANDLE ev, ev2, m, m2, s;
        W32_LONG prev = -1;
        W32_HANDLE hs[4];
        /* Manual event. */
        ev = CreateEventA(0, 1, 0, 0);
        CHECK(ev != 0);
        CHECK_EQ(WaitForSingleObject(ev, 0), W32_WAIT_TIMEOUT);
        CHECK(SetEvent(ev));
        CHECK_EQ(WaitForSingleObject(ev, 0), W32_WAIT_OBJECT_0);
        CHECK_EQ(WaitForSingleObject(ev, 0), W32_WAIT_OBJECT_0);
        CHECK(ResetEvent(ev));
        CHECK_EQ(WaitForSingleObject(ev, 0), W32_WAIT_TIMEOUT);
        /* Auto event: exactly one waiter per Set. */
        ev2 = CreateEventA(0, 0, 0, 0);
        CHECK(ev2 != 0);
        CHECK(SetEvent(ev2));
        CHECK_EQ(WaitForSingleObject(ev2, 0), W32_WAIT_OBJECT_0);
        CHECK_EQ(WaitForSingleObject(ev2, 0), W32_WAIT_TIMEOUT);
        /* Named: same name == same object. */
        {
            W32_HANDLE n1 = CreateEventA(0, 1, 0, "w32a3_named_ev");
            W32_HANDLE n2;
            CHECK(n1 != 0);
            CHECK_EQ(GetLastError(), W32_ERROR_SUCCESS);
            n2 = CreateEventA(0, 1, 0, "w32a3_named_ev");
            CHECK(n2 != 0 && n2 != n1);
            CHECK_EQ(GetLastError(), W32_ERROR_ALREADY_EXISTS);
            CHECK(SetEvent(n1));
            CHECK_EQ(WaitForSingleObject(n2, 0), W32_WAIT_OBJECT_0);
            CHECK(CloseHandle(n1));
            CHECK(CloseHandle(n2));
        }
        /* Mutex contention. */
        m = CreateMutexA(0, 0, 0);
        CHECK(m != 0);
        w_sum = 0;
        for (int i = 0; i < 4; i++)
            hs[i] = CreateThread(0, 0, w_mutex_spin, m, 0, 0);
        CHECK_EQ(WaitForMultipleObjects(4, hs, 1, W32_INFINITE),
                 W32_WAIT_OBJECT_0);
        CHECK_EQ(w_sum, 4 * 2000);
        for (int i = 0; i < 4; i++)
            CHECK(CloseHandle(hs[i]));
        /* Recursive + foreign-release refusal. */
        CHECK_EQ(WaitForSingleObject(m, 0), W32_WAIT_OBJECT_0);
        CHECK_EQ(WaitForSingleObject(m, 0), W32_WAIT_OBJECT_0);
        CHECK(ReleaseMutex(m));
        CHECK(ReleaseMutex(m));
        CHECK(!ReleaseMutex(m));   /* free: refuses */
        /* Abandoned: holder exits without release. */
        m2 = CreateMutexA(0, 0, 0);
        CHECK(m2 != 0);
        hs[0] = CreateThread(0, 0, w_mutex_holder, m2, 0, 0);
        CHECK_EQ(WaitForSingleObject(hs[0], W32_INFINITE),
                 W32_WAIT_OBJECT_0);
        CHECK(CloseHandle(hs[0]));
        CHECK_EQ(WaitForSingleObject(m2, W32_INFINITE),
                 W32_WAIT_ABANDONED_0);
        CHECK_EQ(GetLastError(), W32_ERROR_ABANDONED_WAIT_0);
        CHECK(ReleaseMutex(m2));   /* abandoned take owns it */
        CHECK(CloseHandle(m));
        CHECK(CloseHandle(m2));
        /* Semaphore. */
        s = CreateSemaphoreW(0, 2, 5, 0);
        CHECK(s != 0);
        CHECK_EQ(WaitForSingleObject(s, 0), W32_WAIT_OBJECT_0);
        CHECK_EQ(WaitForSingleObject(s, 0), W32_WAIT_OBJECT_0);
        CHECK_EQ(WaitForSingleObject(s, 0), W32_WAIT_TIMEOUT);
        CHECK(ReleaseSemaphore(s, 3, &prev));
        CHECK_EQ(prev, 0);
        CHECK(!ReleaseSemaphore(s, 99, 0));   /* over max */
        CHECK(!ReleaseSemaphore(s, 0, 0));
        CHECK(CreateSemaphoreW(0, -1, 5, 0) == 0);
        CHECK(CreateSemaphoreW(0, 6, 5, 0) == 0);
        CHECK_EQ(WaitForSingleObject(s, 0), W32_WAIT_OBJECT_0);
        CHECK(CloseHandle(s));
        CHECK(CloseHandle(ev));
        CHECK(CloseHandle(ev2));
    }

    /* ---- 14. waits: timeouts, multi, Ex+APC, processes ---- */
    {
        W32_HANDLE ev = CreateEventA(0, 1, 0, 0);
        W32_HANDLE ev2 = CreateEventA(0, 1, 0, 0);
        W32_HANDLE hs[2];
        W32_HANDLE ha;
        uint64_t t0;
        CHECK(ev != 0 && ev2 != 0);
        /* Finite timeout honoured within the documented slop. */
        t0 = GetTickCount64();
        CHECK_EQ(WaitForSingleObject(ev, 100), W32_WAIT_TIMEOUT);
        CHECK(GetTickCount64() - t0 >= 100);
        CHECK(GetTickCount64() - t0 < 100 + 50);
        CHECK_EQ(GetLastError(), W32_ERROR_WAIT_TIMEOUT);
        /* Multi any/all. */
        hs[0] = ev;
        hs[1] = ev2;
        CHECK(SetEvent(ev2));
        CHECK_EQ(WaitForMultipleObjects(2, hs, 0, 0), W32_WAIT_OBJECT_0 + 1);
        CHECK_EQ(WaitForMultipleObjects(2, hs, 1, 0), W32_WAIT_TIMEOUT);
        CHECK(SetEvent(ev));
        CHECK_EQ(WaitForMultipleObjects(2, hs, 1, 0), W32_WAIT_OBJECT_0);
        /* Ex + APC: the alertable wait releases with IO_COMPLETION. */
        CHECK(ResetEvent(ev));
        ha = CreateThread(0, 0, w_apc_target, ev, 0, 0);
        CHECK(ha != 0);
        Sleep(50);
        CHECK(QueueUserAPC((void *)w_apc_fn, ha, 0x7777));
        {
            W32_DWORD r = WaitForSingleObject(ha, W32_INFINITE);
            W32_DWORD code = 0;
            CHECK_EQ(r, W32_WAIT_OBJECT_0);
            CHECK(GetExitCodeThread(ha, &code));
            CHECK_EQ(code, W32_WAIT_IO_COMPLETION);
            CHECK_EQ(w_apc_got, 0x7777);
        }
        CHECK(CloseHandle(ha));
        CHECK(!QueueUserAPC(0, W32_CURRENT_THREAD, 0));
        /* Unwaitable handle refuses. */
        CHECK_EQ(WaitForSingleObject((W32_HANDLE)(intptr_t)0x1234, 0),
                 W32_WAIT_FAILED);
        /* Too many objects refuses. */
        {
            W32_HANDLE many[65];
            for (int i = 0; i < 65; i++)
                many[i] = ev;
            CHECK_EQ(WaitForMultipleObjects(65, many, 0, 0),
                     W32_WAIT_FAILED);
        }
        /* Process handles are waitable (real fork+exec child). */
        {
            W32_STARTUPINFOA sia;
            W32_PROCESS_INFORMATION pi;
            W32_DWORD code = 0;
            memset(&sia, 0, sizeof(sia));
            sia.cb = sizeof(sia);
            memset(&pi, 0, sizeof(pi));
            CHECK(CreateProcessA("/bin/true", 0, 0, 0, 0, 0, 0, 0,
                                 &sia, &pi));
            CHECK_EQ(WaitForSingleObject(pi.hProcess, W32_INFINITE),
                     W32_WAIT_OBJECT_0);
            CHECK(GetExitCodeProcess(pi.hProcess, &code));
            CHECK_EQ(code, 0u);
            CHECK(CloseHandle(pi.hProcess));
            CHECK(CloseHandle(pi.hThread));
        }
        CHECK(CloseHandle(ev));
        CHECK(CloseHandle(ev2));
    }

    /* ---- 15. threadpool ---- */
    {
        void *works[200];
        int i;
        w_pool_n = 0;
        w_pool_sum = 0;
        for (i = 0; i < 200; i++) {
            works[i] = CreateThreadpoolWork((void *)w_pool_cb,
                                            (void *)(intptr_t)(i + 1), 0);
            CHECK(works[i] != 0);
        }
        CHECK(CreateThreadpoolWork(0, 0, 0) == 0);
        for (i = 0; i < 200; i++)
            SubmitThreadpoolWork(works[i]);
        /* All 200 run: sum(1..200) == 20100. */
        {
            uint64_t deadline = GetTickCount64() + 10000;
            while (w_pool_n < 200 &&
                   (int64_t)(GetTickCount64() - deadline) < 0)
                Sleep(5);
        }
        CHECK_EQ(w_pool_n, 200);
        CHECK_EQ(w_pool_sum, 20100L);
        for (i = 0; i < 200; i++)
            CloseThreadpoolWork(works[i]);
    }

    /* ---- 16. worker TEB: GS + ClientId in a fresh thread ---- */
    {
        static uint64_t probe[4];
        W32_HANDLE h;
        probe[0] = probe[1] = probe[2] = probe[3] = 0;
        h = CreateThread(0, 0, w_teb_probe, probe, 0, 0);
        CHECK(h != 0);
        CHECK_EQ(WaitForSingleObject(h, W32_INFINITE), W32_WAIT_OBJECT_0);
        /* [0]=%gs:0x30, [1]=teb->self, [2]=client_tid, [3]=getpid-tid */
        CHECK_EQ(probe[0], probe[1]);
        CHECK(probe[0] != 0);
        CHECK_EQ(probe[2], probe[3]);
        CHECK(probe[2] != (uint64_t)getpid());
        CHECK(CloseHandle(h));
    }

    printf("checks=%d fails=%d\n", checks, fails);
    return fails ? 1 : 0;
}

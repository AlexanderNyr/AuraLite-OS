/*
 * test_w32_kernel32.c — host unit test for WIN32_PLAN.md phase W32-4.
 *
 * The plan's gate: "Every implemented function has a failure-path test: bad
 * handle -> FALSE + GetLastError() == ERROR_INVALID_HANDLE, not a crash."
 *
 * Failure paths come first here, as they did in the UTF and PE tests, because
 * the happy path is what a smoke test already covers when the OS boots.  What
 * a Win32 program actually depends on -- and what silently breaks an ABI
 * translation layer -- is the polarity of the return value and the error code
 * behind it.
 *
 * The libc side is stubbed rather than linked: this test is about the
 * translation, so read/write/open/close are replaced with instrumented
 * versions.  That makes error injection exact (return -EBADF and check the
 * mapping) instead of depending on the host filesystem.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "w32/w32_abi.h"
#include "w32/w32_handle.h"
#include "w32/w32_errno.h"

static int passed = 0, failed = 0, tn = 0;
#define RUN(f) do { int b = failed; f(); tn++; \
                    if (failed == b) passed++; \
                    else printf("  [%s] FAILED\n", #f); } while (0)
#define CHECK(c) do { if (!(c)) { \
    printf("  FAIL L%d: %s\n", __LINE__, #c); failed++; } } while (0)
#define CHECK_EQ(a, e) do { long long _a = (long long)(a), _e = (long long)(e); \
    if (_a != _e) { printf("  FAIL L%d: %s=%lld want %lld\n", \
                    __LINE__, #a, _a, _e); failed++; } } while (0)

/* ---- instrumented libc ---------------------------------------------------
 * Declared before including kernel32.c so its calls bind to these. */

static long   stub_write_ret = 0;
static long   stub_read_ret  = 0;
static long   stub_open_ret  = 3;
static int    stub_close_ret = 0;
static int    last_write_fd  = -1;
static int    last_open_flags = -1;
static int    closed_fds[16];
static int    closed_n = 0;

long   write(int fd, const void *b, unsigned long n);
long   read(int fd, void *b, unsigned long n);
int    open(const char *p, int fl, ...);
int    close(int fd);
void   _exit(int c) __attribute__((noreturn));
int    nanosleep(const void *req, void *rem);
int    clock_gettime(int id, void *tp);

long write(int fd, const void *b, unsigned long n) {
    (void)b; last_write_fd = fd;
    return stub_write_ret ? stub_write_ret : (long)n;
}
long read(int fd, void *b, unsigned long n) {
    (void)fd; (void)b; (void)n;
    return stub_read_ret;
}
int open(const char *p, int fl, ...) {
    (void)p; last_open_flags = fl;
    return (int)stub_open_ret;
}
int close(int fd) {
    if (closed_n < 16) closed_fds[closed_n++] = fd;
    return stub_close_ret;
}
static int exited = 0;
static int exit_code = -1;
void _exit(int c) { exited = 1; exit_code = c; /* unwind via longjmp is
    overkill here: no test calls ExitProcess, it is covered in the guest. */
    for (;;) { if (exited) break; } abort(); }
int nanosleep(const void *req, void *rem) { (void)req; (void)rem; return 0; }
struct fake_ts { long tv_sec; long tv_nsec; };
int clock_gettime(int id, void *tp) {
    (void)id;
    struct fake_ts *t = tp;
    t->tv_sec = 5; t->tv_nsec = 500000000L;   /* 5.5 s -> 5500 ms */
    return 0;
}

/* malloc/free/memset/memcpy/strlen/strchr come from the host libc. */
#include <stdlib.h>
#include <string.h>

/* Constants kernel32.c takes from AuraLite's headers.  Defined here because
 * this test deliberately does not include the guest libc: it stubs it. */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_TRUNC  0x0200
#define O_APPEND 0x0400
struct timespec { long tv_sec; long tv_nsec; };
typedef long ssize_t;

/* Pull in the implementation under test. */
#define AURALITE_W32_HOST_TEST 1
#define main w32_unused_main
#include "../../w32/src/w32_errno.c"
#include "../../w32/src/w32_handle.c"
#include "../../w32/src/kernel32.c"
#undef main

/* ---- handle table --------------------------------------------------------- */

static void test_std_handles_resolve(void) {
    w32_handle_init();
    CHECK_EQ(w32_handle_to_fd(GetStdHandle(W32_STD_INPUT_HANDLE)),  0);
    CHECK_EQ(w32_handle_to_fd(GetStdHandle(W32_STD_OUTPUT_HANDLE)), 1);
    CHECK_EQ(w32_handle_to_fd(GetStdHandle(W32_STD_ERROR_HANDLE)),  2);
}

static void test_bad_std_handle_selector(void) {
    w32_handle_init();
    W32_HANDLE h = GetStdHandle(42);
    CHECK(h == W32_INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
}

static void test_forged_handles_are_rejected(void) {
    w32_handle_init();
    /* The values a program is most likely to invent, and the ones a cast-based
     * implementation would happily accept as fds 0..3. */
    CHECK_EQ(w32_handle_to_fd((W32_HANDLE)(intptr_t)0), -1);
    CHECK_EQ(w32_handle_to_fd((W32_HANDLE)(intptr_t)1), -1);
    CHECK_EQ(w32_handle_to_fd((W32_HANDLE)(intptr_t)3), -1);
    CHECK_EQ(w32_handle_to_fd((W32_HANDLE)(intptr_t)0x99999), -1);
    CHECK_EQ(w32_handle_to_fd(W32_INVALID_HANDLE_VALUE), -1);
}

static void test_handle_alloc_and_release(void) {
    w32_handle_init();
    CHECK_EQ(w32_handle_live_count(), 0);

    W32_HANDLE a = w32_handle_alloc(7, 1);
    W32_HANDLE b = w32_handle_alloc(8, 1);
    CHECK(a != 0 && b != 0 && a != b);
    CHECK_EQ(w32_handle_to_fd(a), 7);
    CHECK_EQ(w32_handle_to_fd(b), 8);
    CHECK_EQ(w32_handle_live_count(), 2);

    CHECK_EQ(w32_handle_release(a), 7);
    CHECK_EQ(w32_handle_to_fd(a), -1);      /* released handles stop resolving */
    CHECK_EQ(w32_handle_live_count(), 1);
    CHECK_EQ(w32_handle_release(a), -1);    /* double release is refused */
}

static void test_handle_table_exhaustion(void) {
    w32_handle_init();
    int n = 0;
    while (w32_handle_alloc(20 + n, 1) != 0) {
        n++;
        if (n > W32_HANDLE_MAX + 8) break;   /* independent runaway guard */
    }
    CHECK_EQ(n, W32_HANDLE_MAX - 3);         /* three std slots are reserved */
    CHECK_EQ(w32_handle_alloc(99, 1), (W32_HANDLE)0);
}

/* ---- WriteFile ------------------------------------------------------------ */

static void test_writefile_bad_handle(void) {
    w32_handle_init();
    SetLastError(W32_ERROR_SUCCESS);
    W32_DWORD written = 12345;
    W32_BOOL r = WriteFile((W32_HANDLE)(intptr_t)0x4242, "x", 1, &written, 0);
    CHECK_EQ(r, W32_FALSE);
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_HANDLE);
    CHECK_EQ(written, 0);                    /* must be cleared, not left stale */
}

static void test_writefile_success(void) {
    w32_handle_init();
    stub_write_ret = 0;
    W32_DWORD written = 0;
    W32_BOOL r = WriteFile(GetStdHandle(W32_STD_OUTPUT_HANDLE), "hello", 5,
                           &written, 0);
    CHECK_EQ(r, W32_TRUE);
    CHECK_EQ(written, 5);
    CHECK_EQ(last_write_fd, 1);
}

static void test_writefile_null_buffer(void) {
    w32_handle_init();
    W32_DWORD written = 7;
    W32_BOOL r = WriteFile(GetStdHandle(W32_STD_OUTPUT_HANDLE), 0, 4, &written, 0);
    CHECK_EQ(r, W32_FALSE);
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);
    CHECK_EQ(written, 0);
}

static void test_writefile_zero_length_is_success(void) {
    w32_handle_init();
    W32_DWORD written = 9;
    /* Zero-length write is a legal no-op; returning FALSE here would make
     * printf("") look like a failure. */
    W32_BOOL r = WriteFile(GetStdHandle(W32_STD_OUTPUT_HANDLE), 0, 0, &written, 0);
    CHECK_EQ(r, W32_TRUE);
}

static void test_writefile_errno_mapping(void) {
    w32_handle_init();
    stub_write_ret = -9;                     /* -EBADF */
    W32_DWORD written = 0;
    W32_BOOL r = WriteFile(GetStdHandle(W32_STD_OUTPUT_HANDLE), "x", 1, &written, 0);
    CHECK_EQ(r, W32_FALSE);
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_HANDLE);

    stub_write_ret = -28;                    /* -ENOSPC */
    r = WriteFile(GetStdHandle(W32_STD_OUTPUT_HANDLE), "x", 1, &written, 0);
    CHECK_EQ(r, W32_FALSE);
    CHECK_EQ(GetLastError(), W32_ERROR_NOT_ENOUGH_MEMORY);
    stub_write_ret = 0;
}

static void test_writefile_null_written_pointer(void) {
    w32_handle_init();
    /* lpNumberOfBytesWritten may legally be NULL. */
    W32_BOOL r = WriteFile(GetStdHandle(W32_STD_OUTPUT_HANDLE), "abc", 3, 0, 0);
    CHECK_EQ(r, W32_TRUE);
}

/* ---- ReadFile ------------------------------------------------------------- */

static void test_readfile_bad_handle_and_eof(void) {
    w32_handle_init();
    char buf[8];
    W32_DWORD got = 999;

    W32_BOOL r = ReadFile((W32_HANDLE)(intptr_t)0x1234, buf, sizeof buf, &got, 0);
    CHECK_EQ(r, W32_FALSE);
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_HANDLE);
    CHECK_EQ(got, 0);

    /* EOF is success with zero bytes, not an error -- a CRT loop depends on
     * this to terminate rather than to report a failure. */
    stub_read_ret = 0;
    r = ReadFile(GetStdHandle(W32_STD_INPUT_HANDLE), buf, sizeof buf, &got, 0);
    CHECK_EQ(r, W32_TRUE);
    CHECK_EQ(got, 0);
}

/* ---- CreateFileA ---------------------------------------------------------- */

static void test_createfile_flag_translation(void) {
    w32_handle_init();
    stub_open_ret = 5;

    W32_HANDLE h = CreateFileA("/tmp/x", W32_GENERIC_WRITE, 0, 0,
                               W32_CREATE_ALWAYS, 0, 0);
    CHECK(h != W32_INVALID_HANDLE_VALUE);
    CHECK_EQ(w32_handle_to_fd(h), 5);
    /* O_WRONLY|O_CREAT|O_TRUNC */
    CHECK((last_open_flags & 0x0001) != 0);
    CHECK((last_open_flags & 0x0040) != 0);
    CHECK((last_open_flags & 0x0200) != 0);

    h = CreateFileA("/tmp/y", W32_GENERIC_READ, 0, 0, W32_OPEN_EXISTING, 0, 0);
    CHECK(h != W32_INVALID_HANDLE_VALUE);
    CHECK_EQ(last_open_flags & 0x0003, 0);   /* O_RDONLY, no create/trunc */
}

static void test_createfile_failures(void) {
    w32_handle_init();

    W32_HANDLE h = CreateFileA(0, W32_GENERIC_READ, 0, 0, W32_OPEN_EXISTING, 0, 0);
    CHECK(h == W32_INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);

    h = CreateFileA("/tmp/z", W32_GENERIC_READ, 0, 0, 999, 0, 0);
    CHECK(h == W32_INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);

    stub_open_ret = -2;                      /* -ENOENT */
    h = CreateFileA("/nope", W32_GENERIC_READ, 0, 0, W32_OPEN_EXISTING, 0, 0);
    CHECK(h == W32_INVALID_HANDLE_VALUE);
    CHECK_EQ(GetLastError(), W32_ERROR_FILE_NOT_FOUND);
    stub_open_ret = 3;
}

/* ---- CloseHandle ---------------------------------------------------------- */

static void test_closehandle_paths(void) {
    w32_handle_init();
    closed_n = 0;

    /* A real handle closes its fd. */
    stub_open_ret = 11;
    W32_HANDLE h = CreateFileA("/tmp/a", W32_GENERIC_READ, 0, 0,
                               W32_OPEN_EXISTING, 0, 0);
    CHECK_EQ(CloseHandle(h), W32_TRUE);
    CHECK_EQ(closed_n, 1);
    CHECK_EQ(closed_fds[0], 11);

    /* A bad handle is refused. */
    CHECK_EQ(CloseHandle((W32_HANDLE)(intptr_t)0xABCD), W32_FALSE);
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_HANDLE);

    /* A standard handle reports success but must NOT close the fd: taking
     * stdout away from the process is worse than ignoring the request. */
    closed_n = 0;
    CHECK_EQ(CloseHandle(GetStdHandle(W32_STD_OUTPUT_HANDLE)), W32_TRUE);
    CHECK_EQ(closed_n, 0);
}

/* ---- memory --------------------------------------------------------------- */

static void test_virtualalloc(void) {
    void *p = VirtualAlloc(0, 128, W32_MEM_COMMIT, W32_PAGE_READWRITE);
    CHECK(p != 0);
    /* Win32 commits zero-filled pages; a CRT relies on it. */
    unsigned char *b = p;
    int nonzero = 0;
    for (int i = 0; i < 128; i++) if (b[i]) nonzero = 1;
    CHECK_EQ(nonzero, 0);
    CHECK_EQ(VirtualFree(p, 0, W32_MEM_RELEASE), W32_TRUE);

    CHECK_EQ(VirtualAlloc(0, 0, W32_MEM_COMMIT, 0), (void *)0);
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);

    /* Placement is refused loudly rather than silently ignored. */
    CHECK_EQ(VirtualAlloc((void *)0x140000000ull, 64, W32_MEM_COMMIT, 0), (void *)0);
    CHECK_EQ(GetLastError(), W32_ERROR_NOT_SUPPORTED);

    CHECK_EQ(VirtualFree(0, 0, W32_MEM_RELEASE), W32_FALSE);
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_PARAMETER);

    /* MEM_RELEASE with a non-zero size is a caller bug Win32 rejects. */
    void *q = VirtualAlloc(0, 32, W32_MEM_COMMIT, 0);
    CHECK_EQ(VirtualFree(q, 32, W32_MEM_RELEASE), W32_FALSE);
    CHECK_EQ(VirtualFree(q, 0, W32_MEM_RELEASE), W32_TRUE);
}

static void test_heap(void) {
    W32_HANDLE heap = GetProcessHeap();
    CHECK(heap != 0);

    void *p = HeapAlloc(heap, 0, 64);
    CHECK(p != 0);
    CHECK_EQ(HeapFree(heap, 0, p), W32_TRUE);

    /* HEAP_ZERO_MEMORY */
    unsigned char *z = HeapAlloc(heap, 0x8, 32);
    CHECK(z != 0);
    int nonzero = 0;
    for (int i = 0; i < 32; i++) if (z[i]) nonzero = 1;
    CHECK_EQ(nonzero, 0);
    HeapFree(heap, 0, z);

    /* A zero-size request returns a unique block, never NULL. */
    void *tiny = HeapAlloc(heap, 0, 0);
    CHECK(tiny != 0);
    HeapFree(heap, 0, tiny);

    /* A wrong heap handle is refused. */
    CHECK_EQ(HeapAlloc((W32_HANDLE)(intptr_t)1, 0, 16), (void *)0);
    CHECK_EQ(GetLastError(), W32_ERROR_INVALID_HANDLE);
    CHECK_EQ(HeapFree((W32_HANDLE)(intptr_t)1, 0, 0), W32_FALSE);

    /* HeapFree(NULL) is a legal no-op. */
    CHECK_EQ(HeapFree(heap, 0, 0), W32_TRUE);
}

/* ---- time and command line ------------------------------------------------ */

static void test_tickcount(void) {
    CHECK_EQ(GetTickCount64(), 5500);        /* from the clock_gettime stub */
}

static void test_commandline(void) {
    char *argv[] = { (char *)"prog", (char *)"alpha", (char *)"beta gamma", 0 };
    w32_kernel32_init(3, argv);
    /* An argument containing a space is quoted; others are not. */
    CHECK(strcmp(GetCommandLineA(), "prog alpha \"beta gamma\"") == 0);

    w32_kernel32_init(0, 0);
    CHECK(strcmp(GetCommandLineA(), "") == 0);
}

static void test_errno_mapping_table(void) {
    CHECK_EQ(w32_error_from_errno(0),   W32_ERROR_SUCCESS);
    CHECK_EQ(w32_error_from_errno(5),   W32_ERROR_SUCCESS);   /* positive = ok */
    CHECK_EQ(w32_error_from_errno(-2),  W32_ERROR_FILE_NOT_FOUND);
    CHECK_EQ(w32_error_from_errno(-9),  W32_ERROR_INVALID_HANDLE);
    CHECK_EQ(w32_error_from_errno(-12), W32_ERROR_NOT_ENOUGH_MEMORY);
    CHECK_EQ(w32_error_from_errno(-22), W32_ERROR_INVALID_PARAMETER);
    CHECK_EQ(w32_error_from_errno(-24), W32_ERROR_TOO_MANY_OPEN_FILES);
    CHECK_EQ(w32_error_from_errno(-999), W32_ERROR_INVALID_FUNCTION);
}

static void test_lasterror_roundtrip(void) {
    SetLastError(W32_ERROR_ACCESS_DENIED);
    CHECK_EQ(GetLastError(), W32_ERROR_ACCESS_DENIED);
    SetLastError(W32_ERROR_SUCCESS);
    CHECK_EQ(GetLastError(), W32_ERROR_SUCCESS);
}

int main(void) {
    printf("== w32 KERNEL32 translation layer (W32-4) ==\n");

    RUN(test_std_handles_resolve);
    RUN(test_bad_std_handle_selector);
    RUN(test_forged_handles_are_rejected);
    RUN(test_handle_alloc_and_release);
    RUN(test_handle_table_exhaustion);

    RUN(test_writefile_bad_handle);
    RUN(test_writefile_success);
    RUN(test_writefile_null_buffer);
    RUN(test_writefile_zero_length_is_success);
    RUN(test_writefile_errno_mapping);
    RUN(test_writefile_null_written_pointer);

    RUN(test_readfile_bad_handle_and_eof);

    RUN(test_createfile_flag_translation);
    RUN(test_createfile_failures);
    RUN(test_closehandle_paths);

    RUN(test_virtualalloc);
    RUN(test_heap);

    RUN(test_tickcount);
    RUN(test_commandline);
    RUN(test_errno_mapping_table);
    RUN(test_lasterror_roundtrip);

    printf("%s: %d/%d tests passed\n", failed ? "FAIL" : "PASS", passed, tn);
    return failed ? 1 : 0;
}

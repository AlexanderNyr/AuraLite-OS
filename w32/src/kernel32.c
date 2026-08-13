/* kernel32.c — the bounded first import set.  WIN32_PLAN.md phase W32-4.
 *
 * Each export is a translation layer, not an implementation: the work is done
 * by AuraLite's own libc and syscalls, and what happens here is the change of
 * convention -- calling convention, handle representation, error reporting and
 * return-value polarity.  Win32 says "BOOL, zero is failure, detail via
 * GetLastError"; POSIX says "negative is failure, detail in errno".  Every
 * function below is that translation and little else, which is deliberate:
 * anywhere this file starts doing real work is a place a bug can hide that
 * AuraLite's existing tests do not already cover.
 *
 * Every export is W32ABI.  See tests/unit/test_w32_abi.c for why that matters
 * and tests/unit/test_w32_abi_negctl.sh for proof the test would catch its
 * absence.
 */

#include "w32/kernel32.h"

/* The guest headers are skipped when this file is compiled into a host unit
 * test, which stubs the same functions itself (tests/unit/test_w32_kernel32.c).
 * Everything used from them is a POSIX declaration the test provides. */
#ifndef AURALITE_W32_HOST_TEST
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#endif

/* --- process -------------------------------------------------------------- */

W32ABI void ExitProcess(unsigned int code) {
    _exit((int)code);
    for (;;) { }                       /* _exit is noreturn; keep the compiler happy */
}

W32ABI W32_DWORD GetLastError(void)        { return w32_get_last_error_raw(); }
W32ABI void      SetLastError(W32_DWORD c) { w32_set_last_error(c); }

/* --- handles and I/O ------------------------------------------------------ */

W32ABI W32_HANDLE GetStdHandle(W32_DWORD which) {
    switch ((int32_t)which) {
    case (int32_t)W32_STD_INPUT_HANDLE:  return (W32_HANDLE)(intptr_t)W32_STD_INPUT_HANDLE;
    case (int32_t)W32_STD_OUTPUT_HANDLE: return (W32_HANDLE)(intptr_t)W32_STD_OUTPUT_HANDLE;
    case (int32_t)W32_STD_ERROR_HANDLE:  return (W32_HANDLE)(intptr_t)W32_STD_ERROR_HANDLE;
    default:
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_INVALID_HANDLE_VALUE;
    }
}

W32ABI W32_BOOL CloseHandle(W32_HANDLE h) {
    int fd = w32_handle_to_fd(h);
    if (fd < 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    int to_close = w32_handle_release(h);
    if (to_close >= 0) {
        if (close(to_close) < 0) {
            w32_set_last_error(W32_ERROR_INVALID_HANDLE);
            return W32_FALSE;
        }
    }
    /* A standard handle is live but not closable: report success and leave
     * stdout alone.  A program closing its own std handles is common and must
     * not take the stream away from the rest of the process. */
    return W32_TRUE;
}

W32ABI W32_BOOL WriteFile(W32_HANDLE h, const void *buf, W32_DWORD len,
                          W32_DWORD *written, void *overlapped) {
    (void)overlapped;                  /* no async I/O; see D8 */
    if (written) *written = 0;

    int fd = w32_handle_to_fd(h);
    if (fd < 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    if (!buf && len) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (len == 0) { return W32_TRUE; } /* a legal no-op in Win32 */

    ssize_t n = write(fd, buf, (size_t)len);
    if (n < 0) {
        w32_set_last_error(w32_error_from_errno((long)n));
        return W32_FALSE;
    }
    if (written) *written = (W32_DWORD)n;
    return W32_TRUE;
}

W32ABI W32_BOOL ReadFile(W32_HANDLE h, void *buf, W32_DWORD len,
                         W32_DWORD *got, void *overlapped) {
    (void)overlapped;
    if (got) *got = 0;

    int fd = w32_handle_to_fd(h);
    if (fd < 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    if (!buf && len) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (len == 0) return W32_TRUE;

    ssize_t n = read(fd, buf, (size_t)len);
    if (n < 0) {
        w32_set_last_error(w32_error_from_errno((long)n));
        return W32_FALSE;
    }
    if (got) *got = (W32_DWORD)n;
    /* End of file is success with zero bytes in Win32, not an error. */
    return W32_TRUE;
}

W32ABI W32_HANDLE CreateFileA(const char *path, W32_DWORD access,
                              W32_DWORD share, void *sa,
                              W32_DWORD disposition, W32_DWORD flags,
                              W32_HANDLE tmpl) {
    (void)share; (void)sa; (void)flags; (void)tmpl;

    if (!path) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_INVALID_HANDLE_VALUE;
    }

    int oflags;
    int want_read  = (access & W32_GENERIC_READ)  != 0;
    int want_write = (access & W32_GENERIC_WRITE) != 0;

    if (want_read && want_write)  oflags = O_RDWR;
    else if (want_write)          oflags = O_WRONLY;
    else                          oflags = O_RDONLY;

    switch (disposition) {
    case W32_CREATE_ALWAYS:     oflags |= O_CREAT | O_TRUNC; break;
    case W32_CREATE_NEW:        oflags |= O_CREAT;           break;
    case W32_OPEN_ALWAYS:       oflags |= O_CREAT;           break;
    case W32_TRUNCATE_EXISTING: oflags |= O_TRUNC;           break;
    case W32_OPEN_EXISTING:     break;
    default:
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_INVALID_HANDLE_VALUE;
    }

    int fd = open(path, oflags, 0644);
    if (fd < 0) {
        w32_set_last_error(w32_error_from_errno((long)fd));
        return W32_INVALID_HANDLE_VALUE;
    }

    W32_HANDLE h = w32_handle_alloc(fd, 1);
    if (!h) {
        close(fd);
        w32_set_last_error(W32_ERROR_TOO_MANY_OPEN_FILES);
        return W32_INVALID_HANDLE_VALUE;
    }
    return h;
}

/* --- memory ---------------------------------------------------------------
 *
 * VirtualAlloc's full semantics (reserve then commit, MEM_RESERVE without
 * backing) need a VMA model this personality does not have.  What is
 * implemented is the case a CRT actually uses: commit anonymous memory and
 * hand it back.  Anything else is refused rather than half-honoured, so a
 * program relying on reserve-then-commit fails loudly instead of corrupting
 * itself later. */

W32ABI void *VirtualAlloc(void *addr, unsigned long long size,
                          W32_DWORD type, W32_DWORD protect) {
    (void)protect;
    if (size == 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!(type & (W32_MEM_COMMIT | W32_MEM_RESERVE))) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (addr) {
        /* Placement at a caller-chosen address needs the reservation model. */
        w32_set_last_error(W32_ERROR_NOT_SUPPORTED);
        return 0;
    }
    void *p = malloc((size_t)size);
    if (!p) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    memset(p, 0, (size_t)size);        /* Win32 commits zero-filled pages */
    return p;
}

W32ABI W32_BOOL VirtualFree(void *addr, unsigned long long size, W32_DWORD type) {
    if (!addr) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    /* MEM_RELEASE requires size == 0 in Win32; enforcing it catches a common
     * caller bug rather than silently accepting either form. */
    if ((type & W32_MEM_RELEASE) && size != 0) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    free(addr);
    return W32_TRUE;
}

/* One process heap; the token only has to be a stable non-NULL value. */
#define PROCESS_HEAP_TOKEN ((W32_HANDLE)(intptr_t)0x48454150) /* 'HEAP' */

W32ABI W32_HANDLE GetProcessHeap(void) { return PROCESS_HEAP_TOKEN; }

W32ABI void *HeapAlloc(W32_HANDLE heap, W32_DWORD flags, unsigned long long size) {
    if (heap != PROCESS_HEAP_TOKEN) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (size == 0) size = 1;           /* Win32 returns a unique block, not NULL */
    void *p = malloc((size_t)size);
    if (!p) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (flags & 0x8u) memset(p, 0, (size_t)size);   /* HEAP_ZERO_MEMORY */
    return p;
}

W32ABI W32_BOOL HeapFree(W32_HANDLE heap, W32_DWORD flags, void *mem) {
    (void)flags;
    if (heap != PROCESS_HEAP_TOKEN) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    if (mem) free(mem);                /* HeapFree(NULL) is a legal no-op */
    return W32_TRUE;
}

/* --- time ----------------------------------------------------------------- */

W32ABI void Sleep(W32_DWORD ms) {
    struct timespec ts;
    ts.tv_sec  = (long)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, 0);
}

W32ABI W32_ULONGLONG GetTickCount64(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (W32_ULONGLONG)ts.tv_sec * 1000ull
         + (W32_ULONGLONG)(ts.tv_nsec / 1000000L);
}

/* --- command line ---------------------------------------------------------
 *
 * Win32 hands the program one string, not a vector.  Rebuilding it from argv
 * cannot be perfect -- the original quoting is gone by the time argv exists --
 * so the rule here is: quote an argument if and only if it contains a space,
 * which round-trips everything the CRT parser in W32-6 will produce.  Full
 * fidelity needs the raw line from the kernel and is recorded as such. */

static char cmdline[1024];

void w32_kernel32_init(int argc, char **argv) {
    w32_handle_init();
    w32_set_last_error(W32_ERROR_SUCCESS);

    size_t pos = 0;
    for (int i = 0; i < argc && argv && argv[i]; i++) {
        const char *a = argv[i];
        int quote = (strchr(a, ' ') != 0);
        size_t need = strlen(a) + (quote ? 2u : 0u) + (i ? 1u : 0u);
        if (pos + need + 1 >= sizeof cmdline) break;

        if (i) cmdline[pos++] = ' ';
        if (quote) cmdline[pos++] = '"';
        size_t l = strlen(a);
        memcpy(cmdline + pos, a, l);
        pos += l;
        if (quote) cmdline[pos++] = '"';
    }
    cmdline[pos] = '\0';
}

W32ABI const char *GetCommandLineA(void) { return cmdline; }

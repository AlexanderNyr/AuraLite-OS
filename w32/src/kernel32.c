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
#include "w32/w32_module.h"

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
    /* W32A-1: loaded DLLs detach in reverse load order before the process
     * goes away.  A DLL that needed cleanup gets it; nothing outlives us. */
    w32_module_detach_all();
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
    /* W32A-2: the table holds five kinds now, and CloseHandle owns three.
     * Find sessions and change notifications answer to their own closers;
     * mappings and process objects free through the table; plain fds close
     * below (a duplex pipe handle closes both ends). */
    int kind = w32_handle_kind(h);
    if (kind == W32_HANDLE_KIND_FIND || kind == W32_HANDLE_KIND_CHANGE) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    if (kind == W32_HANDLE_KIND_MAP || kind == W32_HANDLE_KIND_PROC) {
        if (w32_handle_release(h) == W32_HANDLE_FREED)
            return W32_TRUE;
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    int fd = w32_handle_to_fd(h);
    if (fd < 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    if (kind < 0) {
        /* A standard pseudo-selector (-10 etc.): live, not a table entry,
         * nothing to close. */
        return W32_TRUE;
    }
    int peer = -1;
    int has_peer = (w32_fs_pipe_drop(h, &peer) == 0);
    w32_fs_note_close(fd);
    if (has_peer && peer >= 0 && peer != fd)
        w32_fs_note_close(peer);
    int to_close = w32_handle_release(h);
    if (to_close < 0) {
        /* A live but non-closable handle (a standard stream): report
         * success and leave stdout alone. */
        return W32_TRUE;
    }
    if (close(to_close) < 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    if (has_peer && peer >= 0 && peer != to_close)
        close(peer);
    return W32_TRUE;
}

W32ABI W32_BOOL WriteFile(W32_HANDLE h, const void *buf, W32_DWORD len,
                          W32_DWORD *written, void *overlapped) {
    W32_OVERLAPPED *ov = (W32_OVERLAPPED *)overlapped;
    if (written) *written = 0;

    int fd = w32_handle_to_fd(h);
    if (fd < 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    /* W32A-2: a duplex pipe handle writes through its write end. */
    {
        int rfd, wfd;
        if (w32_fs_pipe_fds(h, &rfd, &wfd))
            fd = wfd;
    }
    if (!buf && len) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (len == 0) {
        /* A legal no-op in Win32, and it still retires the OVERLAPPED. */
        if (ov) { ov->Internal = 0; ov->InternalHigh = 0; }
        return W32_TRUE;
    }

    ssize_t n = write(fd, buf, (size_t)len);
    if (n < 0) {
        W32_DWORD code = w32_error_from_c(n);
        w32_set_last_error(code);
        if (ov) { ov->Internal = code; ov->InternalHigh = 0; }
        return W32_FALSE;
    }
    if (written) *written = (W32_DWORD)n;
    /* Synchronous I/O retires the OVERLAPPED at once: Internal zero on
     * success (the Win32 code, not an NTSTATUS, on failure) and the count
     * in InternalHigh, so GetOverlappedResult reports it. */
    if (ov) { ov->Internal = 0; ov->InternalHigh = (uint64_t)n; }
    return W32_TRUE;
}

W32ABI W32_BOOL ReadFile(W32_HANDLE h, void *buf, W32_DWORD len,
                         W32_DWORD *got, void *overlapped) {
    W32_OVERLAPPED *ov = (W32_OVERLAPPED *)overlapped;
    if (got) *got = 0;

    int fd = w32_handle_to_fd(h);
    if (fd < 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    /* W32A-2: a duplex pipe handle reads through its read end. */
    {
        int rfd, wfd;
        if (w32_fs_pipe_fds(h, &rfd, &wfd))
            fd = rfd;
    }
    if (!buf && len) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (len == 0) {
        if (ov) { ov->Internal = 0; ov->InternalHigh = 0; }
        return W32_TRUE;
    }

    ssize_t n = read(fd, buf, (size_t)len);
    if (n < 0) {
        W32_DWORD code = w32_error_from_c(n);
        w32_set_last_error(code);
        if (ov) { ov->Internal = code; ov->InternalHigh = 0; }
        return W32_FALSE;
    }
    if (got) *got = (W32_DWORD)n;
    /* End of file is success with zero bytes in Win32, not an error. */
    if (ov) { ov->Internal = 0; ov->InternalHigh = (uint64_t)n; }
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
        w32_set_last_error(w32_error_from_c(fd));
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

W32ABI W32_BOOL VirtualProtect(void *addr, W32_SIZE_T size,
                               W32_DWORD newProt, W32_DWORD *oldProt) {
    if (!addr || size == 0 || !oldProt) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    /* No mprotect syscall exists in AuraLite userspace, so protections can
     * neither change nor be queried.  The one honest answer: every w32
     * block is read/write, so a READWRITE "change" succeeds as a no-op and
     * everything else — including execute-only, which cannot be verified —
     * is refused rather than claimed. */
    if (newProt != W32_PAGE_READWRITE) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    *oldProt = W32_PAGE_READWRITE;
    return W32_TRUE;
}

W32ABI W32_SIZE_T GetLargePageMinimum(void) {
    /* No large-page interface exists; zero is the documented true negative. */
    return 0;
}

/* W32A-2: HeapSize/GlobalSize need to know block sizes, and neither malloc
 * nor the guest libc reports them, so every heap block is recorded in a side
 * table at allocation and forgotten at free.  The table is the only new
 * state; HeapAlloc/HeapFree keep their W32-4 behaviour otherwise. */
#define K32_HEAP_SLOTS 4096

static struct {
    int in_use;
    void *ptr;
    unsigned long long size;
} k32_heap[K32_HEAP_SLOTS];

static void k32_heap_record(void *p, unsigned long long size) {
    size_t i;
    if (!p) return;
    for (i = 0; i < K32_HEAP_SLOTS; i++) {
        if (!k32_heap[i].in_use) {
            k32_heap[i].in_use = 1;
            k32_heap[i].ptr = p;
            k32_heap[i].size = size;
            return;
        }
    }
    /* Full: the block stays allocated but untracked — HeapSize fails for
     * it rather than guess.  4096 live blocks is beyond any fixture. */
}

static void k32_heap_drop(void *p) {
    size_t i;
    if (!p) return;
    for (i = 0; i < K32_HEAP_SLOTS; i++) {
        if (k32_heap[i].in_use && k32_heap[i].ptr == p) {
            k32_heap[i].in_use = 0;
            return;
        }
    }
}

static int k32_heap_size(void *p, unsigned long long *out) {
    size_t i;
    for (i = 0; i < K32_HEAP_SLOTS; i++) {
        if (k32_heap[i].in_use && k32_heap[i].ptr == p) {
            *out = k32_heap[i].size;
            return 1;
        }
    }
    return 0;
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
    k32_heap_record(p, size);
    return p;
}

W32ABI W32_BOOL HeapFree(W32_HANDLE heap, W32_DWORD flags, void *mem) {
    (void)flags;
    if (heap != PROCESS_HEAP_TOKEN) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    if (mem) { k32_heap_drop(mem); free(mem); }  /* NULL is a legal no-op */
    return W32_TRUE;
}

W32ABI void *HeapReAlloc(W32_HANDLE heap, W32_DWORD flags, void *mem,
                         unsigned long long size) {
    unsigned long long old = 0;
    void *p;
    if (heap != PROCESS_HEAP_TOKEN) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (flags & ~W32_HEAP_ZERO_MEMORY) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!mem)
        return HeapAlloc(heap, flags, size);
    if (size == 0) {
        HeapFree(heap, flags, mem);
        return 0;
    }
    k32_heap_size(mem, &old);
    k32_heap_drop(mem);         /* before realloc: nothing dangles after */
    p = realloc(mem, (size_t)size);
    if (!p) {
        k32_heap_record(mem, old);      /* realloc failed: mem still live */
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    k32_heap_record(p, size);
    if ((flags & W32_HEAP_ZERO_MEMORY) && size > old)
        memset((char *)p + old, 0, (size_t)(size - old));
    return p;
}

W32ABI W32_SIZE_T HeapSize(W32_HANDLE heap, W32_DWORD flags, const void *mem) {
    unsigned long long size = 0;
    (void)flags;
    if (heap != PROCESS_HEAP_TOKEN || !mem) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return (W32_SIZE_T)-1;
    }
    if (!k32_heap_size((void *)mem, &size)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return (W32_SIZE_T)-1;
    }
    return (W32_SIZE_T)size;
}

/* Global/Local: the fixed model.  Handles ARE pointers, Lock is the
 * identity, and MOVEABLE is accepted because a fixed block satisfies every
 * moveable caller (locking still works — it just never moves).  GlobalSize
 * reads the same table HeapSize does. */
W32ABI void *GlobalAlloc(W32_UINT flags, W32_SIZE_T size) {
    void *p;
    if (flags & ~(W32_GMEM_FIXED | W32_GMEM_MOVEABLE | W32_GMEM_ZEROINIT)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (size == 0) size = 1;
    p = malloc((size_t)size);
    if (!p) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (flags & W32_GMEM_ZEROINIT) memset(p, 0, (size_t)size);
    k32_heap_record(p, size);
    return p;
}

W32ABI void *GlobalLock(void *h) {
    if (!h) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return h;
}

W32ABI W32_BOOL GlobalUnlock(void *h) {
    if (!h) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    /* Nothing is ever locked, so the count is already zero: FALSE with
     * NO_ERROR is the documented shape, not a failure. */
    w32_set_last_error(W32_ERROR_SUCCESS);
    return W32_FALSE;
}

W32ABI void *GlobalFree(void *h) {
    if (!h) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    k32_heap_drop(h);
    free(h);
    return 0;
}

W32ABI W32_SIZE_T GlobalSize(void *h) {
    unsigned long long size = 0;
    if (!h || !k32_heap_size(h, &size))
        return 0;
    return (W32_SIZE_T)size;
}

W32ABI void *LocalAlloc(W32_UINT flags, W32_SIZE_T size) {
    void *p;
    if (flags & ~(W32_LMEM_FIXED | W32_LMEM_MOVEABLE | W32_LMEM_ZEROINIT)) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (size == 0) size = 1;
    p = malloc((size_t)size);
    if (!p) {
        w32_set_last_error(W32_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (flags & W32_LMEM_ZEROINIT) memset(p, 0, (size_t)size);
    k32_heap_record(p, size);
    return p;
}

W32ABI void *LocalFree(void *h) {
    if (!h) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    k32_heap_drop(h);
    free(h);
    return 0;
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

/* W32A-2: the 32-bit tick is the low half of the 64-bit one, wrapping the
 * way the documented API wraps. */
W32ABI W32_DWORD GetTickCount(void) {
    return (W32_DWORD)GetTickCount64();
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
    w32_fs_init();
    w32_ps_init(argc, argv, NULL);
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

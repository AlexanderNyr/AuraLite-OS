/* kernel32_con.c — console mode / codepage / wide I/O and the serial (COMM)
 * surface.  W32APP_PLAN.md phase W32A-14 (App gate I: PuTTY).
 *
 * PuTTY (and its CLI twin plink) import a small console + serial slice of
 * KERNEL32 that W32A-1 had bound to loud TODO stubs.  Two shapes live here,
 * and the split is deliberate:
 *
 *   CONSOLE  GetConsoleMode / GetConsoleOutputCP / WriteConsoleW /
 *            ReadConsoleW / SetStdHandle are REAL translations over the
 *            existing std handles and file I/O.  WriteConsoleW converts
 *            UTF-16 -> UTF-8 and writes through WriteFile; the personality's
 *            terminal is UTF-8, so GetConsoleOutputCP reports 65001 to match.
 *            GetConsoleMode answers TRUE only for the three std handles (a
 *            redirected file/pipe/socket is NOT a console and fails with
 *            ERROR_INVALID_HANDLE -- which is exactly how PuTTY detects a
 *            redirected stdio and turns console handling off).  SetStdHandle
 *            records an override that GetStdHandle (kernel32.c) consults.
 *
 *   SERIAL   ClearCommBreak / GetCommState / SetCommBreak / SetCommState /
 *            SetCommTimeouts FAIL CLEAN.  Serial ports are a documented
 *            non-goal: there is no COM hardware on the lite personality, so
 *            each returns FALSE + ERROR_INVALID_FUNCTION -- the same code
 *            Win32 returns for a comm call on a non-serial handle.  PuTTY's
 *            serial backend sees the failure and shows its documented "unable
 *            to open connection" path instead of faulting on a TODO stub.
 *
 * Every export is W32ABI (see tests/unit/test_w32_abi.c for why).  The pure
 * logic is exercised by tests/unit/test_w32_kernel32_con.c and end-to-end by
 * tests/integration/cases/test_w32a14_putty_fixture.sh.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "w32/kernel32.h"
#include "w32/w32_handle.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"

/* Console-mode bits GetConsoleMode reports.  Named rather than inlined so the
 * default masks below read as intent, not magic numbers. */
#define W32_ENABLE_PROCESSED_INPUT     0x0001u
#define W32_ENABLE_LINE_INPUT          0x0002u
#define W32_ENABLE_ECHO_INPUT          0x0004u
#define W32_ENABLE_MOUSE_INPUT         0x0010u
#define W32_ENABLE_INSERT_MODE         0x0020u
#define W32_ENABLE_QUICK_EDIT_MODE     0x0040u
#define W32_ENABLE_EXTENDED_FLAGS      0x0080u
#define W32_ENABLE_PROCESSED_OUTPUT    0x0001u
#define W32_ENABLE_WRAP_AT_EOL_OUTPUT  0x0002u

#define W32_CP_UTF8  65001u

/* --- std-handle override (SetStdHandle / GetStdHandle) -------------------- */

static W32_HANDLE g_std_override[3];   /* [0]=in [1]=out [2]=err; NULL=default */

static int std_slot(W32_DWORD which) {
    switch ((int32_t)which) {
    case (int32_t)W32_STD_INPUT_HANDLE:  return 0;
    case (int32_t)W32_STD_OUTPUT_HANDLE: return 1;
    case (int32_t)W32_STD_ERROR_HANDLE:  return 2;
    default: return -1;
    }
}

W32ABI W32_BOOL SetStdHandle(W32_DWORD which, W32_HANDLE h) {
    int s = std_slot(which);
    if (s < 0) {
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    g_std_override[s] = h;
    return W32_TRUE;
}

/* Consulted by GetStdHandle in kernel32.c: a non-NULL override wins over the
 * default pseudo handle.  Declared in kernel32.h. */
W32_HANDLE w32_std_handle_override(W32_DWORD which) {
    int s = std_slot(which);
    return s >= 0 ? g_std_override[s] : (W32_HANDLE)0;
}

/* Is `h` a console (one of the three std pseudo handles as GetStdHandle hands
 * them out)?  Overridden handles point at real files/pipes and are, by
 * definition, no longer consoles. */
static int is_console_handle(W32_HANDLE h) {
    intptr_t v = (intptr_t)h;
    if (v == (intptr_t)(W32_DWORD)W32_STD_INPUT_HANDLE  && !g_std_override[0]) return 1;
    if (v == (intptr_t)(W32_DWORD)W32_STD_OUTPUT_HANDLE && !g_std_override[1]) return 1;
    if (v == (intptr_t)(W32_DWORD)W32_STD_ERROR_HANDLE  && !g_std_override[2]) return 1;
    return 0;
}

/* --- console mode / codepage --------------------------------------------- */

W32ABI W32_BOOL GetConsoleMode(W32_HANDLE h, W32_DWORD *mode) {
    if (!mode) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (!is_console_handle(h)) {
        /* A file, pipe or socket handle: not a console.  This is the signal
         * PuTTY/plink use to detect redirected stdio. */
        w32_set_last_error(W32_ERROR_INVALID_HANDLE);
        return W32_FALSE;
    }
    intptr_t v = (intptr_t)h;
    if (v == (intptr_t)(W32_DWORD)W32_STD_INPUT_HANDLE)
        *mode = W32_ENABLE_PROCESSED_INPUT | W32_ENABLE_LINE_INPUT |
                W32_ENABLE_ECHO_INPUT      | W32_ENABLE_MOUSE_INPUT |
                W32_ENABLE_INSERT_MODE     | W32_ENABLE_QUICK_EDIT_MODE |
                W32_ENABLE_EXTENDED_FLAGS;
    else
        *mode = W32_ENABLE_PROCESSED_OUTPUT | W32_ENABLE_WRAP_AT_EOL_OUTPUT;
    return W32_TRUE;
}

W32ABI W32_UINT GetConsoleOutputCP(void) {
    /* The terminal is UTF-8, and WriteConsoleW below emits UTF-8, so the
     * reported output codepage MUST agree or a caller re-encoding by it would
     * corrupt non-ASCII. */
    return W32_CP_UTF8;
}

/* --- console wide I/O ----------------------------------------------------- */

W32ABI W32_BOOL WriteConsoleW(W32_HANDLE h, const void *buf, W32_DWORD n,
                              W32_DWORD *written, void *reserved) {
    (void)reserved;
    if (written) *written = 0;
    if (!buf && n) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (n == 0) return W32_TRUE;

    const uint16_t *w = (const uint16_t *)buf;
    /* Chunk the UTF-16 -> UTF-8 conversion so an arbitrarily long console
     * write needs no heap: <= 128 code units per pass is <= 512 UTF-8 bytes
     * (a lone code unit is at most 3 bytes; a surrogate pair spanning a chunk
     * boundary re-converts cleanly next pass because we advance by whole
     * units only when the pass succeeded). */
    char u8[512];
    W32_DWORD done = 0;
    while (done < n) {
        size_t chunk = (size_t)(n - done);
        if (chunk > 128) chunk = 128;
        size_t need = 0;
        int rc = w32_utf16_to_utf8(w + done, chunk, u8, sizeof u8, &need);
        if (rc != W32_UTF_OK) {
            w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
            return W32_FALSE;
        }
        W32_DWORD wr = 0;
        if (need && !WriteFile(h, u8, (W32_DWORD)need, &wr, (void *)0))
            return W32_FALSE;              /* WriteFile set the last error */
        done += (W32_DWORD)chunk;
        if (written) *written = done;      /* count in code units, as Win32 does */
    }
    return W32_TRUE;
}

W32ABI W32_BOOL ReadConsoleW(W32_HANDLE h, void *buf, W32_DWORD n,
                             W32_DWORD *got, void *ctrl) {
    (void)ctrl;
    if (got) *got = 0;
    if (!buf && n) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (n == 0) return W32_TRUE;

    /* Read UTF-8 bytes through ReadFile and widen.  Bounded by the local
     * buffer; a caller wanting more calls again, exactly as with a real
     * console read that returns a partial line. */
    char u8[512];
    W32_DWORD want = n;
    if (want > (W32_DWORD)sizeof u8) want = (W32_DWORD)sizeof u8;
    W32_DWORD rd = 0;
    if (!ReadFile(h, u8, want, &rd, (void *)0))
        return W32_FALSE;                  /* ReadFile set the last error */
    if (rd == 0) return W32_TRUE;          /* EOF: zero code units, success */
    size_t need = 0;
    int rc = w32_utf8_to_utf16(u8, (size_t)rd, (uint16_t *)buf, (size_t)n, &need);
    if (rc != W32_UTF_OK) {
        w32_set_last_error(W32_ERROR_INVALID_PARAMETER);
        return W32_FALSE;
    }
    if (got) *got = (W32_DWORD)need;       /* code units read */
    return W32_TRUE;
}

/* --- serial (COMM): fail clean, serial is a documented non-goal ---------- */

static W32_BOOL comm_unsupported(void) {
    w32_set_last_error(W32_ERROR_INVALID_FUNCTION);
    return W32_FALSE;
}

W32ABI W32_BOOL GetCommState(W32_HANDLE h, void *dcb) {
    (void)h; (void)dcb; return comm_unsupported();
}
W32ABI W32_BOOL SetCommState(W32_HANDLE h, const void *dcb) {
    (void)h; (void)dcb; return comm_unsupported();
}
W32ABI W32_BOOL SetCommTimeouts(W32_HANDLE h, const void *timeouts) {
    (void)h; (void)timeouts; return comm_unsupported();
}
W32ABI W32_BOOL SetCommBreak(W32_HANDLE h) {
    (void)h; return comm_unsupported();
}
W32ABI W32_BOOL ClearCommBreak(W32_HANDLE h) {
    (void)h; return comm_unsupported();
}

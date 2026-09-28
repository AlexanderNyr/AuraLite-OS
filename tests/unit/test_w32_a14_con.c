/* W32A-14: host unit test for the console / serial ABI-boundary logic.
 *
 * The in-guest QEMU gate (test_w32a14_putty_fixture.sh) proves the whole PuTTY
 * app slice end to end; this test amalgamates kernel32_con.c (+ the pure UTF
 * converter it uses) against hand-written WriteFile/ReadFile/last-error doubles
 * and exercises the parts that are pure boundary logic:
 *
 *   - GetConsoleOutputCP reports UTF-8 (65001), matching WriteConsoleW.
 *   - GetConsoleMode: TRUE with sane bits for each std handle, FALSE +
 *     ERROR_INVALID_HANDLE for a non-console handle, and FALSE once a handle
 *     has been overridden away from its pseudo (SetStdHandle redirect).
 *   - WriteConsoleW: UTF-16 -> UTF-8 widening (ASCII, a multibyte code point,
 *     and a >128-unit run that forces the internal chunk loop), with the
 *     Win32 code-unit written count.
 *   - ReadConsoleW: UTF-8 -> UTF-16 narrowing from the ReadFile double, EOF.
 *   - SetStdHandle / w32_std_handle_override round-trip.
 *   - the serial COMM set failing CLEAN (FALSE + ERROR_INVALID_FUNCTION).
 *
 * Sanitizers on.  SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "w32/kernel32.h"
#include "w32/w32_handle.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"

/* ---- last-error doubles (the ws2_32 test's pattern) -------------------- */
static W32_DWORD g_lasterr;
void      w32_set_last_error(W32_DWORD e) { g_lasterr = e; }
W32_DWORD w32_get_last_error_raw(void)    { return g_lasterr; }

/* ---- WriteFile / ReadFile doubles ------------------------------------- */
static unsigned char wbuf[8192];
static unsigned      wlen;
W32ABI W32_BOOL WriteFile(W32_HANDLE h, const void *buf, W32_DWORD len,
                          W32_DWORD *written, void *overlapped) {
    (void)h; (void)overlapped;
    const unsigned char *p = (const unsigned char *)buf;
    for (W32_DWORD i = 0; i < len; i++)
        if (wlen < sizeof wbuf) wbuf[wlen++] = p[i];
    if (written) *written = len;
    return W32_TRUE;
}
static unsigned char rbuf[256];
static unsigned      rlen, rpos;
W32ABI W32_BOOL ReadFile(W32_HANDLE h, void *buf, W32_DWORD len,
                         W32_DWORD *got, void *overlapped) {
    (void)h; (void)overlapped;
    unsigned char *p = (unsigned char *)buf;
    W32_DWORD k = 0;
    while (k < len && rpos < rlen) p[k++] = rbuf[rpos++];
    if (got) *got = k;
    return W32_TRUE;
}

/* the units under test */
#include "../../w32/src/w32_utf.c"
#include "../../w32/src/kernel32_con.c"

#define H(x) ((W32_HANDLE)(intptr_t)(W32_DWORD)(x))

static int n, f;
#define CHECK(x) do { ++n; if (!(x)) { ++f; \
    fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); } } while (0)

int main(void) {
    /* ---- codepage --------------------------------------------------- */
    CHECK(GetConsoleOutputCP() == 65001);

    /* ---- GetConsoleMode --------------------------------------------- */
    W32_DWORD mode = 0;
    CHECK(GetConsoleMode(H(W32_STD_OUTPUT_HANDLE), &mode) == W32_TRUE);
    CHECK(mode != 0);
    mode = 0;
    CHECK(GetConsoleMode(H(W32_STD_INPUT_HANDLE), &mode) == W32_TRUE);
    CHECK(mode != 0);
    /* not a console */
    g_lasterr = 0;
    CHECK(GetConsoleMode(H(0x1234), &mode) == W32_FALSE);
    CHECK(g_lasterr == W32_ERROR_INVALID_HANDLE);
    /* NULL out-pointer */
    g_lasterr = 0;
    CHECK(GetConsoleMode(H(W32_STD_OUTPUT_HANDLE), NULL) == W32_FALSE);
    CHECK(g_lasterr == W32_ERROR_INVALID_PARAMETER);

    /* ---- WriteConsoleW: ASCII --------------------------------------- */
    wlen = 0;
    {
        const uint16_t s[] = { 'A', 'B', 'C' };
        W32_DWORD wr = 0;
        CHECK(WriteConsoleW(H(W32_STD_OUTPUT_HANDLE), s, 3, &wr, NULL) == W32_TRUE);
        CHECK(wr == 3);
        CHECK(wlen == 3 && wbuf[0] == 'A' && wbuf[2] == 'C');
    }
    /* ---- WriteConsoleW: a multibyte code point (U+00E9 -> C3 A9) ----- */
    wlen = 0;
    {
        const uint16_t s[] = { 0x00E9 };
        W32_DWORD wr = 0;
        CHECK(WriteConsoleW(H(W32_STD_OUTPUT_HANDLE), s, 1, &wr, NULL) == W32_TRUE);
        CHECK(wr == 1);
        CHECK(wlen == 2 && wbuf[0] == 0xC3 && wbuf[1] == 0xA9);
    }
    /* ---- WriteConsoleW: >128 units, forces the chunk loop ------------ */
    wlen = 0;
    {
        uint16_t big[200];
        for (int i = 0; i < 200; i++) big[i] = (uint16_t)('a' + (i % 26));
        W32_DWORD wr = 0;
        CHECK(WriteConsoleW(H(W32_STD_OUTPUT_HANDLE), big, 200, &wr, NULL) == W32_TRUE);
        CHECK(wr == 200);
        CHECK(wlen == 200);       /* all ASCII -> one byte each */
    }
    /* ---- WriteConsoleW: zero length is a legal no-op ----------------- */
    {
        W32_DWORD wr = 123;
        CHECK(WriteConsoleW(H(W32_STD_OUTPUT_HANDLE), NULL, 0, &wr, NULL) == W32_TRUE);
        CHECK(wr == 0);
    }

    /* ---- ReadConsoleW: UTF-8 in -> UTF-16 out ----------------------- */
    {
        rbuf[0] = 'h'; rbuf[1] = 'i'; rlen = 2; rpos = 0;
        uint16_t got_buf[8];
        memset(got_buf, 0, sizeof got_buf);
        W32_DWORD got = 0;
        CHECK(ReadConsoleW(H(W32_STD_INPUT_HANDLE), got_buf, 8, &got, NULL) == W32_TRUE);
        CHECK(got == 2 && got_buf[0] == 'h' && got_buf[1] == 'i');
    }
    /* ---- ReadConsoleW: EOF (nothing to read) ------------------------ */
    {
        rlen = 0; rpos = 0;
        uint16_t got_buf[4];
        W32_DWORD got = 99;
        CHECK(ReadConsoleW(H(W32_STD_INPUT_HANDLE), got_buf, 4, &got, NULL) == W32_TRUE);
        CHECK(got == 0);
    }

    /* ---- SetStdHandle / override ------------------------------------ */
    {
        W32_HANDLE fake = (W32_HANDLE)(intptr_t)0xDEAD;
        CHECK(w32_std_handle_override(W32_STD_ERROR_HANDLE) == (W32_HANDLE)0);
        CHECK(SetStdHandle(W32_STD_ERROR_HANDLE, fake) == W32_TRUE);
        CHECK(w32_std_handle_override(W32_STD_ERROR_HANDLE) == fake);
        /* once overridden, the pseudo is no longer a console */
        W32_DWORD m = 0;
        CHECK(GetConsoleMode(H(W32_STD_ERROR_HANDLE), &m) == W32_FALSE);
        /* an unknown "which" is rejected */
        g_lasterr = 0;
        CHECK(SetStdHandle(42, fake) == W32_FALSE);
        CHECK(g_lasterr == W32_ERROR_INVALID_HANDLE);
        SetStdHandle(W32_STD_ERROR_HANDLE, (W32_HANDLE)0);   /* restore */
    }

    /* ---- serial COMM: fail clean ------------------------------------ */
    {
        W32_HANDLE h = (W32_HANDLE)(intptr_t)0x2222;
        char dcb[64]; memset(dcb, 0, sizeof dcb);
        char to[32];  memset(to, 0, sizeof to);
        g_lasterr = 0;
        CHECK(GetCommState(h, dcb) == W32_FALSE);
        CHECK(g_lasterr == W32_ERROR_INVALID_FUNCTION);
        CHECK(SetCommState(h, dcb) == W32_FALSE);
        CHECK(SetCommTimeouts(h, to) == W32_FALSE);
        CHECK(SetCommBreak(h) == W32_FALSE);
        CHECK(ClearCommBreak(h) == W32_FALSE);
        CHECK(g_lasterr == W32_ERROR_INVALID_FUNCTION);
    }

    if (f == 0) printf("W32A14-CON-OK (%d checks)\n", n);
    else        printf("W32A14-CON-FAIL (%d/%d failed)\n", f, n);
    return f ? 1 : 0;
}

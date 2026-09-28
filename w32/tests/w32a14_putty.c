/* w32/tests/w32a14_putty.c — W32APP_PLAN.md phase W32A-14 guest fixture.
 *
 * The CI-automatable twin of the PuTTY app gate.  A single mingw-w64 TU linked
 * -nostdlib with --entry=winstart (no CRT, no main) that imports the exact
 * KERNEL32 / USER32 / ADVAPI32 / WS2_32 surface a real PuTTY session drives,
 * BY NAME, and exercises the personality paths PuTTY actually walks:
 *
 *   SESSION PERSISTENCE  the advapi32 Reg*A set PuTTY saves/loads a session
 *       with -- create HKCU\Software\SimonTatham\PuTTY\Sessions\<name>, write
 *       HostName (REG_SZ) + PortNumber (REG_DWORD), close, reopen, read back
 *       and ASSERT the values round-trip.  This is PuTTY's config store.
 *
 *   CONSOLE PERSONALITY  the W32A-14 REAL console slice: GetConsoleOutputCP
 *       (UTF-8), GetConsoleMode on a console handle (TRUE) and on a non-console
 *       (FALSE), WriteConsoleW widening to the UTF-8 terminal, and a
 *       SetStdHandle/GetStdHandle override round-trip.
 *
 *   TERMINAL WINDOW PERSONALITY  ShowCursor's displacement counter, SetCursor's
 *       previous-cursor return, MessageBeep's bell, and a class-long round-trip
 *       (SetClassLongPtrA returns the value the previous call installed) on a
 *       real registered window class -- the four USER32 verbs W32A-1 had bound
 *       to loud TODO stubs that a PuTTY terminal hits on its first keystroke.
 *
 *   SERIAL NEGATIVE FLOW  the COMM set FAILS CLEAN (GetCommState/SetCommState/
 *       SetCommTimeouts/SetCommBreak/ClearCommBreak all return FALSE): serial
 *       is a documented non-goal, and PuTTY's serial backend must see a clean
 *       failure, not a fault, to show its "unable to open" path.
 *
 *   DYNAMIC WINSOCK  LoadLibraryA("WS2_32.DLL") + GetProcAddress for WSAStartup/
 *       socket/closesocket/WSACleanup, then a real call chain -- PuTTY resolves
 *       WinSock dynamically, so the app gate must prove that path, not just the
 *       static IAT one W32A-12 covered.
 *
 * Prints W32A14-PUTTY-OK and exits 78 on a clean run; FAIL-<mark> +
 * W32A14-PUTTY-FAIL / exit 1 otherwise.  Markers greppable by
 * tests/integration/cases/test_w32a14_putty_fixture.sh.
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <winsock2.h>
#include <windows.h>
#include <stdint.h>

/* -nostdlib: the compiler may still emit these for copies/inits/zeroing. */
void *memset(void *d, int c, unsigned long long n) {
    volatile unsigned char *p = (volatile unsigned char *)d;
    while (n-- > 0) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long long n) {
    volatile unsigned char *p = (volatile unsigned char *)d;
    volatile const unsigned char *q = (volatile const unsigned char *)s;
    while (n-- > 0) *p++ = *q++;
    return d;
}
unsigned long long strlen(const char *s) {
    unsigned long long n = 0;
    while (s[n]) n++;
    return n;
}
static int streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static HANDLE out;
static DWORD  written;
static int    fails;

static void say(const char *s) {
    DWORD n = 0;
    while (s[n]) n++;
    WriteFile(out, s, n, &written, NULL);
}
static void mark_fail(const char *m) { say("FAIL-"); say(m); say("\r\n"); fails++; }
#define CHECK(cond, mark) do { if (!(cond)) mark_fail(mark); } while (0)

/* Dynamic WS2_32 entry-point types (PuTTY resolves these at run time). */
typedef int      (WINAPI *pWSAStartup)(WORD, LPWSADATA);
typedef SOCKET   (WINAPI *psocket)(int, int, int);
typedef int      (WINAPI *pclosesocket)(SOCKET);
typedef int      (WINAPI *pWSACleanup)(void);

void __stdcall winstart(void) {
    LONG   rc;
    HKEY   key = NULL;

    out   = GetStdHandle(STD_OUTPUT_HANDLE);
    fails = 0;
    say("W32A14: PuTTY app-gate fixture start\r\n");

    /* ---- session persistence: PuTTY's Reg*A save/load round-trip -------- */
    {
        const char *path =
            "Software\\SimonTatham\\PuTTY\\Sessions\\w32a14test";
        DWORD disp = 0;
        rc = RegCreateKeyExA(HKEY_CURRENT_USER, path, 0, NULL, 0,
                             KEY_READ | KEY_WRITE, NULL, &key, &disp);
        CHECK(rc == ERROR_SUCCESS && key, "reg-create");

        const char *host = "example.com";
        rc = RegSetValueExA(key, "HostName", 0, REG_SZ,
                            (const BYTE *)host, (DWORD)(strlen(host) + 1));
        CHECK(rc == ERROR_SUCCESS, "reg-set-host");

        DWORD port = 22;
        rc = RegSetValueExA(key, "PortNumber", 0, REG_DWORD,
                            (const BYTE *)&port, sizeof port);
        CHECK(rc == ERROR_SUCCESS, "reg-set-port");
        RegCloseKey(key);
        key = NULL;

        /* reopen the way PuTTY does when a saved session is loaded */
        rc = RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_READ, &key);
        CHECK(rc == ERROR_SUCCESS && key, "reg-reopen");

        char  hbuf[64];
        DWORD hlen = sizeof hbuf, type = 0;
        memset(hbuf, 0, sizeof hbuf);
        rc = RegQueryValueExA(key, "HostName", NULL, &type,
                              (BYTE *)hbuf, &hlen);
        CHECK(rc == ERROR_SUCCESS && type == REG_SZ, "reg-get-host-rc");
        CHECK(streq(hbuf, "example.com"), "reg-host-value");

        DWORD pval = 0, plen = sizeof pval;
        type = 0;
        rc = RegQueryValueExA(key, "PortNumber", NULL, &type,
                              (BYTE *)&pval, &plen);
        CHECK(rc == ERROR_SUCCESS && type == REG_DWORD, "reg-get-port-rc");
        CHECK(pval == 22, "reg-port-value");
        RegCloseKey(key);
        key = NULL;
    }

    /* ---- console personality (W32A-14 REAL slice) ----------------------- */
    {
        UINT cp = GetConsoleOutputCP();
        CHECK(cp == 65001, "console-cp-utf8");

        DWORD mode = 0;
        CHECK(GetConsoleMode(out, &mode) == TRUE, "console-mode-ok");
        CHECK(mode != 0, "console-mode-bits");

        /* a non-console handle is NOT a console */
        DWORD m2 = 0;
        CHECK(GetConsoleMode((HANDLE)(intptr_t)0x1234, &m2) == FALSE,
              "console-mode-nonconsole");

        /* WriteConsoleW widens UTF-16 to the UTF-8 terminal */
        const wchar_t *w = L"W32A14 console: ok\r\n";
        DWORD nw = 0, wlen = 0;
        while (w[wlen]) wlen++;
        CHECK(WriteConsoleW(out, w, wlen, &nw, NULL) == TRUE, "writeconsolew");
        CHECK(nw == wlen, "writeconsolew-count");

        /* SetStdHandle/GetStdHandle override round-trip (on STD_ERROR so our
         * own stdout stays intact) */
        HANDLE saved = GetStdHandle(STD_ERROR_HANDLE);
        CHECK(SetStdHandle(STD_ERROR_HANDLE, out) == TRUE, "setstdhandle");
        CHECK(GetStdHandle(STD_ERROR_HANDLE) == out, "getstdhandle-override");
        CHECK(SetStdHandle(STD_ERROR_HANDLE, saved) == TRUE, "setstdhandle-restore");
    }

    /* ---- terminal-window personality (cursor / bell / class-long) ------- */
    {
        CHECK(MessageBeep(MB_OK) == TRUE, "messagebeep");

        /* ShowCursor is a displacement counter, not a boolean */
        int base = ShowCursor(FALSE);         /* -1 relative to start */
        int back = ShowCursor(TRUE);          /* back to start */
        CHECK(back == base + 1, "showcursor-counter");

        /* SetCursor returns the previously-set cursor */
        HCURSOR c1 = (HCURSOR)(intptr_t)0x1001;
        HCURSOR c2 = (HCURSOR)(intptr_t)0x1002;
        SetCursor(c1);
        HCURSOR prev = SetCursor(c2);
        CHECK(prev == c1, "setcursor-previous");

        /* class-long round-trip on a real window class */
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize        = sizeof wc;
        wc.lpfnWndProc   = DefWindowProcA;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.lpszClassName = "W32A14PuTTYClass";
        ATOM a = RegisterClassExA(&wc);
        CHECK(a != 0, "registerclass");

        HWND hwnd = CreateWindowExA(0, "W32A14PuTTYClass", "w32a14",
                                    WS_OVERLAPPEDWINDOW, 0, 0, 320, 240,
                                    NULL, NULL, wc.hInstance, NULL);
        CHECK(hwnd != NULL, "createwindow");
        if (hwnd) {
            /* PuTTY stamps its small icon into the class; first call returns
             * the previous (0), the second returns what the first installed. */
            LONG_PTR old = SetClassLongPtrA(hwnd, GCLP_HICONSM,
                                            (LONG_PTR)(intptr_t)0xABCD);
            CHECK(old == 0, "classlong-first-prev");
            LONG_PTR again = SetClassLongPtrA(hwnd, GCLP_HICONSM,
                                              (LONG_PTR)(intptr_t)0x1234);
            CHECK(again == (LONG_PTR)(intptr_t)0xABCD, "classlong-roundtrip");
            DestroyWindow(hwnd);
        }
    }

    /* ---- serial negative flow: the COMM set fails clean ----------------- */
    {
        /* PuTTY opens the port with CreateFile first; on the lite personality
         * there is no COM device, but even a stray handle must fail cleanly
         * through the comm calls, never fault on a TODO stub. */
        HANDLE h = (HANDLE)(intptr_t)0x2222;   /* not a serial handle */
        DCB dcb;
        memset(&dcb, 0, sizeof dcb);
        dcb.DCBlength = sizeof dcb;
        CHECK(GetCommState(h, &dcb) == FALSE, "commstate-get-failclean");
        CHECK(SetCommState(h, &dcb) == FALSE, "commstate-set-failclean");
        COMMTIMEOUTS to;
        memset(&to, 0, sizeof to);
        CHECK(SetCommTimeouts(h, &to) == FALSE, "commtimeouts-failclean");
        CHECK(SetCommBreak(h) == FALSE, "commbreak-set-failclean");
        CHECK(ClearCommBreak(h) == FALSE, "commbreak-clear-failclean");
    }

    /* ---- dynamic WinSock: PuTTY's LoadLibrary+GetProcAddress path ------- */
    {
        HMODULE ws = LoadLibraryA("WS2_32.DLL");
        CHECK(ws != NULL, "loadlibrary-ws2_32");
        if (ws) {
            pWSAStartup   fStartup = (pWSAStartup)  (void *)GetProcAddress(ws, "WSAStartup");
            psocket       fSocket  = (psocket)      (void *)GetProcAddress(ws, "socket");
            pclosesocket  fClose   = (pclosesocket) (void *)GetProcAddress(ws, "closesocket");
            pWSACleanup   fCleanup = (pWSACleanup)  (void *)GetProcAddress(ws, "WSACleanup");
            CHECK(fStartup && fSocket && fClose && fCleanup, "getprocaddress-ws2_32");
            if (fStartup && fSocket && fClose && fCleanup) {
                WSADATA wsa;
                memset(&wsa, 0, sizeof wsa);
                CHECK(fStartup(MAKEWORD(2, 2), &wsa) == 0, "dyn-wsastartup");
                SOCKET s = fSocket(AF_INET, SOCK_STREAM, 0);
                CHECK(s != INVALID_SOCKET, "dyn-socket");
                if (s != INVALID_SOCKET) CHECK(fClose(s) == 0, "dyn-closesocket");
                CHECK(fCleanup() == 0, "dyn-wsacleanup");
            }
        }
    }

    if (fails == 0) {
        say("W32A14-PUTTY-OK\r\n");
        ExitProcess(78);
    }
    say("W32A14-PUTTY-FAIL\r\n");
    ExitProcess(1);
}

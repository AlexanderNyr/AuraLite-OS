/* w32/examples/unsupported-app/registry.c — a program that runs on stubs.
 *
 * WIN32_PLAN.md phase W32-8, updated for the W32A-1 binding contract.
 * Apache-2.0; mingw-w64's public-domain <windows.h> declarations, none
 * of its runtime.
 *
 * This example exists to demonstrate the failure mode, which is as much a
 * part of a personality's contract as the things that work.
 *
 * It uses the registry (ADVAPI32), which is an explicit non-goal until
 * W32A-9 -- decision D8 in WIN32_PLAN.md.  The point is WHERE and HOW
 * it fails:
 *
 *   - not at LOAD time: since W32A-1 the loader binds unimplemented
 *     imports to loud phase-owned stubs ("binding is not behaviour"),
 *     so every import of this program binds and it starts normally;
 *   - but at the FIRST CALL through a stub, which logs the DLL, the
 *     symbol and the owning phase, sets a guest-visible error, and
 *     returns -- the program runs until the missing import, and the
 *     missing import fails loudly instead of crashing somewhere with
 *     no explanation.
 *
 * Expected output on AuraLite:
 *
 *     w32run: /tests/w32unsup.exe mapped at 0x400000000000, 5 import(s) bound
 *     w32unsup: running with stubbed ADVAPI32
 *     w32: TODO advapi32.dll!RegOpenKeyExA needs W32A-9 (not yet implemented)
 *     w32: TODO advapi32.dll!RegCloseKey needs W32A-9 (not yet implemented)
 *     w32unsup: stub failed with 50, exiting
 *
 * (W32A-9 replaces the TODO stubs with their FAIL-CLEAN finals and will
 * move this example again; the gate pins today's contract, not the
 * future's.)
 */

#include <windows.h>

static HANDLE w32unsup_out;

static void w32unsup_puts(const char *s) {
    DWORD w = 0;
    DWORD n = 0;
    while (s[n] != 0)
        n++;
    WriteFile(w32unsup_out, s, n, &w, NULL);
}

static void w32unsup_putu32(unsigned int v) {
    char b[11];
    int i = 10;
    b[10] = 0;
    if (v == 0)
        b[--i] = '0';
    while (v != 0 && i > 0) {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    }
    w32unsup_puts(b + i);
}

void __stdcall winstart(void) {
    DWORD err;
    HKEY key;

    w32unsup_out = GetStdHandle(STD_OUTPUT_HANDLE);
    w32unsup_puts("w32unsup: running with stubbed ADVAPI32\r\n");

    /* Both calls land on loud TODO stubs (stub_map.tsv, phase-owned by
     * W32A-9): each logs once, sets ERROR_NOT_SUPPORTED, and returns. */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE", 0, KEY_READ, &key) == 0)
        RegCloseKey(key);
    err = GetLastError();

    w32unsup_puts("w32unsup: stub failed with ");
    w32unsup_putu32((unsigned int)err);
    w32unsup_puts(", exiting\r\n");

    ExitProcess(0);
}

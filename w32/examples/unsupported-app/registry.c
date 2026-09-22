/* w32/examples/unsupported-app/registry.c — the example that used to run
 * on stubs, and now runs on the real thing.
 *
 * WIN32_PLAN.md phase W32-8 birthed this example to demonstrate the
 * loud-stub failure mode. W32A-9 shipped the registry, the TODO stubs
 * for every ADVAPI32 ledger import retired, and the example moved with
 * its phase: where it used to prove WHERE a missing import fails, it
 * now proves the registry is real end to end through compiler-emitted
 * code — open the seeded key, read ProductName back, close.
 *
 * Expected output on AuraLite:
 *
 *     w32run: /tests/w32unsup.exe mapped at 0x400000000000, N import(s) bound
 *     w32unsup: the registry is real (W32A-9)
 *     w32unsup: ProductName = AuraLite OS (w32 personality)
 *     '/apps/w32run' (tid N) exited (code=0)
 *
 * (The import count is the compiler's business — the gate asserts the
 * bound receipt generically. The name "w32unsup" and this directory
 * stay: the history is the point. When W32A-10+ retires the next stub
 * module, the loud-stub demo lives in the A-1 fixtures, not here.)
 *
 * Apache-2.0; mingw-w64's public-domain <windows.h> declarations, none
 * of its runtime.
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

void __stdcall winstart(void) {
    HKEY key;
    char buf[128];
    DWORD n = sizeof(buf) - 1;

    w32unsup_out = GetStdHandle(STD_OUTPUT_HANDLE);
    w32unsup_puts("w32unsup: the registry is real (W32A-9)\r\n");

    /* The seeded key answers through the real A-variant engine. */
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\AuraLite\\CurrentVersion",
                      0, KEY_READ, &key) == 0) {
        if (RegQueryValueExA(key, "ProductName", NULL, NULL,
                             (LPBYTE)buf, &n) == 0 && n < sizeof(buf)) {
            buf[n] = 0;
            w32unsup_puts("w32unsup: ProductName = ");
            w32unsup_puts(buf);
            w32unsup_puts("\r\n");
        }
        RegCloseKey(key);
    }

    ExitProcess(0);
}

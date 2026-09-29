/* w32/examples/delay-load-app/delayload.c — runtime-bound imports.
 *
 * W32APP_PLAN.md phase W32A-18, the ladder's delay/runtime-load rung.
 * Apache-2.0; public-domain mingw-w64 <windows.h>, none of mingw-w64's
 * runtime linked (see the Makefile's -nostdlib).
 *
 * USER32 is NOT in this program's import table: it is resolved at run time
 * with LoadLibraryA + GetProcAddress (W32A-1's dynamic path — the same
 * name-resolution the personality drives for PE delay-load descriptors).
 * A true __delayLoadHelper2 delay-load needs mingw's delayimp helper, which
 * pulls the CRT and so is out of scope for these -nostdlib examples; the
 * runtime-bind path shown here exercises the same loader machinery.
 *
 *     make                       # needs x86_64-w64-mingw32-gcc
 *     run delayload.exe          # on AuraLite
 */

#include <windows.h>

typedef int (__stdcall *MessageBoxW_t)(HWND, LPCWSTR, LPCWSTR, UINT);

static void put(HANDLE out, const char *s) {
    DWORD w = 0, n = 0;
    while (s[n]) n++;
    WriteFile(out, s, n, &w, NULL);
}

void __stdcall winstart(void) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

    HMODULE u32 = LoadLibraryA("user32.dll");
    if (!u32) {
        put(out, "LoadLibraryA(user32.dll) failed\r\n");
        ExitProcess(1);
    }
    put(out, "user32.dll loaded at runtime\r\n");

    MessageBoxW_t mbox = (MessageBoxW_t)GetProcAddress(u32, "MessageBoxW");
    if (!mbox) {
        put(out, "GetProcAddress(MessageBoxW) failed\r\n");
        ExitProcess(2);
    }
    put(out, "MessageBoxW resolved; calling it\r\n");
    mbox(NULL, L"Bound at run time via LoadLibrary + GetProcAddress.",
         L"delay-load example", MB_OK);

    FreeLibrary(u32);
    ExitProcess(0);
}

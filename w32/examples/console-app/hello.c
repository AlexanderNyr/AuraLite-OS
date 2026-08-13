/* w32/examples/console-app/hello.c — a console Win32 program for AuraLite.
 *
 * WIN32_PLAN.md phase W32-8.  Apache-2.0, like the rest of AuraLite; it
 * uses mingw-w64's public-domain <windows.h> declarations and links none
 * of mingw-w64's runtime (see the Makefile's -nostdlib).
 *
 * Built with mingw-w64 on the host and run on AuraLite unchanged:
 *
 *     make                       # needs x86_64-w64-mingw32-gcc
 *     cp hello.exe /path/to/initrd/
 *     run hello.exe              # on AuraLite
 *
 * Everything here is ordinary Win32 -- there is no AuraLite-specific header,
 * no #ifdef, and nothing to port.  That is the point of a personality: the
 * program does not know where it is running.
 */

#include <windows.h>

/* The entry point is named directly rather than relying on mainCRTStartup:
 * mingw-w64's CRT is deliberately NOT linked (see the Makefile), so nothing
 * would define it.  The personality's own startup ran before this is
 * reached -- TLS callbacks and .CRT$XC* initialisers (W32-6). */
void __stdcall winstart(void) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;

    static const char msg[] =
        "Hello from a Win32 console program on AuraLite OS.\r\n";
    WriteFile(out, msg, (DWORD)(sizeof msg - 1), &written, NULL);

    /* The command line arrives as ONE STRING; splitting it is the program's
     * job on Windows.  GetCommandLineA returns what was passed. */
    const char *cmdline = GetCommandLineA();
    WriteFile(out, "command line: ", 14, &written, NULL);
    DWORD n = 0;
    while (cmdline[n]) n++;
    WriteFile(out, cmdline, n, &written, NULL);
    WriteFile(out, "\r\n", 2, &written, NULL);

    /* A heap allocation, to show KERNEL32's heap is real. */
    void *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1024);
    if (p) {
        WriteFile(out, "HeapAlloc worked.\r\n", 19, &written, NULL);
        HeapFree(GetProcessHeap(), 0, p);
    }

    ExitProcess(0);
}

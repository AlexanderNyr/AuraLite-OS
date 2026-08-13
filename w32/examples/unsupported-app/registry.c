/* w32/examples/unsupported-app/registry.c — a program AuraLite will NOT run.
 *
 * WIN32_PLAN.md phase W32-8.  Apache-2.0; mingw-w64's public-domain
 * <windows.h> declarations, none of its runtime.
 *
 * This example exists to demonstrate the failure mode, which is as much a
 * part of a personality's contract as the things that work.
 *
 * It uses the registry (ADVAPI32), which is an explicit non-goal --
 * decision D8 in WIN32_PLAN.md.  The point is WHERE and HOW it fails:
 *
 *   - not at the first call, with a wild jump through an unwritten import
 *     slot, halfway through doing something;
 *   - but at LOAD time, before a single instruction of the program runs,
 *     naming the DLL and the function that could not be resolved.
 *
 * Expected output on AuraLite:
 *
 *     w32run: unresolved import ADVAPI32.dll!RegOpenKeyExA
 *
 * A loader that bound what it could and left the rest zeroed would run this
 * program until it happened to touch the registry, then crash somewhere with
 * no explanation.  Failing early and by name is the whole reason import
 * binding treats an unknown symbol as fatal (see w32/src/w32_bind.c).
 */

#include <windows.h>

void __stdcall winstart(void) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;

    /* This line is never reached on AuraLite: the failure happens at load
     * time, not here. */
    WriteFile(out, "this should not print\r\n", 23, &written, NULL);

    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE", 0, KEY_READ, &key) == 0)
        RegCloseKey(key);

    ExitProcess(0);
}

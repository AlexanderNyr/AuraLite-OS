/* userspace/apps/w32run/dlltest.c — WIN32_PLAN.md phase W32-7 gate.
 *
 * Drives LoadLibrary / GetProcAddress / FreeLibrary against both kinds of
 * module: the built-ins, and a real DLL file loaded from the initrd.
 *
 * Native rather than a PE for the same reason sehtest.c is: it isolates the
 * module layer, so a failure here is the loader's and not the PE entry path's.
 *
 * Exit 88 on success.
 */

#include <stdio.h>
#include <string.h>

#include "w32/w32_module.h"
#include "w32/w32_errno.h"
#include "w32/kernel32.h"

static int failures;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

int main(void) {
    w32_module_init();

    /* ---- built-in modules ------------------------------------------- */

    W32_HMODULE k32 = w32_GetModuleHandleA("kernel32");
    check(k32 != NULL, "GetModuleHandleA(kernel32)");

    /* The spellings a real program uses interchangeably must all work. */
    check(w32_GetModuleHandleA("KERNEL32.dll") == k32,
          "KERNEL32.dll is the same module as kernel32");
    check(w32_GetModuleHandleA("KeRnEl32") == k32,
          "module names are case-insensitive");

    check(w32_GetModuleHandleA("nosuchmodule") == NULL,
          "an unknown module returns NULL");

    /* The gate's headline case. */
    void *wf = w32_GetProcAddress(k32, "WriteFile");
    check(wf != NULL, "GetProcAddress(kernel32, WriteFile)");

    if (wf) {
        /* Calling through the returned pointer is the real test: a table
         * lookup that returns a plausible address proves nothing. */
        typedef W32_BOOL (W32ABI *writefile_fn)(W32_HANDLE, const void *,
                                                W32_DWORD, W32_DWORD *, void *);
        writefile_fn f = (writefile_fn)wf;
        static const char msg[] = "DLL-CALLED-THROUGH-GETPROCADDRESS\n";
        W32_DWORD wrote = 0;
        W32_HANDLE h = GetStdHandle((W32_DWORD)-11);
        W32_BOOL ok = f(h, msg, (W32_DWORD)(sizeof msg - 1), &wrote, NULL);
        check(ok && wrote == sizeof msg - 1, "the returned pointer is callable");
    }

    /* A missing export: NULL and ERROR_PROC_NOT_FOUND, never a crash. */
    w32_set_last_error(0);
    check(w32_GetProcAddress(k32, "NoSuchExport") == NULL,
          "a missing export returns NULL");
    check(GetLastError() == W32_ERROR_PROC_NOT_FOUND,
          "and sets ERROR_PROC_NOT_FOUND");

    /* A fabricated handle must be rejected rather than dereferenced.  This
     * is why HMODULEs are minted from a table instead of being mapped
     * addresses. */
    check(w32_GetProcAddress((W32_HMODULE)(unsigned long)0x41414141,
                             "WriteFile") == NULL,
          "a fabricated HMODULE is refused");
    check(w32_GetProcAddress(NULL, "WriteFile") == NULL,
          "a NULL HMODULE is refused");

    /* ---- a real DLL from disk ---------------------------------------- */

    int before = w32_module_count();

    W32_HMODULE dll = w32_LoadLibraryA("/tests/testdll.dll");
    check(dll != NULL, "LoadLibraryA of a real DLL");
    if (!dll) {
        printf("W32-DLL-FAIL %d\n", failures + 1);
        return 1;
    }

    /* DllMain must have run at load time, before any export is called. */
    typedef int (W32ABI *intfn)(void);
    intfn was_attached = (intfn)w32_GetProcAddress(dll, "dll_was_attached");
    check(was_attached != NULL, "GetProcAddress(dll_was_attached)");
    if (was_attached)
        check(was_attached() == 1, "DllMain ran before any export was called");

    /* A leaf export: proves the relocation and mapping arithmetic. */
    typedef int (W32ABI *addfn)(int, int);
    addfn add = (addfn)w32_GetProcAddress(dll, "dll_add");
    check(add != NULL, "GetProcAddress(dll_add)");
    if (add) check(add(40, 2) == 42, "calling into the DLL returns 42");

    /* An export that calls back out through the DLL's OWN import table.
     * A loader that mapped the DLL but skipped its imports passes every
     * test above and fails this one. */
    intfn speak = (intfn)w32_GetProcAddress(dll, "dll_speak");
    check(speak != NULL, "GetProcAddress(dll_speak)");
    if (speak) check(speak() == 1, "the DLL's own imports were bound");

    check(w32_GetProcAddress(dll, "NotExported") == NULL,
          "a missing export in a real DLL returns NULL");

    /* Loading the same path twice must share one mapping, not load it
     * again -- otherwise a program gets two copies of the DLL's state. */
    W32_HMODULE again = w32_LoadLibraryA("/tests/testdll.dll");
    check(again == dll, "loading the same DLL twice returns the same handle");
    check(w32_module_refcount(dll) == 2, "and takes a second reference");
    check(w32_module_count() == before + 1, "without adding a second module");

    check(w32_FreeLibrary(again) == 1, "FreeLibrary drops a reference");
    check(w32_module_refcount(dll) == 1, "one reference remains");
    check(add(1, 1) == 2, "the DLL still works after the first FreeLibrary");

    check(w32_FreeLibrary(dll) == 1, "the last FreeLibrary succeeds");
    check(w32_module_count() == before, "and the module is gone");

    /* Freeing a built-in is a no-op that reports success: a well-behaved
     * program's cleanup path should not look like a failure. */
    check(w32_FreeLibrary(k32) == 1, "FreeLibrary on a built-in succeeds");
    check(w32_GetProcAddress(k32, "WriteFile") != NULL,
          "and the built-in is still usable");

    /* ---- refusals ----------------------------------------------------- */

    w32_set_last_error(0);
    check(w32_LoadLibraryA("/tests/nosuch.dll") == NULL,
          "loading a missing file fails");
    check(GetLastError() == W32_ERROR_MOD_NOT_FOUND,
          "and reports ERROR_MOD_NOT_FOUND");

    /* An .exe is not a DLL.  Loading one would run its entry point under
     * DllMain's contract, which it does not follow. */
    check(w32_LoadLibraryA("/tests/k32test.exe") == NULL,
          "loading an .exe as a library is refused");

    check(w32_LoadLibraryA("") == NULL, "an empty path is refused");
    check(w32_LoadLibraryA(NULL) == NULL, "a NULL path is refused");

    /* A forwarder export ("KERNEL32.Sleep" as a string where code should
     * be).  Returning its address would hand the caller a pointer to text
     * that they would then call, so it is refused by name. */
    W32_HMODULE fwd = w32_LoadLibraryA("/tests/fwddll.dll");
    check(fwd != NULL, "a DLL containing a forwarder still loads");
    if (fwd) {
        w32_set_last_error(0);
        check(w32_GetProcAddress(fwd, "dll_add") == NULL,
              "a forwarder export is refused, not returned");
        check(GetLastError() == W32_ERROR_PROC_NOT_FOUND,
              "and reports ERROR_PROC_NOT_FOUND");
        /* Its non-forwarder exports must still work: the refusal is per
         * symbol, not per module. */
        intfn ok2 = (intfn)w32_GetProcAddress(fwd, "dll_was_attached");
        check(ok2 != NULL, "ordinary exports of the same DLL still resolve");
        w32_FreeLibrary(fwd);
    }

    /* A DLL whose DllMain returns FALSE has refused to initialise.  The
     * load must fail AND the mapping must be gone -- a leaked module here
     * is the "leaking the address space" the plan's gate warns about. */
    int before_bad = w32_module_count();
    check(w32_LoadLibraryA("/tests/baddll.dll") == NULL,
          "a DLL whose DllMain fails does not load");
    check(w32_module_count() == before_bad,
          "and leaves no module behind");

    /* Load and free repeatedly: the module table must not leak slots. */
    for (int i = 0; i < 8; i++) {
        W32_HMODULE m = w32_LoadLibraryA("/tests/testdll.dll");
        if (!m) { check(0, "repeated load succeeded"); break; }
        w32_FreeLibrary(m);
    }
    check(w32_module_count() == before,
          "eight load/free cycles leak no module slots");

    if (failures == 0) {
        printf("W32-DLL-OK\n");
        return 88;
    }
    printf("W32-DLL-FAIL %d\n", failures);
    return 1;
}

#!/usr/bin/env bash
# test_w32_loadlibrary.sh — WIN32_PLAN.md phase W32-7 gate.
#
# The phase asked whether dynamic loading is reachable and allowed the answer
# to be a documented refusal. It is reachable: mmap grants PROT_EXEC and the
# export directory parses, so real user-supplied DLLs load rather than only
# the built-in modules.
#
# The plan's four gate items:
#
#   1. GetProcAddress(GetModuleHandle("kernel32"), "WriteFile") returns the
#      implementation and calling through it works;
#   2. a missing export returns NULL + ERROR_PROC_NOT_FOUND, never a crash;
#   3. since real DLL loading landed: a DLL whose DllMain fails is handled
#      without leaking the address space;
#   4. refusals (forwarders, non-DLLs) are explicit.
#
# The in-guest program checks each case itself and reaches exit 88 only if
# every one passed, so a partial failure is distinguishable from a crash.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "W32-7 LoadLibrary and GetProcAddress"

LOG="$IL_LOGDIR/w32_loadlibrary.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 10
il_send "run /apps/dlltest"
il_send_delay 8
il_send "exit"

il_run_qemu "$LOG" 90

# --- 1. the built-in modules ----------------------------------------------
il_assert_grep "$LOG" "ok   GetModuleHandleA\(kernel32\)" \
    "GetModuleHandleA finds a built-in module"
il_assert_grep "$LOG" "ok   KERNEL32.dll is the same module as kernel32" \
    "the .dll suffix and the bare name are one module"
il_assert_grep "$LOG" "ok   GetProcAddress\(kernel32, WriteFile\)" \
    "GetProcAddress returns the implementation"
# A table lookup returning a plausible address proves nothing; the pointer is
# actually called, and WriteFile's own output is the evidence.
il_assert_grep "$LOG" "DLL-CALLED-THROUGH-GETPROCADDRESS" \
    "and calling through that pointer works"

# --- 2. missing exports and bad handles -----------------------------------
il_assert_grep "$LOG" "ok   a missing export returns NULL" \
    "a missing export returns NULL"
il_assert_grep "$LOG" "ok   and sets ERROR_PROC_NOT_FOUND" \
    "and sets ERROR_PROC_NOT_FOUND"
il_assert_grep "$LOG" "ok   a fabricated HMODULE is refused" \
    "a fabricated HMODULE is refused rather than dereferenced"

# --- 3. a real DLL from disk ----------------------------------------------
il_assert_grep "$LOG" "DLL-ATTACH" \
    "DllMain(DLL_PROCESS_ATTACH) ran at load time"
il_assert_grep "$LOG" "ok   DllMain ran before any export was called" \
    "and ran before any export was called"
il_assert_grep "$LOG" "ok   calling into the DLL returns 42" \
    "a function in the loaded DLL is callable"
# The DLL calls WriteFile through its OWN import table: a loader that mapped
# the DLL but skipped its imports would pass every check above and fail here.
il_assert_grep "$LOG" "DLL-SPEAK" \
    "the DLL's own imports were bound, not just the main image's"
il_assert_grep "$LOG" "ok   loading the same DLL twice returns the same handle" \
    "a second load shares one mapping instead of loading it twice"
il_assert_grep "$LOG" "ok   the last FreeLibrary succeeds" \
    "FreeLibrary releases the module on the last reference"
il_assert_grep "$LOG" "DLL-DETACH" \
    "and DllMain(DLL_PROCESS_DETACH) ran on the way out"
il_assert_grep "$LOG" "ok   eight load/free cycles leak no module slots" \
    "repeated load/free leaks no module slots"

# --- 4. explicit refusals -------------------------------------------------
il_assert_grep "$LOG" "forwarder exports are not supported" \
    "a forwarder export is refused by name"
il_assert_grep "$LOG" "ok   ordinary exports of the same DLL still resolve" \
    "the refusal is per symbol, not per module"
il_assert_grep "$LOG" "DllMain returned FALSE" \
    "a DLL whose DllMain fails is refused"
il_assert_grep "$LOG" "ok   and leaves no module behind" \
    "and its mapping is torn down, not leaked"
il_assert_grep "$LOG" "not a DLL \(IMAGE_FILE_DLL is clear\)" \
    "loading an .exe as a library is refused"

# --- overall --------------------------------------------------------------
il_assert_grep "$LOG" "W32-DLL-OK" \
    "every module-layer check passed"
il_assert_grep "$LOG" "'/apps/dlltest' \(tid [0-9]+\) exited \(code=88\)" \
    "the fixture exited with its success status"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION.*KERNEL|kernel panic" \
    "no kernel fault from any of the above"
il_assert_grep "$LOG" "Goodbye!" \
    "shell survived and exited cleanly"

il_summary

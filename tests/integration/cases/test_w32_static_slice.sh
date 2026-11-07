#!/usr/bin/env bash
# test_w32_static_slice.sh — RESIDUE3_PLAN.md phase C8, box 2 (RES-57).
#
# The five W32 app ledgers carry 1694 `static-only` import markers.  The ledger
# says what that means: the personality's export tables bind the symbol, and no
# pinned application was ever watched reaching it.  835 of the markers are
# distinct DLL/symbol pairs; 24 more are ordinal imports with no name to
# resolve; the rest repeat across apps.
#
# w32c8_static_slice.exe is generated from those ledgers and the two export
# tables (tools/w32_static_slice.py), so the guest cannot be checking a
# different list than the host printed.  It:
#
#   1. LoadLibraryA's every DLL the markers name (20 of them),
#   2. GetProcAddress's all 820 named pairs, printing
#      `[w32] STATIC-ONLY <dll>: <have>/<total> resolved`,
#   3. names every pair that comes back NULL,
#   4. calls a 9-stub slice plus 4 real implementations with real prototypes.
#
# What it found on the first run, which is why the box exists:
#   * `LoadLibraryA("imm32.dll")`, `("ole32.dll")` and `("uxtheme.dll")` all
#     answered NULL: three modules the personality implements were exported in
#     the static tables but never registered as loadable, so IAT binding worked
#     while the classic LoadLibrary + GetProcAddress shape failed.  Fixed in
#     w32/src/w32_module.c.
#   * a naive "call every argument-free stub" slice must exclude msvcrt's CRT
#     machinery and the stubs shadowed by real implementations -- measured
#     twice, as a fault at RIP 0x4005a09d and one at 0x40007bf8.  The call
#     slice is now restricted to stubs nothing shadows.
#
# The gate asserts the whole table, not a sample: 820/820 over 20 modules,
# every per-DLL line, 9/9 stub calls, 4/4 real calls, exit 78.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "RESIDUE3 C8: static-only receipts (RES-57: 1694 markers, 820 pairs)"

LOG="$IL_LOGDIR/w32_c8_static.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

if ! tar tf "$IL_BUILD/initrd.tar" 2>/dev/null | grep -q w32c8_static_slice; then
    echo "  SKIP: the C8 static-only fixture is not in the image (no cross-compiler)"
    il_summary
    exit 0
fi

# The guest table and the ledger must agree before QEMU even starts: the
# generated header is the same list the fixture walks.
if [ -f "$IL_BUILD/w32_static_slice.h" ]; then
    il_assert_grep "$IL_BUILD/w32_static_slice.h" "^#define K_SO_MARKERS 1694$" \
        "the generated table covers all 1694 static-only markers"
    il_assert_grep "$IL_BUILD/w32_static_slice.h" "^#define K_SO_TOTAL 820$" \
        "820 distinct named pairs"
    il_assert_grep "$IL_BUILD/w32_static_slice.h" "^#define K_SO_ORDINALS 24$" \
        "24 ordinal markers carry no name to resolve"
    il_assert_grep "$IL_BUILD/w32_static_slice.h" "^#define K_SO_MISSING 0$" \
        "no marker names a symbol the export tables lack"
fi

il_send_delay 8
il_send "run /tests/w32c8_static_slice.exe"
il_send_delay 14
il_send "exit"

il_run_qemu "$LOG" 120

# --- the fixture itself ----------------------------------------------------
il_assert_grep "$LOG" "C8-STATIC-OK" \
    "every pair resolved as the generated table says it would"
il_assert_grep "$LOG" "'/tests/w32c8_static_slice\\.exe' \\(tid [0-9]+\\) exited \\(code=78\\)" \
    "w32c8_static_slice.exe exited 78"
il_assert_no_grep "$LOG" "C8-STATIC-FAIL" \
    "no disagreement with the generated table"
il_assert_no_grep "$LOG" "STATIC-ONLY MISSING" \
    "not one of the 820 pairs came back NULL"
il_assert_no_grep "$LOG" "Page Fault|W32-SEH-UNHANDLED|kernel panic" \
    "no fault while resolving and calling"

# --- all 20 modules load ---------------------------------------------------
il_assert_grep "$LOG" "\\[w32\\] STATIC-ONLY modules: 20/20 loaded" \
    "all 20 DLLs the markers name load by name"

# --- every DLL's resolution line -------------------------------------------
il_assert_grep "$LOG" "STATIC-ONLY advapi32.dll: 45/45 resolved via GetProcAddress" "advapi32 45/45"
il_assert_grep "$LOG" "STATIC-ONLY comctl32.dll: 21/21 resolved via GetProcAddress" "comctl32 21/21"
il_assert_grep "$LOG" "STATIC-ONLY comdlg32.dll: 9/9 resolved via GetProcAddress" "comdlg32 9/9"
il_assert_grep "$LOG" "STATIC-ONLY crypt32.dll: 8/8 resolved via GetProcAddress" "crypt32 8/8"
il_assert_grep "$LOG" "STATIC-ONLY dbghelp.dll: 1/1 resolved via GetProcAddress" "dbghelp 1/1"
il_assert_grep "$LOG" "STATIC-ONLY dwmapi.dll: 2/2 resolved via GetProcAddress" "dwmapi 2/2"
il_assert_grep "$LOG" "STATIC-ONLY gdi32.dll: 87/87 resolved via GetProcAddress" "gdi32 87/87"
il_assert_grep "$LOG" "STATIC-ONLY imm32.dll: 10/10 resolved via GetProcAddress" "imm32 10/10"
il_assert_grep "$LOG" "STATIC-ONLY kernel32.dll: 267/267 resolved via GetProcAddress" "kernel32 267/267"
il_assert_grep "$LOG" "STATIC-ONLY mpr.dll: 6/6 resolved via GetProcAddress" "mpr 6/6"
il_assert_grep "$LOG" "STATIC-ONLY msvcrt.dll: 37/37 resolved via GetProcAddress" "msvcrt 37/37"
il_assert_grep "$LOG" "STATIC-ONLY ole32.dll: 12/12 resolved via GetProcAddress" "ole32 12/12"
il_assert_grep "$LOG" "STATIC-ONLY sensapi.dll: 2/2 resolved via GetProcAddress" "sensapi 2/2"
il_assert_grep "$LOG" "STATIC-ONLY shell32.dll: 18/18 resolved via GetProcAddress" "shell32 18/18"
il_assert_grep "$LOG" "STATIC-ONLY shlwapi.dll: 18/18 resolved via GetProcAddress" "shlwapi 18/18"
il_assert_grep "$LOG" "STATIC-ONLY user32.dll: 257/257 resolved via GetProcAddress" "user32 257/257"
il_assert_grep "$LOG" "STATIC-ONLY uxtheme.dll: 15/15 resolved via GetProcAddress" "uxtheme 15/15"
il_assert_grep "$LOG" "STATIC-ONLY version.dll: 3/3 resolved via GetProcAddress" "version 3/3"
il_assert_grep "$LOG" "STATIC-ONLY wininet.dll: 1/1 resolved via GetProcAddress" "wininet 1/1"
il_assert_grep "$LOG" "STATIC-ONLY wintrust.dll: 1/1 resolved via GetProcAddress" "wintrust 1/1"

il_assert_grep "$LOG" "STATIC-ONLY 820/820 resolved over 20 modules" \
    "820/820 resolved, with the 24 ordinal markers accounted for"

# --- calls, not merely bindings --------------------------------------------
il_assert_grep "$LOG" "STATIC-ONLY calls: 9/9 made" \
    "9 argument-free stubs were called, not merely bound"
il_assert_grep "$LOG" "STATIC-ONLY call mpr.dll!WNetAddConnection2W -> 0x00000000000004c6" \
    "mpr!WNetAddConnection2W answers ERROR_NO_NETWORK when called"
il_assert_grep "$LOG" "STATIC-ONLY call mpr.dll!WNetCloseEnum -> 0x0000000000000006" \
    "mpr!WNetCloseEnum answers ERROR_INVALID_HANDLE"
il_assert_grep "$LOG" "STATIC-ONLY real calls: 4/4" \
    "4 real implementations called through hand-written prototypes"
il_assert_grep "$LOG" "STATIC-ONLY call user32.dll!GetSystemMetrics\\(0\\) -> " \
    "user32!GetSystemMetrics answered"
il_assert_grep "$LOG" "STATIC-ONLY call kernel32.dll!GetVersion\\(\\) -> 0x" \
    "kernel32!GetVersion answered"
il_assert_grep "$LOG" "STATIC-ONLY call kernel32.dll!GetUserDefaultLCID\\(\\) -> " \
    "kernel32!GetUserDefaultLCID answered"

il_summary

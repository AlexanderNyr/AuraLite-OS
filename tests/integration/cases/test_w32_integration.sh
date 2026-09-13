#!/usr/bin/env bash
# test_w32_integration.sh — WIN32_PLAN.md phase W32-8 gate.
#
# The end-to-end claim of the whole plan: a Win32 program built by a real
# compiler runs on AuraLite unmodified.
#
# Every earlier phase tested a mechanism with a hand-written fixture, which
# was the right way to isolate faults but always left one question open --
# whether a binary a normal toolchain emits, with the section layout, the
# import thunks and the code generation that come with it, actually works.
# This case answers that with mingw-w64 output.
#
# It also checks the two routing behaviours and the documented stub
# behaviour (since W32A-1 the loader binds unimplemented imports to
# loud phase-owned stubs instead of refusing at load time).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32-8 integration: real mingw-w64 binaries"

LOG="$IL_LOGDIR/w32_integration.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# The mingw-w64 examples are only in the image when the cross-compiler was
# installed at build time.  Skipping loudly beats asserting on a file that
# was never built.
if ! tar tf "$IL_BUILD/initrd.tar" 2>/dev/null | grep -q w32hello; then
    echo "  SKIP: mingw-w64 examples not in the image (no cross-compiler)"
    il_summary
    exit 0
fi

il_send_delay 10
il_send "run /tests/w32hello.exe"
il_send_delay 6
il_send "run /tests/w32unsup.exe"
il_send_delay 5
il_send "run /tests/petest.exe"
il_send_delay 5
il_send "exit"

il_run_qemu "$LOG" 100

# --- the shell routes a PE with imports through w32run ---------------------
il_assert_grep "$LOG" "imports Win32 DLLs; running via /apps/w32run" \
    "the shell detects a PE with imports and routes it"
il_assert_grep "$LOG" "w32run: /tests/w32hello\.exe .* 7 import\(s\) bound" \
    "all seven of the compiler's imports bound"

# --- a real compiler-emitted binary runs -----------------------------------
il_assert_grep "$LOG" "Hello from a Win32 console program on AuraLite OS" \
    "a mingw-w64-built .exe runs unmodified"
il_assert_grep "$LOG" "command line: /tests/w32hello\.exe" \
    "GetCommandLineA reported the command line"
il_assert_grep "$LOG" "HeapAlloc worked" \
    "the KERNEL32 heap works for compiler-emitted code"
il_assert_grep "$LOG" "'/apps/w32run' \(tid [0-9]+\) exited \(code=0\)" \
    "and it exited cleanly through ExitProcess"

# --- the documented stub ------------------------------------------------------
# The unsupported example imports ADVAPI32 (the registry -- a non-goal, D8).
# Since W32A-1 such imports bind to loud phase-owned stubs: the program
# runs until the first missing import, which fails loudly with the owning
# phase named and a guest-visible error -- not a crash, not a silence.
il_assert_grep "$LOG" "w32run: /tests/w32unsup\\.exe .* 5 import\\(s\\) bound" \
    "all five imports bound, stubbed ones included"
il_assert_grep "$LOG" "w32: TODO advapi32\\.dll!RegOpenKeyExA needs W32A-9" \
    "the missing import fails loudly with its owning phase named"
il_assert_grep "$LOG" "w32: TODO advapi32\\.dll!RegCloseKey needs W32A-9" \
    "every touched stub reports, not just the first"
il_assert_grep "$LOG" "w32unsup: running with stubbed ADVAPI32" \
    "the program itself starts and runs"
il_assert_grep "$LOG" "w32unsup: stub failed with 50, exiting" \
    "and the stub's failure is guest-visible, not silent"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION.*KERNEL|kernel panic" \
    "the stub path is clean, not a fault"

# --- an import-free PE still takes the kernel path -------------------------
# That path applies per-section W^X, so it must not be given up for the
# convenience of routing everything through w32run.
il_assert_grep "$LOG" "\[pe\] +loaded 4 section\(s\)" \
    "a PE without imports is still loaded by the kernel directly"
il_assert_grep "$LOG" "'/tests/petest\.exe' \(tid [0-9]+\) exited \(code=77\)" \
    "and runs correctly"

il_assert_grep "$LOG" "Goodbye!" \
    "shell survived all three runs and exited cleanly"

il_summary

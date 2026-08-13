#!/usr/bin/env bash
# test_w32_kernel32.sh — WIN32_PLAN.md phase W32-4 gate.
#
# Proves, in a booted system, that a PE32+ .exe importing KERNEL32 through a
# real import table has those imports bound and calls them successfully across
# the Windows-x64 -> System V boundary.
#
# The guest binary asserts its own results and exits 55 only if every check
# passed; a failure exits 1.  The markers below are printed at each step so a
# partial failure says which step, rather than only "wrong exit code".
#
# Both the .exe and its import library are produced in-tree by nasm -f win64
# and lld-link (already in REQUIRED_TOOLS).  No Microsoft DLL is involved.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32-4 KERNEL32 imports"

LOG="$IL_LOGDIR/w32_kernel32.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 10
il_send "run /apps/w32run /tests/k32test.exe"
il_send_delay 5
il_send "exit"

il_run_qemu "$LOG" 70

# --- binding ---------------------------------------------------------------
il_assert_grep "$LOG" "w32run: .*k32test\.exe mapped at 0x[0-9a-f]+, 10 import\(s\) bound" \
    "all ten KERNEL32 imports resolved and written to the IAT"

# --- the calls themselves --------------------------------------------------
# GetStdHandle + WriteFile.  This is also the ms_abi boundary working: the
# arguments arrived in RCX/RDX/R8/R9 and were read from RDI/RSI/... correctly.
il_assert_count "$LOG" "W32-KERNEL32-OK" 2 \
    "WriteFile through an import produced output (twice)"

# A handle the program never minted must be refused with the documented code,
# not accepted and not crashed.
il_assert_grep "$LOG" "BADHANDLE-REFUSED" \
    "bad HANDLE rejected with ERROR_INVALID_HANDLE"

# GetProcessHeap/HeapAlloc/HeapFree round trip, including a write to the block.
il_assert_grep "$LOG" "HEAP-OK" \
    "HeapAlloc returned usable memory and HeapFree accepted it"

# GetTickCount64 returned a plausible non-zero value.
il_assert_grep "$LOG" "TICK-OK" \
    "GetTickCount64 returned a non-zero tick count"

# --- exit status -----------------------------------------------------------
# 55 is reached only after every in-guest assertion passed; 1 is its failure
# path.  Checking the value distinguishes "all checks passed" from "printed
# some markers then died".
il_assert_grep "$LOG" "'/apps/w32run' \(tid [0-9]+\) exited \(code=55\)" \
    "guest program reported success through its exit status"
il_assert_no_grep "$LOG" "exited \(code=1\)" \
    "no in-guest assertion failed"

# --- nothing broke ---------------------------------------------------------
il_assert_no_grep "$LOG" "unresolved import" \
    "no import was left unbound"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION|kernel panic|Page Fault" \
    "no kernel fault"
il_assert_grep "$LOG" "Goodbye!" \
    "shell survived and exited cleanly"

il_summary

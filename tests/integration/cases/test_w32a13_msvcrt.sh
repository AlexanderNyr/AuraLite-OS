#!/usr/bin/env bash
# test_w32a13_msvcrt.sh — W32APP_PLAN.md phase W32A-13 gate.
#
# One mingw-w64 fixture (w32a13_msvcrt.exe) linked -lmsvcrt, so its msvcrt
# imports are genuine NAME imports resolved through w32_bind.c.  It runs the
# msvcrt bridge REAL over the personality: HEAP UNITY across two DLLs (a
# msvcrt.dll!malloc block measured by kernel32.dll!HeapSize, and
# kernel32.dll!HeapReAlloc growing a CRT pointer), the string/memory core,
# the seeded MSVCRT rand LCG, _beginthreadex over CreateThread (join + exit
# code), and the _onexit chain — the run ends with msvcrt.dll!exit(78), which
# runs the LIFO onexit callbacks (OX-B then OX-A) and then ExitProcess.
# It prints W32A13-MSVCRT-OK and exits 78 on a clean run, or FAIL-<mark> +
# W32A13-MSVCRT-FAIL / exit 1.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-13 integration: the msvcrt bridge over the native runtimes"

LOG="$IL_LOGDIR/w32a13_msvcrt.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# The fixture is only in the image when the cross-compiler was installed at
# build time.  Skip loudly rather than assert on a file never built.
if ! tar tf "$IL_BUILD/initrd.tar" 2>/dev/null | grep -q w32a13_msvcrt; then
    echo "  SKIP: W32A-13 fixture not in the image (no cross-compiler)"
    il_summary
    exit 0
fi

il_send_delay 8
il_send "run /tests/w32a13_msvcrt.exe"
il_send_delay 18
il_send "exit"

il_run_qemu "$LOG" 90

# --- the fixture passed every in-guest check --------------------------------
il_assert_grep "$LOG" "W32A13-MSVCRT-OK" \
    "the msvcrt fixture passed in-guest"

# --- and exited 78 through msvcrt exit -> ExitProcess -----------------------
il_assert_grep "$LOG" "'/tests/w32a13_msvcrt\\.exe' \\(tid [0-9]+\\) exited \\(code=78\\)" \
    "w32a13_msvcrt.exe exited 78"

# --- the onexit chain ran LIFO on the way out (B before A) ------------------
il_assert_grep "$LOG" "OX-B" "onexit callback ran"

# --- no failed check, no fault ----------------------------------------------
il_assert_no_grep "$LOG" "W32A13-MSVCRT-FAIL" \
    "no fixture check failed"
il_assert_no_grep "$LOG" "FAIL-" \
    "no individual msvcrt assertion failed"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION.*KERNEL|kernel panic|PANIC" \
    "no fault during the run"

il_summary

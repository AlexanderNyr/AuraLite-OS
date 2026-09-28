#!/usr/bin/env bash
# test_w32a14_putty_fixture.sh — W32APP_PLAN.md phase W32A-14 gate (App gate I:
# PuTTY), CI-automatable half.
#
# One mingw-w64 fixture (w32a14_putty.exe) that imports the exact KERNEL32 /
# USER32 / ADVAPI32 surface a real PuTTY session drives -- by NAME, resolved
# through w32_bind.c -- and resolves WS2_32 DYNAMICALLY the way PuTTY does.  It
# walks PuTTY's personality paths and asserts each: the Reg*A session save/load
# round-trip (config store), the W32A-14 REAL console slice (mode/codepage/
# WriteConsoleW/SetStdHandle), the terminal-window verbs W32A-1 had left as loud
# TODO stubs (ShowCursor counter, SetCursor previous, MessageBeep, SetClassLongPtrA
# class-long round-trip), the serial COMM set failing CLEAN (documented non-goal),
# and the dynamic LoadLibrary("WS2_32")+GetProcAddress WinSock path.
#
# It prints W32A14-PUTTY-OK and exits 78 on a clean run, or FAIL-<mark> +
# W32A14-PUTTY-FAIL / exit 1.
#
# The full PuTTY receipt (GUI config-dialog screenshots, live SSH/Telnet/Raw
# against external servers, human-pasted hash-verified run of the pinned
# putty.exe) is human-run and is documented as such in docs/w32app_receipts.md
# under #putty -- it is not headlessly automatable and is out of this gate's
# scope by design.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-14 integration: the PuTTY app gate (fixture twin)"

LOG="$IL_LOGDIR/w32a14_putty.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# The fixture is only in the image when the cross-compiler was installed at
# build time.  Skip loudly rather than assert on a file never built.
if ! tar tf "$IL_BUILD/initrd.tar" 2>/dev/null | grep -q w32a14_putty; then
    echo "  SKIP: W32A-14 fixture not in the image (no cross-compiler)"
    il_summary
    exit 0
fi

il_send_delay 8
il_send "run /tests/w32a14_putty.exe"
il_send_delay 18
il_send "exit"

il_run_qemu "$LOG" 90

# --- the fixture passed every in-guest check --------------------------------
il_assert_grep "$LOG" "W32A14-PUTTY-OK" \
    "the PuTTY app-gate fixture passed in-guest"

# --- and exited 78 ----------------------------------------------------------
il_assert_grep "$LOG" "'/tests/w32a14_putty\\.exe' \\(tid [0-9]+\\) exited \\(code=78\\)" \
    "w32a14_putty.exe exited 78"

# --- no failed check, no fault ----------------------------------------------
il_assert_no_grep "$LOG" "W32A14-PUTTY-FAIL" \
    "no fixture check failed"
il_assert_no_grep "$LOG" "FAIL-" \
    "no individual PuTTY assertion failed"
il_assert_no_grep "$LOG" "TODO .* needs W32A" \
    "no console/cursor/comm import fell through to a TODO stub"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION.*KERNEL|kernel panic|PANIC" \
    "no fault during the run"

il_summary

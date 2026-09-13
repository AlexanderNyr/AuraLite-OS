#!/usr/bin/env bash
# test_w32_a2_kernel32.sh — W32APP_PLAN.md phase W32A-2 gate.
#
# The breadth claim: seven mingw-w64 fixtures, one per W32A-2 group
# (find, time, mapping, pipes, process, locale, heap), each asserting
# REAL behaviour and each refusal by name.  Every fixture prints one
# `[W32A2-<GROUP>] pass` marker on success (or `[W32A2-<GROUP>]
# FAIL:<mark>` lines, one per failed check) and exits 55 on a clean
# run, 1 on any failure.
#
# The message-table rule is enforced by the find fixture's round-trip
# (files the fixture creates are visible to the native shell) and by
# the process fixture's FormatMessage assertions; the host suite pins
# the full error-code/message coverage table.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-2 integration: seven guest kernel32 fixtures"

LOG="$IL_LOGDIR/w32_a2_kernel32.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

# The fixtures are only in the image when the cross-compiler was
# installed at build time.  Skipping loudly beats asserting on files
# that were never built (CI fails on the absence separately).
if ! tar tf "$IL_BUILD/initrd.tar" 2>/dev/null | grep -q w32a2_find; then
    echo "  SKIP: W32A-2 fixtures not in the image (no cross-compiler)"
    il_summary
    exit 0
fi

il_send_delay 8
for f in find time map pipes proc locale heap; do
    il_send "run /tests/w32a2_$f.exe"
    il_send_delay 8
done
# The find fixture plants C:\tmp\w32a2_rt.txt: the native shell must
# see the round-trip file the guest created.
il_send "ls /tmp"
il_send_delay 4
il_send "exit"

il_run_qemu "$LOG" 200

# --- every fixture printed its pass marker ---------------------------------
for g in FIND TIME MAP PIPES PROC LOCALE HEAP; do
    il_assert_grep "$LOG" "\\[W32A2-$g\\] pass" \
        "the $g fixture passed in-guest"
done

# --- and every one exited 55 through ExitProcess -----------------------------
for f in find time map pipes proc locale heap; do
    il_assert_grep "$LOG" "'/tests/w32a2_$f\\.exe' \\(tid [0-9]+\\) exited \\(code=55\\)" \
        "w32a2_$f.exe exited 55"
done

# --- no failure marker anywhere ----------------------------------------------
il_assert_no_grep "$LOG" "W32A2-.*FAIL" \
    "no fixture reported a failed check"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION.*KERNEL|kernel panic" \
    "no faults across the seven runs"

# --- the native round-trip ---------------------------------------------------
il_assert_grep "$LOG" "w32a2_rt\\.txt" \
    "files the guest created are visible to the native shell"

il_assert_grep "$LOG" "Goodbye!" \
    "shell survived all seven runs and exited cleanly"

il_summary

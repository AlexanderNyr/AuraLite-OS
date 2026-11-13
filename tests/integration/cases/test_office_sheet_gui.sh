#!/usr/bin/env bash
# test_office_sheet_gui.sh — OFFICE_PLAN.md OF5 gate: the Sheet grid.
#
# The live GUI lane opens ONE serial connection at a time, so anything
# the app prints while nobody is connected is dropped.  Typing into the
# window therefore happens "silently"; the assertions here are built to
# hold without input-loop receipts, the same way the WR-0 lane asserts:
#
#   1. the window launched (receipt captured during the boot wait);
#   2. ten numbers typed down column B and =SUM(B2:B11) committed
#      (VNC screenshot captured after the commit);
#   3. Ctrl+S wrote the workbook and Esc closed the window;
#   4. `aosheet --eval` on the SAVED FILE re-evaluates every cell from
#      disk and prints the values — this holds on one connection, so
#      the receipt is captured, and Sheet1!B12 = 55 is the proof that
#      the typed cells and the formula really reached the file.
#
# What this lane cannot assert (input-loop receipts), the live VNC run
# does: see live_sheet.py at the repository root (LIVE SHEET: PASS),
# which screenshots the grid, the sum, the chart and the reload.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
. lib/gui_lane.sh

il_section "Office Sheet grid (OF5): type, sum, save, reload"

GL_LOG="$IL_LOGDIR/office_sheet_gui.log"

if gl_boot_bios 'auralite#'; then
    gl_send "run aosheet"
    gl_wait '\[sheet\] gui: workbook ready' 30 || true
    # Cursor starts at A1: one right, one down puts us on B2.
    gl_key right
    gl_key down
    # Arrow keys do not commit an in-progress cell edit; ret commits
    # and steps down one row.
    for v in 1 2 3 4 5 6 7 8 9 10; do
        gl_type "$v"
        gl_key ret
    done
    # Cursor is on B12 now.
    gl_type "=SUM(B2:B11)"
    gl_key ret
    gl_shot "office_sheet_sum" || true
    gl_key ctrl-s
    sleep 2
    gl_key esc
    # The console replays the keystrokes the window consumed; let the
    # replay drain before sending the verification command.
    sleep 6
    gl_send ""
    sleep 1
    gl_cmd "run aosheet --eval /documents/Untitled.aosheet" 'SHEET EVAL: PASS' 60 || true
    gl_shot "office_sheet_eval" || true
    gl_stop
fi

il_assert_grep "$GL_LOG" "\[sheet\] gui: workbook ready" \
    "grid: the window launched"
il_assert_grep "$GL_LOG" "^Sheet1!B2 = 1\r?$" \
    "grid: reload --eval sees the typed column from disk"
il_assert_grep "$GL_LOG" "^Sheet1!B11 = 10\r?$" \
    "grid: reload --eval sees the last typed cell"
il_assert_grep "$GL_LOG" "^Sheet1!B12 = 55\r?$" \
    "grid: =SUM(B2:B11) recomputes to 55 from the saved file"
il_assert_grep "$GL_LOG" "SHEET EVAL: PASS" \
    "grid: the saved workbook re-evaluates cleanly"

il_assert_no_grep "$GL_LOG" "PANIC"               "no kernel panic"
il_assert_no_grep "$GL_LOG" "UNHANDLED EXCEPTION" "no kernel exception"

il_summary

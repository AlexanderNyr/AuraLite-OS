#!/usr/bin/env bash
# test_office_sheet_core.sh — OFFICE_PLAN.md OF4 gate: the Sheet engine.
#
# The engine is gated without a single pixel (the grid is OF5):
#
#   1. `aosheet --eval /lib/demo.aosheet` loads the shipped demo workbook,
#      recalculates it and prints computed values as receipts;
#   2. the same values must come out of the engine's unit tests
#      (tests/unit/test_ao_formula.c) — checked here only as receipts,
#      there as assertions;
#   3. the cyclic sheet reads #CYCLE! — a message, not a hang (the QEMU
#      timeout is the hang detector);
#   4. the arena counters print, and nothing panicked.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Office Sheet engine (OF4): formulas, recalc, cycles, CSV"

LOG="$IL_LOGDIR/office_sheet_core.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run aosheet --eval /lib/demo.aosheet"
il_send_delay 5
il_send "run aosheet --csv /lib/demo.aosheet"
il_send_delay 5
il_send "exit"

il_run_qemu "$LOG" 80

il_assert_grep "$LOG" "Data!A3 = 30" \
    "sheet: SUM over the range computed"
il_assert_grep "$LOG" "Data!A4 = 31.5" \
    "sheet: the dependent cell recalculated (30 * 1.05)"
il_assert_grep "$LOG" "Data!B2 = widgets: 30" \
    "sheet: concatenation built the string"
il_assert_grep "$LOG" "Data!C1 = big" \
    "sheet: IF took the true branch"
il_assert_grep "$LOG" "Data!D1 = 2023-03-15" \
    "sheet: TEXT() formatted the pinned-pattern date"
il_assert_grep "$LOG" "Cycle!A1 = #CYCLE!" \
    "sheet: the self-referential cell is a message, not a hang"
il_assert_grep "$LOG" "Cycle!A2 = #CYCLE!" \
    "sheet: the cycle propagates to its dependent"
il_assert_grep "$LOG" "SHEET EVAL: PASS" \
    "sheet: the eval verdict is green"
il_assert_grep "$LOG" "arena [0-9]+/[0-9]+ bytes" \
    "sheet: allocation is counted, not trusted"
il_assert_grep "$LOG" "widgets: 30" \
    "sheet: CSV export carries the computed values"

il_assert_no_grep "$LOG" "PANIC"               "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no kernel exception"

il_summary

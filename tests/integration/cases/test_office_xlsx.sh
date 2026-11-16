#!/usr/bin/env bash
# test_office_xlsx.sh — OFFICE_PLAN.md OF8 gate: .xlsx in the guest.
#
# `aosheet --xlsx-test` builds a workbook with a number, a formula and a
# Cyrillic string, saves it as /documents/xlsx-gate.xlsx, reads it back, and
# requires the formula to have survived as typed input AND still compute.
#
# The host half of the same claim lives in tests/unit/test_ao_xlsx.c and
# tools/office/check_xlsx.py; this case is the half that proves the bridge is
# linked into the binary a user actually runs, at a size the userspace linker
# accepts — something no host test can see.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Office OOXML (OF8): .xlsx written and read back in the guest"
LOG="$IL_LOGDIR/office_xlsx.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run aosheet --xlsx-test"
il_send_delay 6
il_send "ls /documents"
il_send_delay 3
il_send "exit"
il_run_qemu "$LOG" 120

il_assert_grep "$LOG" "xlsx source: [0-9]+ cells" \
    "xlsx: the source workbook was built in the guest"
il_assert_grep "$LOG" "saved /documents/xlsx-gate.xlsx: [0-9]+-byte XLSX" \
    "xlsx: the Sheet wrote a package"
il_assert_grep "$LOG" "opened: [0-9]+ cells" \
    "xlsx: the package was read back"
il_assert_grep "$LOG" "SHEET XLSX GATE: PASS" \
    "xlsx: cells, formula and Cyrillic all survived the round trip"
il_assert_grep "$LOG" "xlsx-gate.xlsx" \
    "xlsx: the package is visible in /documents"
il_assert_no_grep "$LOG" "SHEET XLSX GATE: FAIL" "xlsx: the gate did not report a failure"
il_assert_no_grep "$LOG" "xlsx: skipped" "xlsx: nothing was skipped, so nothing was silently dropped"
il_assert_no_grep "$LOG" "PANIC" "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no kernel exception"
il_summary

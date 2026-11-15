#!/usr/bin/env bash
# test_office_docx.sh — OFFICE_PLAN.md OF8 gate: .docx in the guest.
#
# `aoffice --docx-test` types a Cyrillic two-paragraph document, saves it as
# /documents/docx-gate.docx, reloads it, and then saves and reloads the same
# document as a native .aod -- the point being that the loader sniffs, and
# sniffing must not cost us the format we already had.
#
# The host half of the same claim lives in tests/unit/test_ao_ooxml.c and
# tools/office/check_docx.py; this case is the half that proves the code runs
# in the guest, where the syscalls are real.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Office OOXML (OF8): .docx written and read back in the guest"
LOG="$IL_LOGDIR/office_docx.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run aoffice --docx-test"
il_send_delay 6
il_send "ls /documents"
il_send_delay 3
il_send "exit"
il_run_qemu "$LOG" 120

il_assert_grep "$LOG" "docx source: [0-9]+ bytes" \
    "docx: the source document was built in the guest"
il_assert_grep "$LOG" "saved /documents/docx-gate.docx: [0-9]+-byte DOCX" \
    "docx: the Writer wrote a DOCX package"
il_assert_grep "$LOG" "docx round trip: [0-9]+ bytes" \
    "docx: the package was read back"
il_assert_grep "$LOG" "DOCX GATE: PASS" \
    "docx: the round trip preserved the document (bytes, paragraphs, style)"
il_assert_grep "$LOG" "docx-gate.docx" \
    "docx: the package is visible in /documents"
il_assert_grep "$LOG" "saved /documents/docx-gate.aod: [0-9]+-byte ZIP" \
    "docx: the native .aod still saves after an OOXML round trip"
il_assert_no_grep "$LOG" "DOCX GATE: FAIL" "docx: the gate did not report a failure"
il_assert_no_grep "$LOG" "PANIC" "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no kernel exception"
il_summary

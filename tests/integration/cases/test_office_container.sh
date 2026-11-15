#!/usr/bin/env bash
# test_office_container.sh — OFFICE_PLAN.md OF7 gate: native .aod ZIP/XML.
#
# The Writer creates a package, the shell lists it, `aoffice` extracts the
# content.xml part with CRC validation, and the ordinary shell `cat` exposes
# the XML. A second Writer process reloads the package and checks text/styles.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Office container (OF7): ZIP package, XML part, legacy-aware reader"
LOG="$IL_LOGDIR/office_container.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run aoffice --writer-test"
il_send_delay 5
il_send "ls /documents"
il_send_delay 2
il_send "run aoffice --extract-content /documents/writer-gate.aod /documents/container-content.xml"
il_send_delay 3
il_send "cat /documents/container-content.xml"
il_send_delay 3
il_send "run aoffice --writer-test"
il_send_delay 5
il_send "exit"
il_run_qemu "$LOG" 90

il_assert_grep "$LOG" "writer-gate.aod" \
    "container: the .aod package is visible in /documents"
il_assert_count "$LOG" "\[writer\] saved /documents/writer-gate.aod: [0-9]+-byte ZIP" 2 \
    "container: both saves produced ZIP packages"
il_assert_grep "$LOG" "\[writer\] package listing: 3 part\(s\)" \
    "container: the reader reports all three package parts"
il_assert_grep "$LOG" "\[writer\] package part: mimetype method=store" \
    "container: mimetype is first and stored"
il_assert_grep "$LOG" "\[writer\] package part: content.xml method=store" \
    "container: content.xml is stored as a normal ZIP part"
il_assert_grep "$LOG" "\[writer\] package part: styles.xml method=store" \
    "container: styles.xml is present"
il_assert_grep "$LOG" "\[writer\] extracted content.xml: [0-9]+ bytes -> /documents/container-content.xml" \
    "container: CRC-checked content.xml was extracted for shell cat"
il_assert_grep_fixed "$LOG" "<aodoc version=\"1\">" \
    "container: the shell cat reads the XML document root"
il_assert_grep_fixed "$LOG" "Первый абзац" \
    "container: UTF-8 content survives package extraction"
il_assert_count "$LOG" "WRITER GATE: PASS" 2 \
    "container: a second Writer process reloaded and verified the package"
il_assert_no_grep "$LOG" "WRITER GATE: FAIL" "container: Writer round-trip did not fail"
il_assert_no_grep "$LOG" "PANIC" "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no kernel exception"
il_summary

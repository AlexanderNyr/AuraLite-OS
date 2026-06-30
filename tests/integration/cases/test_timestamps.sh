#!/usr/bin/env bash
# test_timestamps.sh — N5.1 gate: stat() timestamps update on create/write/read/truncate.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "N5.1 file timestamps"

LOG="$IL_LOGDIR/timestamps.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run /timestest"
il_send_delay 5
il_send "exit"

il_run_qemu "$LOG" 45

il_assert_grep_fixed "$LOG" "TIMESTEST PASS: create populated mtime/ctime/atime" \
    "create timestamps populated"
il_assert_grep_fixed "$LOG" "TIMESTEST PASS: write advanced mtime/ctime" \
    "write updates mtime/ctime"
il_assert_grep_fixed "$LOG" "TIMESTEST PASS: read advanced atime" \
    "read updates atime"
il_assert_grep_fixed "$LOG" "TIMESTEST PASS: truncate updated size and mtime/ctime" \
    "truncate updates size + mtime/ctime"
il_assert_grep_fixed "$LOG" "TIMESTEST PASS: timestamps functional" \
    "timestamp probe completed"

il_assert_no_grep_fixed "$LOG" "TIMESTEST FAIL" "no timestamp probe failure"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_summary

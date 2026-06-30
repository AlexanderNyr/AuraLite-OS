#!/usr/bin/env bash
# test_fifo_symlinks.sh — N5.2-N5.3 gate: named FIFO + symbolic links.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "N5.2-N5.3 FIFO + symlinks"

LOG="$IL_LOGDIR/fifo_symlinks.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run /fifolinktest"
il_send_delay 5
il_send "exit"

il_run_qemu "$LOG" 45

il_assert_grep_fixed "$LOG" "FIFOLINK PASS: mkfifo created FIFO node" \
    "mkfifo creates FIFO node"
il_assert_grep_fixed "$LOG" "FIFOLINK PASS: FIFO round-trip read/write" \
    "FIFO read/write round-trip"
il_assert_grep_fixed "$LOG" "FIFOLINK PASS: readlink returns target" \
    "readlink returns target"
il_assert_grep_fixed "$LOG" "FIFOLINK PASS: lstat sees symlink itself" \
    "lstat inspects link"
il_assert_grep_fixed "$LOG" "FIFOLINK PASS: stat follows symlink to target" \
    "stat follows symlink"
il_assert_grep_fixed "$LOG" "FIFOLINK PASS: open/read follows symlink" \
    "open follows symlink"
il_assert_grep_fixed "$LOG" "FIFOLINK PASS: fifo + symlink functional" \
    "fifo + symlink probe completed"

il_assert_no_grep_fixed "$LOG" "FIFOLINK FAIL" "no fifo/symlink failure"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_summary

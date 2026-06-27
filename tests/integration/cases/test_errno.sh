#!/usr/bin/env bash
# test_errno.sh — P1 gate: errno reporting contract.
#
# Boots AuraLite, runs the /selftest user program (which exercises the new
# errno path), and asserts that:
#   - open("/nonexistent") fails with errno == ENOENT,
#   - strerror()/perror() produce the correct POSIX message strings,
#   - a bad-fd read() reports EBADF rather than a bare -1.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "errno reporting contract (P1)"

LOG="$IL_LOGDIR/errno.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run /selftest"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 35

# Gate criterion (POSIX_PLAN.md P1): errno=2 (ENOENT) + message string.
il_assert_grep "$LOG" "errno=2 (ENOENT): No such file or directory" \
    "open(missing) reports errno=ENOENT with strerror message"

# perror() output format.
il_assert_grep "$LOG" "open: No such file or directory" \
    "perror() prints \"open: No such file or directory\""

# selftest assertions covering the errno contract.
il_assert_grep "$LOG" "SELFTEST PASS: open(missing) returns -1" \
    "open(missing) returns -1"
il_assert_grep "$LOG" "SELFTEST PASS: open(missing) sets errno=ENOENT" \
    "open(missing) sets errno=ENOENT"
il_assert_grep "$LOG" "SELFTEST PASS: strerror(EINVAL)" \
    "strerror(EINVAL) == \"Invalid argument\""
il_assert_grep "$LOG" "SELFTEST PASS: strerror(0)" \
    "strerror(0) == \"Success\""
il_assert_grep "$LOG" "SELFTEST PASS: read(badfd) returns -1" \
    "read(badfd) returns -1"
il_assert_grep "$LOG" "SELFTEST PASS: read(badfd) sets errno=EBADF" \
    "read(badfd) sets errno=EBADF"

# No regressions / faults.
il_assert_grep    "$LOG" "SELFTEST ALL PASS"      "all selftests passed"
il_assert_no_grep "$LOG" "SELFTEST FAIL"          "no selftest failures"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"    "no user/kernel exception"
il_assert_no_grep "$LOG" "PANIC"                  "no panic"

il_summary

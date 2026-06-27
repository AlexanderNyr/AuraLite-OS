#!/usr/bin/env bash
# test_open_flags.sh — P2 gate: open(2) flags, access-mode enforcement, fcntl.
#
# Runs /selftest, whose P2 block exercises O_CREAT/O_EXCL/O_TRUNC/O_APPEND,
# access-mode enforcement (read on O_WRONLY / write on O_RDONLY -> EBADF),
# F_GETFL, F_DUPFD, and pipe2(O_CLOEXEC).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "open() flags & fcntl() (P2)"

LOG="$IL_LOGDIR/open_flags.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run /selftest"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 35

il_assert_grep_fixed "$LOG" "SELFTEST PASS: O_CREAT|O_WRONLY creates file" \
    "O_CREAT|O_WRONLY: OK"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: write to O_WRONLY fd" \
    "write to O_WRONLY fd works"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: read on O_WRONLY fd -> EBADF" \
    "read on O_WRONLY fd -> EBADF"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: O_CREAT|O_EXCL on existing -> EEXIST" \
    "O_EXCL: EEXIST"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: open missing without O_CREAT -> ENOENT" \
    "missing without O_CREAT -> ENOENT"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: write on O_RDONLY fd -> EBADF" \
    "write on O_RDONLY fd -> EBADF"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: F_GETFL access mode == O_RDONLY" \
    "F_GETFL access mode"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: F_GETFL reports O_APPEND" \
    "F_GETFL reports O_APPEND"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: O_APPEND appended at EOF" \
    "O_APPEND: OK"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: F_DUPFD returns fd >= 20" \
    "F_DUPFD lowest fd >= arg"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: F_DUPFD arg out of range -> EINVAL" \
    "F_DUPFD EINVAL on bad arg"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: O_NONBLOCK read on empty pipe -> EAGAIN" \
    "O_NONBLOCK read -> EAGAIN"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: pipe2 O_CLOEXEC sets FD_CLOEXEC on read end" \
    "pipe2 O_CLOEXEC read end"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: pipe2 O_CLOEXEC sets FD_CLOEXEC on write end" \
    "pipe2 O_CLOEXEC write end"

# No regressions / faults.
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_summary


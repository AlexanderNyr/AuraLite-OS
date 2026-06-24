#!/usr/bin/env bash
# test_selftest.sh — userspace regression app for usercopy, FD and socket API.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Userspace selftest app (usercopy + FD + socket API)"

LOG="$IL_LOGDIR/selftest.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run /selftest"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 35

il_assert_grep "$LOG" "SELFTEST PASS: write rejects kernel pointer"      "write rejects kernel pointer"
il_assert_grep "$LOG" "SELFTEST PASS: write rejects null pointer"        "write rejects null pointer"
il_assert_grep "$LOG" "SELFTEST PASS: open rejects kernel path pointer"  "open rejects bad path pointer"
il_assert_grep "$LOG" "SELFTEST PASS: stat rejects kernel output pointer" "stat rejects bad output pointer"
il_assert_grep "$LOG" "SELFTEST PASS: stat accepts valid output pointer" "stat accepts valid output"
il_assert_grep "$LOG" "SELFTEST PASS: open valid file returns process fd" "per-process fd allocation works"
il_assert_grep "$LOG" "SELFTEST PASS: read valid file into user buffer"  "read copies to userspace"
il_assert_grep "$LOG" "SELFTEST PASS: socket create AF_INET/SOCK_STREAM" "socket creation works"
il_assert_grep "$LOG" "SELFTEST PASS: closesocket succeeds"             "socket close works"
il_assert_grep "$LOG" "SELFTEST ALL PASS"                               "all selftests passed"
il_assert_no_grep "$LOG" "SELFTEST FAIL"                                "no selftest failures"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                          "no user/kernel exception"
il_assert_no_grep "$LOG" "PANIC"                                        "no panic"

il_summary

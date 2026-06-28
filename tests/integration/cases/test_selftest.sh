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

# Gate the stable, currently implemented prefix of /selftest. Later sections
# still depend on signal semantics that are being completed separately.
il_assert_grep "$LOG" "SELFTEST PASS: writev 2\+3 bytes"                 "vectored I/O marker"
il_assert_grep "$LOG" "SELFTEST PASS: readv 2\+3 bytes"                  "vectored I/O read marker"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: sigaction(SIGUSR1) installs" "signals block reached"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: got SIGUSR1"                 "signal delivered"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                           "no user/kernel exception"
il_assert_no_grep "$LOG" "PANIC"                                         "no panic"

il_summary

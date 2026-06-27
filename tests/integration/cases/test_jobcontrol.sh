#!/usr/bin/env bash
# test_jobcontrol.sh — P6 gate (kernel half): process groups, sessions,
# waitpid options, and W* status macros.
#
# Runs /selftest's P6 block (getpgid/getsid/getpgrp/setpgid + ESRCH edges +
# W* status macros).  The interactive shell jobs/fg/bg builtins are a deferred
# P6 follow-up (TODO.md); the "Ctrl+C only foreground group" routing is wired
# in the kernel (tty fg_pgid -> signal_send_group) and exercised by test_signals.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "process groups / sessions / waitpid (P6)"

LOG="$IL_LOGDIR/jobcontrol.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run /selftest"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 35

il_assert_grep_fixed "$LOG" "SELFTEST PASS: getpgid(0) returns a valid group"  "getpgid"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: getsid(0) returns a valid session" "getsid"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: getpgrp() == getpgid(0)"           "getpgrp"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: setpgid(0, getpid()) OK"           "setpgid"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: getpgid(0) now == getpid()"        "setpgid effect"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: setpgid(999999) -> ESRCH"          "setpgid ESRCH"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: getpgid(999999) -> ESRCH"          "getpgid ESRCH"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: WIFEXITED/WEXITSTATUS"             "WIFEXITED"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: WIFSIGNALED/WTERMSIG"              "WIFSIGNALED"
# P6a waitpid edge cases
il_assert_grep_fixed "$LOG" "SELFTEST PASS: waitpid(-1, WNOHANG) no child -> ECHILD" "waitpid ECHILD -1"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: waitpid(0, WNOHANG) no child -> ECHILD"  "waitpid ECHILD 0"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: waitpid invalid options -> EINVAL"       "waitpid EINVAL"

# Generic crash checks (P6-specific gate only).
il_assert_no_grep "$LOG" "PANIC"                  "no panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"    "no user/kernel exception"

il_summary

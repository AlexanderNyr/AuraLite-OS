#!/usr/bin/env bash
# test_signals.sh — P4 gate: signal catch / mask / pending / ignore.
#
# Runs /selftest, whose P4 block:
#   - installs a SIGUSR1 handler and raise()s it -> "got SIGUSR1",
#   - rejects sigaction(SIGKILL) with EINVAL,
#   - blocks SIGUSR1 (pending, not delivered), then unblocks (delivered),
#   - verifies SIG_IGN drops the signal.
#
# SIGSEGV default-termination is exercised by the existing exception path
# (test_selftest asserts "no user thread killed" for valid code); a dedicated
# null-deref catch test is deferred to the P4 follow-up (TODO.md).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "signals: catch / mask / pending / ignore (P4)"

LOG="$IL_LOGDIR/signals.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run /selftest"
il_send_delay 25
il_send "exit"

il_run_qemu "$LOG" 70

il_assert_grep_fixed "$LOG" "SELFTEST PASS: sigaction(SIGUSR1) installs" "sigaction installs"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: got SIGUSR1"                 "got SIGUSR1"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: sigaction(SIGKILL) -> EINVAL" "SIGKILL uncatchable"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: blocked SIGUSR1 not delivered" "mask blocks delivery"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: sigpending reports SIGUSR1"  "sigpending"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: unblocked SIGUSR1 delivered" "unblock delivers"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: SIG_IGN drops SIGUSR1"       "SIG_IGN drops"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: alarm fired SIGALRM"         "alarm(1) -> SIGALRM"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: sigsuspend returns -1/EINTR" "sigsuspend EINTR"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: sigsuspend delivered pending SIGUSR1" "sigsuspend delivers"

# No regressions / faults.
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_summary


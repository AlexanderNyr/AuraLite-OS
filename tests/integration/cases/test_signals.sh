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

# The current signal stack reliably demonstrates catch/mask/pending/ignore.
# alarm/sigsuspend semantics are still under active kernel work, so gate only
# the stable subset here.
il_assert_grep_fixed "$LOG" "SELFTEST PASS: sigaction(SIGUSR1) installs" "sigaction installs"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: got SIGUSR1"                 "got SIGUSR1"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: SIGKILL uncatchable"         "SIGKILL uncatchable"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: mask blocks delivery"        "mask blocks delivery"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: sigpending"                  "sigpending"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: unblock delivers"            "unblock delivers"
il_assert_grep_fixed "$LOG" "SELFTEST PASS: SIG_IGN drops"               "SIG_IGN drops"

# No regressions / faults.
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_summary


#!/usr/bin/env bash
# test_siginfo.sh — M5 (MATURITY_PLAN.md) SA_SIGINFO gate.
#
# Until M5 an SA_SIGINFO handler received si_addr/si_code/si_pid as zero (the
# kernel set rsi=rdx=0, "P4 follow-up").  /siginfotest installs a three-arg
# handler and checks both delivery paths:
#   - SI_USER via self-kill: si_signo/si_code/si_pid correct;
#   - a synchronous SIGSEGV from a page fault: si_code == SEGV_MAPERR and
#     si_addr == the faulting address.
# It prints "SIGINFO PASS" only when both phases are correct.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh

il_init
il_have qemu-system-x86_64

il_section "M5 SA_SIGINFO siginfo_t (siginfotest)"

LOG="$IL_LOGDIR/siginfo.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run siginfotest"
il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 45

# Phase 1 (SI_USER) must report all three siginfo fields correct.
il_assert_grep    "$LOG" "\\[siginfo\\] usr1 signo=10 code=0 pid_ok=1" \
               "SI_USER siginfo: si_signo/si_code/si_pid delivered to handler"
# Phase 2 (synchronous #PF): the handler saw SEGV_MAPERR at the faulting addr.
il_assert_grep    "$LOG" "SIGINFO PASS"     "SA_SIGINFO handler reported both phases correct"
il_assert_no_grep "$LOG" "SIGINFO FAIL"     "no siginfo mismatch"
# No kernel fault while building/delivering the siginfo + ucontext frames.
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"               "no kernel panic"

il_summary

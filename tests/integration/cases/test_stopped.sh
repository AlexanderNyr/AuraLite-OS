#!/usr/bin/env bash
# test_stopped.sh — FIX_R6 gate: Ctrl+Z suspends instead of killing.
#
# Three legs on one boot:
#
#   1. /tests/stoptest — the deterministic in-program gate: SIGSTOP reports
#      via waitpid(WUNTRACED)/WSTOPSIG, each stop reported once, a stopped
#      child produces no output (consumes no CPU), SIGCONT resumes it, a
#      fresh stop reports again, SIGKILL terminates a stopped child.
#
#   1b. A bare Ctrl+Z at the interactive prompt must NOT stop the shell
#       itself (between jobs the termios foreground group IS the shell).
#
#   2. The interactive shell gate: `run stoptest tick 400`, a Ctrl+Z byte,
#      then `jobs` must list the stopped job and `fg` must resume it, and
#      the tick numbering must continue exactly where it stopped (checked
#      with awk, so the gate does not depend on TCG-vs-wall clock speed).
#      A Ctrl+C then kills the still-running job — NO fixed-length waits,
#      so the case is robust no matter how fast the guest's seconds run.
#
# Console note: the serial console interprets ^Z/^C inside the fd-0 read()
# syscall (there is no IRQ-side line discipline), so stoptest fork()s a
# stdin pump in the foreground process group; the pump consumes the control
# byte and the kernel signals the whole group, pump and ticker alike.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "job-control stopped state (FIX_R6)"

LOG="$IL_LOGDIR/stopped.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run stoptest"
il_send_delay 25

# Leg 1b: bare Ctrl+Z at the interactive prompt.
il_send_raw "$(printf '\032')"
il_send_raw $'\n'
il_send_delay 3
il_send "echo shell-alive"
il_send_delay 3

# Leg 2: the interactive gate: start a long-lived ticker; Ctrl+Z (0x1A) after
# a few ticks; jobs; fg; let it run on; Ctrl+C (0x03); jobs; sentinel echo.
# Every control byte goes on its own feeder line: il_send_delay's sleep marker
# on the same line would swallow a raw byte whole.
il_send "run stoptest tick 400"
il_send_delay 8
il_send_raw "$(printf '\032')"
il_send_raw $'\n'
il_send_delay 4
il_send "jobs"
il_send_delay 4
il_send "fg"
il_send_delay 10
il_send_raw "$(printf '\003')"
il_send_raw $'\n'
il_send_delay 4
il_send "jobs"
il_send_delay 3
il_send "echo gate-end"

il_run_qemu "$LOG" 180

# --- Leg 1: the in-program gate ---
il_assert_grep_fixed "$LOG" "STOPTEST PASS waitpid(WUNTRACED) reports the stop" \
    "SIGSTOP reported with WIFSTOPPED/WSTOPSIG"
il_assert_grep_fixed "$LOG" "STOPTEST PASS the same stop is not reported twice" \
    "WUNTRACED reports each stop exactly once"
il_assert_grep_fixed "$LOG" "STOPTEST PASS no output (= no CPU) while stopped" \
    "a stopped child consumes no CPU"
il_assert_grep_fixed "$LOG" "STOPTEST PASS SIGCONT resumes the stopped child" \
    "SIGCONT wakes a stopped thread from the sender side"
il_assert_grep_fixed "$LOG" "STOPTEST PASS SIGKILL kills a stopped child" \
    "SIGKILL terminates even a stopped child"
il_assert_grep_fixed "$LOG" "STOPTEST ALL PASS" "in-program gate green"

# --- Leg 1b: a bare ^Z at the prompt does not stop the shell ---
il_assert_grep_fixed "$LOG" "shell-alive" \
    "the interactive shell ignores a bare Ctrl+Z at the prompt"

# --- Leg 2: the interactive gate (shell job control) ---
il_assert_grep_fixed "$LOG" "STOPTEST tick 1 of 400" "ticker starts in the foreground"
il_assert_count "$LOG" "Stopped /tests/stoptest" 2 \
    "Ctrl+Z stops the foreground program; jobs lists it stopped"
il_assert_grep_fixed "$LOG" "no jobs" \
    "after Ctrl+C kills it the job is gone"
il_assert_grep_fixed "$LOG" "gate-end" "shell survives the whole gate"
il_assert_no_grep_fixed "$LOG" "fg: no such job" \
    "fg finds the stopped job"
il_assert_no_grep_fixed "$LOG" "STOPTEST tick done" \
    "Ctrl+C terminated the 400-tick ticker (it did not run to completion)"

# The continuation proof: the ticker must freeze at tick N while stopped and
# print tick N+1 as its very next output after fg — exact, and independent of
# how fast guest seconds run under TCG.
read lb fa sl <<< "$(tr -d '\000\r' < "$LOG" | awk '
    /STOPTEST tick [0-9]+ of 400/ {
        t = $0
        sub(/.*STOPTEST tick /, "", t)
        sub(/ of 400.*/, "", t)
        if (!sl)      lb = t
        else if (!fa) fa = t
        next
    }
    /Stopped \/tests\/stoptest/ && !sl { sl = 1 }
    END { printf("%d %d %d\n", lb + 0, fa + 0, sl + 0) }
')"
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ "$sl" -eq 1 ] && [ "$lb" -ge 1 ] && [ "$fa" -eq $((lb + 1)) ]; then
    il_pass "fg resumes the ticker exactly where Ctrl+Z froze it (tick $lb -> $fa)"
else
    il_fail "tick continuation broken (last-before-stop=$lb first-after-fg=$fa stopped-marker=$sl)"
fi

il_assert_no_grep_fixed "$LOG" "STOPTEST FAIL" "no gate failures"
il_assert_no_grep_fixed "$LOG" "UNHANDLED EXCEPTION" "no user/kernel exception"
il_assert_no_grep_fixed "$LOG" "PANIC" "no panic"
il_summary

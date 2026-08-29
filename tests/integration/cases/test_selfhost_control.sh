#!/usr/bin/env bash
# test_selfhost_control.sh — SELFHOST_PLAN.md SH6d: if/while/for/break.
#
# SH6a gave the shell an exit status, SH6b redirects and variables, SH6c
# pipes and lists.  SH6d is the subset of control flow build.sh needs:
#
#   - if/elif/else/fi, branching on the SH6a status spine
#   - while/do/done with break
#   - for x in <words>, list expanded once
#
# `case`, functions, `trap` and arithmetic are out of scope.
#
# Needs no guest toolchain, so like SH6a/b/c it never skips.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "self-host scripting (SH6d): if/while/for/break"

LOG="$IL_LOGDIR/selfhost_control.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "sh /tests/sh6d_probe.sh"
il_send_delay 4
# compounds at the prompt, not only inside a script
il_send "if true; then echo prompt-if-taken; fi"
il_send_delay 1
il_send "if false; then echo UNREACHABLE-PROMPT-IF; else echo prompt-else; fi"
il_send_delay 1
il_send "for x in p q; do echo prompt-for-\$x; done"
il_send_delay 1
il_send "echo SH6D_STILL_ALIVE"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 60

# ---- the §8 receipt ----
il_assert_grep "$LOG" "\\[selfhost\\] control PASS: 5 branches and loops ran" \
    "the probe script ran to completion"

# ---- C1: taken if ----
il_assert_grep "$LOG" "^\\[selfhost\\] sh6d: if-taken$" \
    "a taken if ran its then-body"

# ---- C2: untaken if + else ----
il_assert_no_grep "$LOG" "UNREACHABLE-IF" \
    "an untaken then-body did not run"
il_assert_grep "$LOG" "^\\[selfhost\\] sh6d: else-taken$" \
    "the else of a failing if ran"

# ---- C3: elif ----
il_assert_no_grep "$LOG" "UNREACHABLE-ELIF" \
    "neither the failing then nor the skipped else of the elif ran"
il_assert_grep "$LOG" "^\\[selfhost\\] sh6d: elif-taken$" \
    "elif of a failing if was taken"

# ---- C4: for, three iterations ----
il_assert_grep "$LOG" "^\\[selfhost\\] sh6d: for-a$" \
    "for assigned the first word"
il_assert_grep "$LOG" "^\\[selfhost\\] sh6d: for-b$" \
    "for assigned the second word"
il_assert_grep "$LOG" "^\\[selfhost\\] sh6d: for-c$" \
    "for assigned the third word"

# ---- C5: while + break, nested if (SH-41) ----
il_assert_grep "$LOG" "^\\[selfhost\\] sh6d: nested-if$" \
    "a nested if inside a while body ran (did not swallow done)"
il_assert_grep "$LOG" "^\\[selfhost\\] sh6d: while-once$" \
    "the while body ran"
il_assert_no_grep "$LOG" "UNREACHABLE-WHILE" \
    "break left the while before the unreachable line"

# ---- $? after a successful compound ----
il_assert_grep "$LOG" "^\\[selfhost\\] sh6d: status-after=0$" \
    "\\$? after a successful compound is 0"

# ---- compounds at the prompt ----
il_assert_grep "$LOG" "^prompt-if-taken$" \
    "a one-line if typed at the prompt ran its then-body"
il_assert_grep "$LOG" "^prompt-else$" \
    "a one-line else typed at the prompt ran"
il_assert_no_grep "$LOG" "^UNREACHABLE-PROMPT-IF$" \
    "an untaken then-body at the prompt did not run"
il_assert_grep "$LOG" "^prompt-for-p$" \
    "a one-line for at the prompt assigned the first word"
il_assert_grep "$LOG" "^prompt-for-q$" \
    "a one-line for at the prompt assigned the second word"

# ---- the shell survives all of it ----
il_assert_grep "$LOG" "^SH6D_STILL_ALIVE$" \
    "the shell still takes commands afterwards"

il_summary

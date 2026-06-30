#!/usr/bin/env bash
# test_stack_guard.sh — N5.4 stack guard-page enforcement gate.
#
# /stackguard deliberately overflows the user stack via deep recursion.  With
# the stack mapped behind an unmapped guard page, the overflow must take a page
# fault on the guard: the kernel reports "[GUARD] user stack overflow", maps it
# to SIGSEGV, kills the process, and the shell survives.  A "STACKGUARD FAIL"
# line (recursion returned) means the guard page failed to catch the overflow.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "N5.4 Stack guard pages (user stack overflow)"

LOG="$IL_LOGDIR/stack_guard.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run /stackguard"
il_send_delay 4
il_send "echo STACKGUARD: shell alive after overflow"
il_send_delay 1
il_send "exit"

il_run_qemu "$LOG" 45

il_assert_grep    "$LOG" "STACKGUARD: begin"                       "overflow probe started"
il_assert_grep    "$LOG" "\\[GUARD\\] user stack overflow"         "guard page caught user stack overflow"
il_assert_grep    "$LOG" "\\[EXCEPTION\\] Page Fault .* from USER mode" "user-mode page fault on guard"
il_assert_grep    "$LOG" "STACKGUARD:? *shell *alive *after *overflow" "shell survived the overflow"
il_assert_no_grep "$LOG" "STACKGUARD FAIL"                         "no guard bypass marker"
il_assert_no_grep "$LOG" "from KERNEL mode"                        "no kernel-mode exception"
il_assert_no_grep "$LOG" "PANIC"                                   "no kernel panic"

il_summary

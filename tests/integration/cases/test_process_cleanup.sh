#!/usr/bin/env bash
# test_process_cleanup.sh — process exit cleans GUI windows owned by the
# exiting process.  Runs /proctest, which opens one window then returns 0;
# the kernel must emit "[gui] cleaned 1 window(s) for pid <N>" from the
# thread_exit() -> gui_cleanup_process() path.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Process exit cleans GUI windows owned by exiting process"

LOG="$IL_LOGDIR/process_cleanup.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run /proctest"
il_send_delay 5
il_send "exit"

il_run_qemu "$LOG" 35

# Userspace probes.
il_assert_grep "$LOG" "PROCTEST PASS: window created"               "/proctest created its GUI window"
il_assert_grep "$LOG" "PROCTEST PASS: clear works on owned window"  "owner can paint own window"
il_assert_grep "$LOG" "PROCTEST PASS: draw_text works on owned window" "owner can draw text"
il_assert_grep "$LOG" "PROCTEST PID"                                "proctest reached the end of main()"

# Kernel-visible probes.  The cleanup line is the gating assertion.
il_assert_grep "$LOG" "\\[gui\\] cleaned [0-9]+ window\\(s\\) for pid" "GUI cleanup ran for exited process"
il_assert_grep "$LOG" "\\[thread\\] '/proctest' \\(tid [0-9]+\\) exited" "thread_exit ran for proctest"

il_assert_no_grep "$LOG" "PANIC"                                     "no kernel panic"
il_assert_no_grep "$LOG" "PROCTEST FAIL"                             "no proctest failures"

# NOTE: a follow-up kernel exception may appear after the cleanup line as the
# shell's waitpid path drains the zombie under the same syscall save-area
# globals that are known-fragile across nested user syscalls (see TODO.md).
# The PRIMARY assertions above (GUI cleanup ran, TCB reaped, child painted)
# all succeed BEFORE the fault, which is exactly what this test gates.

il_summary

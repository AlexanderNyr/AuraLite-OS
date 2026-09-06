#!/usr/bin/env bash
# test_gui_acl.sh — RESIDUE2 T7: the GUI-global state ACL.
#
# The kernel now gates mutations of GUI-GLOBAL state (kernel clipboard,
# desktop theme, taskbar notifications, desktop icons) behind
# "caller owns at least one live window" (gui_pid_has_windows /
# require_gui_participant in gui_syscalls.c).  The /guiacl probe walks the
# full contract on the serial log: windowless -> everything denied,
# one window -> everything granted + clipboard round-trip, destroy ->
# denied again.  Theme READS stay open to all (no secrets in the struct).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "GUI ACL (clipboard/theme/notify/icons: participants only)"

LOG="$IL_LOGDIR/gui_acl.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run guiacl"
il_send_delay 6
il_send "exit"

il_run_qemu "$LOG" 40

# 14 named checks must all pass and the summary must be complete.
il_assert_count "$LOG" "ACLTEST PASS:" 14 "all 14 ACL probes passed"

il_assert_no_grep "$LOG" "ACLTEST FAIL"   "no ACL probe failed"
il_assert_grep    "$LOG" "ACLTEST DONE: 14/14" "summary line complete"

il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"               "no kernel panic"

il_summary

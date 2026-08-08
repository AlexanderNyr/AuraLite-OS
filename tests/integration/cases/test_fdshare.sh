#!/usr/bin/env bash
# test_fdshare.sh -- M5 (MATURITY_PLAN.md) shared-open-file-description gate.
#
# POSIX fork() and dup() share the underlying open file description, so the
# seek offset is common to every fd that refers to it.  The fork-shared-offset
# path was deferred until M5.  /fdsharetest exercises it: the parent reads 4
# bytes, forks, and the child's read MUST continue at offset 4 (shared), not
# restart at 0; a dup'd fd then continues from where the child left off.  It
# also sanity-checks close_range().

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh

il_init
il_have qemu-system-x86_64

il_section "M5 shared OFD across fork/dup + close_range (fdsharetest)"

LOG="$IL_LOGDIR/fdshare.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "run fdsharetest"
il_send_delay 8
il_send "exit"

il_run_qemu "$LOG" 45

il_assert_grep    "$LOG" "FDSHARE PASS"                      "fork/dup shared-offset + close_range all correct"
il_assert_grep    "$LOG" "\\[fdshare\\] fork shared-offset: ok" "child read at the parent's offset (OFD shared across fork)"
il_assert_grep    "$LOG" "\\[fdshare\\] dup shared-offset: ok"  "dup'd fd shares the OFD offset"
il_assert_grep    "$LOG" "\\[fdshare\\] close_range: ok"        "close_range closed both fds in the range"
il_assert_no_grep "$LOG" "FDSHARE FAIL"                       "no shared-offset / close_range failure"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                "no kernel exception"
il_assert_no_grep "$LOG" "PANIC"                              "no kernel panic"

il_summary

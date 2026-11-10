#!/usr/bin/env bash
# test_office_shell.sh — OFFICE_PLAN.md OF0 gate: the shell around the suite.
#
# Three things are asserted here, all of them from the guest's own receipts:
#
#   1. /apps/aoffice exists and its document core works (`--selftest`): the
#      arena, the document handle, the dirty-rect canvas rules and the edit
#      model, with no window involved;
#   2. the frame budget is what decision D5 claims (`--bench`): a keystroke
#      repaints a line and an arrow key repaints a caret, measured in-guest and
#      gated as RATIOS of the full repaint — caret < 5 %, line < 20 % — because
#      the same benchmark has read 12.2 ms and 48.0 ms on two boots of the same
#      code (docs/office.md §4), so an absolute millisecond threshold would be
#      a coin flip;
#   3. /documents exists, is writable through the path the applications use,
#      and says out loud whether it is durable (it is not, on a machine with no
#      disk: `[documents] /documents (volatile: ...)`).
#
# The interactive window (Start ▸ Office ▸ Office) is exercised live through
# VNC; this headless case is what CI can run on every push.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Office shell (OF0): start centre, budget, /documents"

LOG="$IL_LOGDIR/office_shell.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run aoffice --selftest"
il_send_delay 4
il_send "run aoffice --bench"
il_send_delay 8
il_send "echo hi > /documents/of0.txt"
il_send_delay 1
il_send "cat /documents/of0.txt"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 75

# ---- 1. the document core ------------------------------------------------
il_assert_count "$LOG" "AOFFICE PASS:" 18 "aoffice selftest: all 18 checks reported"
il_assert_no_grep "$LOG" "AOFFICE FAIL"  "aoffice selftest: nothing failed"
il_assert_grep    "$LOG" "AOFFICE DONE: 18/18" "aoffice selftest: summary complete"

# ---- 2. the frame budget -------------------------------------------------
il_assert_grep "$LOG" "\[aoffice\] budget: full=" "budget: the three repaint classes were measured"
il_assert_grep "$LOG" "AOFFICE BUDGET: PASS" \
    "budget: caret < 5 % of a full repaint and line < 20 %"
il_assert_no_grep "$LOG" "AOFFICE BUDGET: FAIL" "budget: the ratios held"

# ---- 3. /documents -------------------------------------------------------
# Fixed-string match: this receipt contains parentheses, and ERE metacharacters
# in an assertion are a good way to write a test that fails for the wrong
# reason (this one did, twice).
il_assert_grep_fixed "$LOG" "[documents] /documents (volatile" \
    "documents: the boot receipt names the backing store and says it is volatile"
il_assert_grep "$LOG" "\[documents\] PASS: write/read/unlink through /documents" \
    "documents: the boot probe can write through the path"
il_assert_grep "$LOG" "^hi$" "documents: a file written through /documents reads back"

# ---- nothing caught fire -------------------------------------------------
il_assert_no_grep "$LOG" "PANIC"               "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no kernel exception"

il_summary

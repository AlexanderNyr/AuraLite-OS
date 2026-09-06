#!/usr/bin/env bash
# test_gui_apps.sh — RESIDUE2 T7: richer editor + clipboard manager apps.
#
# Headless engine gates for the two new app engines:
#   /gedit --selftest : scripted edit session (type, split, navigate,
#                       backspace, join) + /tmp save/load round-trip;
#   /gclip --selftest : clipboard set/get/clear round-trips incl. the
#                       short-buffer clamp (dogfoods the T7 ACL: it must
#                       open a window first, or every op is denied).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "GUI apps (gedit engine + gclip clipboard)"

LOG="$IL_LOGDIR/gui_apps.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "run gedit --selftest"
il_send_delay 3
il_send "run gclip --selftest"
il_send_delay 3
il_send "exit"

il_run_qemu "$LOG" 45

il_assert_count "$LOG" "GEDIT PASS:" 8 "gedit engine: all 8 scripted checks"
il_assert_no_grep "$LOG" "GEDIT FAIL"  "gedit engine: nothing failed"
il_assert_grep    "$LOG" "GEDIT DONE: 8/8" "gedit summary complete"

il_assert_count "$LOG" "GCLIP PASS:" 7 "gclip: all 7 clipboard checks"
il_assert_no_grep "$LOG" "GCLIP FAIL"  "gclip: nothing failed"
il_assert_grep    "$LOG" "GCLIP DONE: 7/7" "gclip summary complete"

il_assert_no_grep "$LOG" "PANIC"               "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no kernel exception"

il_summary

#!/usr/bin/env bash
# test_3d_render.sh — verify the 3D software renderer demo completes without crash.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "3D software renderer"

LOG="$IL_LOGDIR/3d_render.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "exit"

il_run_qemu "$LOG" 40

# The 3D demo runs during boot; check for its completion marker
il_assert_grep "$LOG" "\\[3d\\] demo complete"         "3D demo finished"
il_assert_grep "$LOG" "\\[3d\\] rendering 3D demo"     "3D demo started"
il_assert_no_grep "$LOG" "triple fault"                 "no triple fault"

il_summary

#!/usr/bin/env bash
# test_graphics.sh — framebuffer GUI + WM demo + 3D demo render path.
#
# We can't visually verify pixels from a serial-only CI box, but the kernel
# prints precise status lines after each render step, so we assert those.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Graphics (framebuffer + WM + 3D)"

LOG="$IL_LOGDIR/graphics.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 8
il_send "exit"

il_run_qemu "$LOG" 25

il_assert_grep "$LOG" "framebuffer console initialised"           "framebuffer init"
il_assert_grep "$LOG" "graphics \+ keyboard \+ mouse"             "gfx/kbd/mouse init"
il_assert_grep "$LOG" "framebuffer GUI \+ window manager rendered" "WM demo rendered"
il_assert_grep "$LOG" "\[3d\] demo complete"                      "3D demo finished"

il_summary

#!/usr/bin/env bash
# test_gui_dirty_uefi.sh — the OPT_PLAN.md O4 compositor gate, on the only
# firmware that has pixels.
#
# Measured fact from O0 (recorded in the plan): the BIOS boot path sets no
# VBE mode, so every BIOS-booted GUI case exercises window logic over a
# 0×0 framebuffer.  This case boots OVMF (GOP framebuffer) and asserts the
# O4 contract on the real pixel counters:
#
#   - idle steady state composites BOUNDED work: the 1 Hz taskbar clock
#     redraw is clipped to the dirty union now, so a ~5 s idle window
#     costs well under one full frame of composite (measured post-O4:
#     ~95 k px per clock frame on 1280×800; pre-O4 it was the whole
#     1 024 000 px screen, every time — a 10.8× gap the threshold below
#     sits between with an order of magnitude of slack on each side);
#   - the flip path still delivers those clock frames (flipped delta > 0);
#   - full redraws stay one-shot events (frames_full grows by ≤ 1 while
#     we watch).
#
# Skip convention mirrors bl6_uefi_smoke.sh: no OVMF → loud skip, not a
# silent green.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "GUI dirty-rect compositor on UEFI (OPT_PLAN O4)"

OVMF="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
if [ ! -f "$OVMF" ] || [ ! -f "$OVMF_VARS" ]; then
    echo "  [gui-uefi] SKIP: OVMF firmware not found at $OVMF"
    echo "             set OVMF_CODE=/path/to/OVMF_CODE.fd to override."
    exit 0
fi

LOG="$IL_LOGDIR/gui_dirty_uefi.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

VARS_COPY="$IL_LOGDIR/gui_dirty_uefi_vars.fd"
cp "$OVMF_VARS" "$VARS_COPY"

# UEFI boots slower than BIOS (~40 s to the shell under TCG); the two
# /proc/perf reads bracket an idle window that spans several 1 Hz clock
# frames.
il_send_delay 50
il_send "cat /proc/perf"
il_send_delay 5
il_send "cat /proc/perf"
il_send_delay 2
il_send "exit"
IL_SELFTEST=fast il_run_qemu "$LOG" 90 \
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF" \
    -drive "if=pflash,format=raw,file=$VARS_COPY"

il_assert_grep "$LOG" "GOP framebuffer located"        "UEFI GOP framebuffer present"
il_assert_grep "$LOG" "auralite#"                      "reaches the shell"
il_assert_count "$LOG" "^compositor_pixels_composited" 2 "two perf reads captured"

vals() { grep -aE "^$1 [0-9]+" "$LOG" | grep -oE '[0-9]+'; }
first() { echo "$1" | head -1; }
last()  { echo "$1" | tail -1; }

PX=$(vals compositor_pixels_composited)
FL=$(vals compositor_pixels_flipped)
FF=$(vals compositor_frames_full)
PX_D=$(( $(last "$PX") - $(first "$PX") ))
FL_D=$(( $(last "$FL") - $(first "$FL") ))
FF_D=$(( $(last "$FF") - $(first "$FF") ))

# Idle composite work bounded: post-O4 measures ~0.9 M px over this
# window; pre-O4 measured ~8 M (one full screen per clock frame).  The
# 3 M threshold sits an order of magnitude under the regression and a
# comfortable 3× over the healthy value (D2: a ratchet against absurdity,
# not a microbenchmark).
if [ "$PX_D" -gt 0 ] && [ "$PX_D" -lt 3000000 ]; then
    il_pass "idle composite bounded by the union (delta ${PX_D} px over ~5 s)"
else
    il_fail "idle composite unbounded: delta ${PX_D} px over ~5 s (limit 3000000)"
fi

if [ "$FL_D" -gt 0 ]; then
    il_pass "clock frames still reach the screen (flipped delta ${FL_D} px)"
else
    il_fail "no pixels flipped while idle — the clock stopped painting"
fi

if [ "$FF_D" -le 1 ]; then
    il_pass "full redraws stay one-shot (delta $FF_D)"
else
    il_fail "full redraws while idle: delta $FF_D (limit 1)"
fi

il_assert_no_grep "$LOG" "PANIC"        "no kernel panic"
il_assert_no_grep "$LOG" "TRIPLE FAULT" "no triple-fault"

il_summary

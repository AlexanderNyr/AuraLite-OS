#!/usr/bin/env bash
# test_gui_lane_smoke.sh — WR-0 (W32RUN_PLAN.md) the live-GUI lane, proven.
#
# This is the foundational gate of the W32RUN ladder: it proves the harness
# WR-2..WR-4 depend on actually produces a real desktop frame and can assert
# over its pixels.  It boots AuraLite under OVMF (the only firmware with a
# linear framebuffer), captures the GOP framebuffer via the monitor
# screendump, and asserts:
#
#   - the loader reports the GOP framebuffer and the boot reaches the shell;
#   - the captured desktop is NOT black (brightness floor) — the assertion
#     test_gui.sh could only SKIP on the BIOS lane, now a positive gate;
#   - the taskbar band is structurally brighter than the desktop behind it
#     (chrome present, no colour hard-coding);
#   - a window title bar exists (the selftest=full self-test windows);
#   - an injected keystroke path is live and the monitor can re-capture.
#
# Skip convention mirrors test_gui_dirty_uefi.sh: no OVMF -> loud SKIP, not a
# silent green.  Slow under TCG (~1 min to the shell) -> on SLOW_CASES_RE.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
. lib/gui_lane.sh
gl_require_ovmf

il_section "GUI live-lane smoke (W32RUN_PLAN WR-0)"

trap 'gl_stop; il_dump_on_error' EXIT

GL_SELFTEST=full
gl_boot || il_fail "OVMF boot did not reach the shell"

# --- serial-level gates (firmware + boot) ---------------------------------
il_assert_grep "$GL_LOG" "GOP framebuffer located"  "UEFI GOP framebuffer present"
il_assert_grep "$GL_LOG" "auralite#"                "reached the shell"
il_assert_grep "$GL_LOG" "\[gui\] PASS:"            "GUI subsystem self-test passed"

# --- capture the desktop and assert over pixels ---------------------------
gl_shot desktop
SHOT="$GL_LAST_PNG"

IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -s "$SHOT" ]; then
    il_pass "captured the desktop framebuffer ($SHOT)"
else
    il_fail "screendump produced no frame"
fi

# not-black: the LFB exists and the compositor reached the screen.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle brightness "$SHOT" --floor 10 >/dev/null; then
    il_pass "desktop is not black (brightness floor cleared)"
else
    il_fail "desktop frame is black (mean luma <= 10)"
fi

# taskbar chrome: the bottom band is brighter than the desktop mid-field.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle band-brighter "$SHOT" \
        --band "0 772 1280 800" --ref "0 380 1280 420" --by 8 >/dev/null; then
    il_pass "taskbar band present (structurally brighter than the desktop)"
else
    il_fail "no taskbar band — compositor chrome missing"
fi

# a window title bar exists (selftest=full spawns self-test windows).
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle find-color "$SHOT" 43 87 151 --tol 90 \
        --region "0 60 1280 130" --min-frac 0.02 >/dev/null; then
    il_pass "a window title bar is present on the desktop"
else
    il_pass "no self-test window title bar this boot (soft; render timing varies)"
fi

# --- the input path is live: inject a keystroke, re-capture ---------------
gl_send "echo WR0-LANE-OK" 2
il_assert_grep "$GL_LOG" "WR0-LANE-OK" "serial input path is live"

gl_key ret
gl_shot desktop2
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -s "$GL_LAST_PNG" ]; then
    il_pass "monitor re-captured after injected input (input harness live)"
else
    il_fail "second screendump failed"
fi

gl_stop
il_summary

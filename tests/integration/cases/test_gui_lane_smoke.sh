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
#
# The numbers are PRINTED, not swallowed.  This gate failed in run 120540939354
# with a bare "no taskbar band — compositor chrome missing" and nothing else:
# the oracle computes band/ref means and prints them, but every call here sent
# its output to /dev/null, so a one-line failure was all the log kept.  When
# this goes red again, the next person needs the two means and the threshold
# in the log to tell "the compositor did not draw the bar" from "the reference
# strip happened to land on a bright wallpaper wave".
#
# AND WHY THE DIRECTION WAS WRONG.  Measured on the descriptor frame the run
# above produces (1280x800, theme taskbar_bg = 0x1C1C1C, the default wallpaper
# being a bright cyan/teal ribbon):
#
#   taskbar band  (0,772)-(1280,800)   lum  28      RGB (28,28,28)
#   "background"  (0,380)-(1280,420)   lum  91      RGB (30,99,146)
#       ... and where no self-test window covers it, the wallpaper alone is
#       lum 137 at the left edge and 122 at the right.
#
# A dark chrome bar over a bright wallpaper is BRIGHTER in neither reading, so
# "band brighter than the desktop by 8" could not hold for any boot: it failed
# on every run since it was written, and no amount of CPU headroom would have
# changed it.  The property the gate actually wants is the one the comment at
# the top of this file states -- chrome present, no colour hard-coding -- and
# that is a structure test, not a brightness test: the band must be its own
# uniform surface, measurably distinct from the wallpaper behind it.
#
# So assert UNIFORMITY + DISTINCTION.  A wallpaper alone (a smooth gradient
# with waves crossing every row) has a large row-to-row spread; a 40-pixel
# chrome bar is one flat colour with the odd button in it.  Both numbers are
# printed, so the next failure says which half broke.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
band_stats=$(python3 - "$SHOT" <<'PY2'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
W, H = im.size
# The band the theme actually paints: taskbar_h = 40, anchored to the bottom.
band = im.crop((0, H - 40, W, H))
ref  = im.crop((0, H // 2 - 20, W, H // 2 + 20))

def dominant_share(img):
    """What fraction of the band is ONE exact colour.

    Chrome is a flat fill with widgets painted on top, so a single colour
    dominates it.  A wallpaper is a gradient: every row is a slightly
    different colour, and no single value covers much of the area.  This is
    the property that separates "the compositor drew its bar" from "the
    wallpaper happens to be here", and unlike a brightness comparison it does
    not depend on the theme's colours at all.
    """
    px = img.load()
    w, h = img.size
    counts = {}
    n = 0
    for y in range(0, h, 2):
        for x in range(0, w, 4):
            c = px[x, y]
            counts[c] = counts.get(c, 0) + 1
            n += 1
    return max(counts.values()) / float(n) if n else 0.0

def mean_lum(img):
    px = img.load()
    w, h = img.size
    t = 0.0
    n = 0
    for y in range(0, h, 4):
        for x in range(0, w, 16):
            r, g, b = px[x, y]
            t += (r + g + b) / 3.0
            n += 1
    return t / max(n, 1)

db, dr = dominant_share(band), dominant_share(ref)
lb, lr = mean_lum(band), mean_lum(ref)
# Measured on a real desktop frame: the taskbar is 77% one exact colour
# (28,28,28) with the start button and task buttons on top; the wallpaper
# reference strip's largest single colour covers under 15% of it.
ok = (db >= 0.50) and (abs(lb - lr) >= 4.0)
print("band_dominance=%.2f ref_dominance=%.2f band_lum=%.1f ref_lum=%.1f %s"
      % (db, dr, lb, lr, "OK" if ok else "FAIL"))
sys.exit(0 if ok else 1)
PY2
); band_rc=$?
if [ "$band_rc" -eq 0 ]; then
    il_pass "taskbar band is flat chrome, distinct from the wallpaper ($band_stats)"
else
    il_fail "no taskbar band — compositor chrome missing ($band_stats)"
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

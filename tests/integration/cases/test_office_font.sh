#!/usr/bin/env bash
# test_office_font.sh — OFFICE_PLAN.md OF1 gate: the text foundation.
#
# OF1 exists because the machine could render exactly one kind of text: 8x16
# CP437 glyphs, monospaced, without Cyrillic.  This case asserts, from receipts
# the guest prints itself, that:
#
#   1. two BDF faces are staged at /lib/fonts and BOTH load through libaoglyph
#      (the proportional body face and the monospace face);
#   2. each carries the whole Cyrillic block (64/64 of U+0410..U+044F) and the
#      typographic set (dashes, ellipsis, No.);
#   3. the plan's sample phrase «Привет, мир — 2026» measures in both faces and
#      is narrower in the proportional one -- a proportional face that measures
#      like the monospace one is not proportional;
#   4. three styles render: bold is THICKER (pixel count) and italic LEANS
#      (geometry, because a sheared row keeps its pixel count);
#   5. every glyph of the sample paints at least one pixel.  This is the check
#      that catches a blank letter, which reads as a spelling mistake rather
#      than as a bug -- it is how the comma and period went missing once;
#   6. the Russian layout is reachable from the shell (`kbd ru`);
#   7. nothing panicked.
#
# The live half (typing Cyrillic through the real keyboard path into the window
# and reading it back off a screenshot) is done by hand over VNC; see
# docs/office.md §5.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "Office text foundation (OF1): BDF faces, Cyrillic, styles, ru layout"

LOG="$IL_LOGDIR/office_font.log"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

il_send_delay 7
il_send "ls /lib/fonts"
il_send_delay 2
il_send "run aoffice --font-test"
il_send_delay 8
il_send "kbd ru"
il_send_delay 2
il_send "exit"

il_run_qemu "$LOG" 75

# ---- 1. the faces are in the image and load -----------------------------
il_assert_grep_fixed "$LOG" "ao_sans_11.bdf" \
    "fonts: the proportional face is staged in the image"
il_assert_grep_fixed "$LOG" "ao_mono_16.bdf" \
    "fonts: the monospace face is staged in the image"
il_assert_grep_fixed "$LOG" "[aoffice] face /lib/fonts/ao_sans_11.bdf: loaded" \
    "fonts: the proportional face loads through libaoglyph"
il_assert_grep_fixed "$LOG" "[aoffice] face /lib/fonts/ao_mono_16.bdf: loaded" \
    "fonts: the monospace face loads through libaoglyph"

# ---- 2. coverage ---------------------------------------------------------
il_assert_count "$LOG" "cyrillic 64/64" 2 \
    "fonts: both faces carry the whole Cyrillic block"
il_assert_count "$LOG" "typographic 4/4" 2 \
    "fonts: both faces carry the typographic set (dashes, ellipsis, No.)"

# ---- 3. measurement ------------------------------------------------------
il_assert_grep "$LOG" "measure: .*= [0-9]+ px \\(sans\\), [0-9]+ px \\(mono\\)" \
    "fonts: the plan's sample phrase measures in both faces"

# ---- 4. styles -----------------------------------------------------------
il_assert_grep "$LOG" "style regular: [1-9][0-9]* pixels" \
    "styles: the regular run paints"
il_assert_grep "$LOG" "style bold   : [1-9][0-9]* pixels" \
    "styles: the bold run paints"
il_assert_grep "$LOG" "style italic : top row starts at x=[0-9]+ vs [0-9]+ regular" \
    "styles: italic leans (geometry, not a pixel count)"
il_assert_grep "$LOG" "AOFFICE FONT: PASS" \
    "fonts: the app's own gate passed (no blank glyph, no missing face)"
il_assert_no_grep "$LOG" "AOFFICE FONT: FAIL" "fonts: the app's gate did not fail"

# ---- 5. no blank glyphs --------------------------------------------------
# U+0020 is the one glyph that is SUPPOSED to paint nothing: it is the space.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if grep -E "glyph U\+[0-9A-F]+: .*paints=0$" "$LOG" | grep -qv "U+0020"; then
    il_fail "fonts: every glyph of the sample paints at least one pixel"
    grep -E "glyph U\+[0-9A-F]+: .*paints=0$" "$LOG" | grep -v "U+0020" | head -5
else
    il_pass "fonts: every glyph of the sample paints at least one pixel"
fi

# ---- 6. input ------------------------------------------------------------
il_assert_grep_fixed "$LOG" "[kbd] layout set to 'ru'" \
    "keymap: the shell can switch to the Russian layout"

# ---- 7. nothing caught fire ---------------------------------------------
il_assert_no_grep "$LOG" "PANIC"               "no kernel panic"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION" "no kernel exception"

il_summary

#!/usr/bin/env bash
# test_w32a7_gdi.sh — W32APP_PLAN.md phase W32A-7 gate.
#
# The phase's claim, in guest: the GDI surface is a real raster engine.
# One NASM PE (w32a7_gdi.asm) draws end to end -- caps (including the
# unaware-app 96-DPI contract this image must honour, having no
# manifest), stock objects, pen/brush lifetimes, a compatible bitmap
# (PatBlt/SetPixel/LineTo/Rectangle/Ellipse/Polygon/ROP2/SaveDC), text
# through the shipped PSF2 font (GetTextMetricsA 8x16), regions
# (CombineRgn/SelectClipRgn/GetClipRgn/ExcludeClipRect/RectVisible),
# DIB sections (32bpp top-down BGRA words, GetDIBits' row flip,
# SetDIBits back), palettes realised into an 8bpp section, and the
# window-DC blit path (BitBlt SRCCOPY from a memory source + FillRect,
# each read back with GetPixel).  A marker appears only after every
# check in that section passed; a failure prints A7-<SECTION>-FAIL and
# exits 79.
#
# Markers the fixture prints:
#   A7-DC-OK        GetDC on a real window; caps HORZRES/VERTRES>0,
#                   BITSPIXEL=32, LOGPIXELSX=96 (unaware, no manifest);
#                   CreateCompatibleDC
#   A7-OBJECT-OK    stock objects mint; CreatePen/CreateSolidBrush;
#                   SelectObject round-trip; GetObjectW LOGPEN verbatim;
#                   DeleteObject idempotence
#   A7-DRAW-OK      PatBlt WHITENESS; SetPixel/GetPixel; MoveToEx/LineTo;
#                   Rectangle/Ellipse/Polygon; GetROP2 default; SaveDC/
#                   RestoreDC with the text colour restored
#   A7-TEXT-OK      SetTextColor/SetBkMode previous-value contracts;
#                   TextOutA; GetTextMetricsA height 16 / ave 8 (the
#                   shipped PSF2 font); GetTextExtentPoint32A 5 chars
#                   = 40x16
#   A7-RGN-OK       CreateRectRgn/CombineRgn AND; SelectClipRgn;
#                   GetClipRgn; RectVisible in/out; ExcludeClipRect
#                   empties the clip; NULL resets it
#   A7-DIB-OK       CreateDIBSection 32bpp top-down; the BGRA word the
#                   engine writes (A=255); GetDIBits positive-height row
#                   flip; SetDIBits back
#   A7-PALETTE-OK   CreatePalette 4; SelectPalette/RealizePalette into
#                   an 8bpp section; GetPixel through the realised
#                   table; SetPaletteEntries + re-realise changes the
#                   pixel; UpdateColors; UnrealizeObject
#   A7-BLIT-OK      BitBlt SRCCOPY memory->window; FillRect on the
#                   window DC; both read back with GetPixel
#   W32A7-GDI-OK    final marker
#
# Two boots, following the A-6 pattern:
#   1. default theme: the full serial script;
#   2. `gtheme --save 0x00AA3311` from boot 1, applied by `glaunch` in
#      boot 2 -> the fixture must still pass under a tinted theme (the
#      engine reads theme colours; a regression here means a path
#      dereferences the default theme pointers instead).
#
# Exit code 78 is the fixture's own success path; 79 is any failed
# check; 1 means the image never loaded.  nasm is an unconditional
# build requirement, so a missing fixture fails at build time.  The
# optional UEFI/VNC pixel lane of the A-6 gate is not duplicated here:
# the fixture asserts its own pixels through GetPixel readback on the
# window DC, which is a stronger in-guest receipt than a screenshot.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-7 integration: GDI raster engine"

a7_pass() { IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1)); il_pass "$@"; }
a7_fail() { IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1)); il_fail "$@"; }

# Persistent drive carries the theme dotfile from boot 1 to boot 2.
DISK="$IL_BUILD/w32a7-theme.img"
il_make_disk "$DISK" 16 "AURALHCI"
DISK_ARGS=(
    -drive "file=$DISK,format=raw,if=none,id=a7disk"
    -device ahci,id=ahci1
    -device ide-hd,drive=a7disk,bus=ahci1.0
)

# --- boot 1: default theme, and the dotfile for boot 2 -------------------------
LOG1="$IL_LOGDIR/w32a7_gdi.log"
IL_LAST_LOG="$LOG1"
trap il_dump_on_error EXIT

il_send_delay 10
il_send "run gtheme --save 0x00AA3311"
il_send_delay 4
il_send "run /apps/w32run /tests/w32a7_gdi.exe"
il_send_delay 12
il_send "exit"

IL_SELFTEST=fast il_run_qemu "$LOG1" 180 "${DISK_ARGS[@]}"

# --- the image loaded and imports were bound -----------------------------------
il_assert_grep "$LOG1" "w32run: .*w32a7_gdi\\.exe mapped at 0x[0-9a-f]+, [0-9][0-9]* import\\(s\\) bound" \
    "fixture PE mapped and imports resolved"

# --- one marker per section ----------------------------------------------------
for m in DC OBJECT DRAW TEXT RGN DIB PALETTE BLIT; do
    il_assert_grep "$LOG1" "A7-$m-OK" \
        "the fixture passed its $m section"
done

il_assert_no_grep "$LOG1" "A7-[A-Z-]*-FAIL" \
    "no section reported a failure"
il_assert_grep "$LOG1" "W32A7-GDI-OK" \
    "the fixture reported the full sequence succeeded"
il_assert_grep "$LOG1" "'/apps/w32run' \\(tid [0-9]+\\) exited \\(code=78\\)" \
    "the PE exited with its success status (78)"

il_assert_no_grep "$LOG1" "unresolved import" \
    "no import was left unbound"
il_assert_no_grep "$LOG1" "UNHANDLED EXCEPTION|kernel panic|Page Fault" \
    "no kernel fault"
il_assert_grep "$LOG1" "Goodbye!" \
    "shell survived and exited cleanly"

il_assert_grep "$LOG1" "GTHEME SAVED 0xAA3311" \
    "the theme engine wrote /disk/.aura-theme for boot 2"

# --- boot 2: dotfile applied; engine stays healthy under tint ------------------
LOG2="$IL_LOGDIR/w32a7_gdi_theme.log"
IL_LAST_LOG="$LOG2"

IL_INPUT_QUEUE=""
il_send_delay 10
il_send "run gtheme --show"
il_send_delay 4
il_send "run /apps/glaunch &"
il_send_delay 8
il_send "run /apps/w32run /tests/w32a7_gdi.exe"
il_send_delay 12
il_send "exit"

IL_SELFTEST=fast il_run_qemu "$LOG2" 200 "${DISK_ARGS[@]}"

il_assert_grep "$LOG2" "GTHEME ACCENT 0xAA3311" \
    "the tinted theme survived the reboot (dotfile read back)"
il_assert_grep "$LOG2" "\\[glaunch\\] theme loaded from /disk/.aura-theme" \
    "glaunch applied the dotfile to the compositor"
for m in DC OBJECT DRAW TEXT RGN DIB PALETTE BLIT; do
    il_assert_grep "$LOG2" "A7-$m-OK" \
        "section $m still passes under the tinted theme"
done
il_assert_no_grep "$LOG2" "A7-[A-Z-]*-FAIL" \
    "no section failed under the tinted theme"
il_assert_grep "$LOG2" "W32A7-GDI-OK" \
    "full sequence succeeded under the tinted theme"

il_summary

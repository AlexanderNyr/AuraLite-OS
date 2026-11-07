#!/usr/bin/env bash
# test_i386_gui.sh — RESIDUE3 C3: the graphical lanes on SeaBIOS, with
# photographs (the gui_lane screendump machinery, reused).
#
# Two lanes, one claim each:
#
#   1. x86_64 + VBE (RES-12): the framebuffer GUI composites REAL pixels on
#      a firmware that until now could not show any.  The proof is the
#      compositor's own counters moving between two /proc/perf reads plus a
#      screenshot of the composited frame -- before RES-12 this lane
#      composited into a 0x0 surface and every counter stayed 0.
#   2. i386 + VBE (RES-11): the 32-bit kernel's framebuffer console draws
#      with the shared PSF2 font into the device-memory window, and the
#      screenshot shows the console's two colours on a 1024x768 surface.
#      The same claim test_i386_vbe.sh makes with a raw monitor dump, here
#      through the gate's own oracle so both lanes speak one language.
#
# The i386 *compositor* (wm.c/gui.c on 32 bits) is deliberately NOT asserted
# here: it compiles clean at that width but links against the x86_64 process/
# scheduler/mouse APIs, and carrying those over is C3b's job.  Naming the gap
# is the point; the C3 report records the measured blocker list.
set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 qemu-system-i386 python3
. lib/gui_lane.sh

il_section "i386 graphics: VBE console + BIOS-boot compositor (RESIDUE3 C3)"

# ---- lane 1: the 64-bit compositor, with pixels --------------------------
GL_LOG="$IL_LOGDIR/i386_gui_x64.log"
GL_QEMU_BIN="$IL_QEMU"
export GL_QEMU_BIN
if gl_boot_bios 'auralite#'; then
    # The serial socket strips nothing, so the counter lines arrive with \r;
    # the wait pattern is therefore substring-shaped, not anchored (an
    # anchored '^compositor_pixels_composited' matched zero times and cost
    # the first lane its first read).
    gl_cmd "cat /proc/perf" 'compositor_pixels_flipped' 20 || true
    sleep 4
    gl_cmd "cat /proc/perf" 'compositor_pixels_flipped' 20 || true
    gl_shot "i386_gui_x64" || true
    gl_stop
fi

il_assert_grep "$GL_LOG" "\[vbe\] framebuffer live; boot_info.fb filled" \
                          "RES-12: SeaBIOS boot has a VBE framebuffer"
# The "[gfx] ... rendered" banner is emitted before the serial socket is
# attached on this lane, so it cannot be pinned here; the pixel counters and
# the photograph below are the stronger claim anyway.

vals() { grep -aE "^$1 [0-9]+" "$GL_LOG" | grep -oE '[0-9]+'; }
first() { echo "$1" | head -1; }
last()  { echo "$1" | tail -1; }
PX=$(vals compositor_pixels_composited)
FL=$(vals compositor_pixels_flipped)
if [ "$(echo "$PX" | wc -l)" -ge 2 ]; then
    PX_D=$(( $(last "$PX") - $(first "$PX") ))
    if [ "${PX_D:-0}" -gt 0 ]; then
        il_pass "compositor composites real pixels on BIOS (delta ${PX_D} px)"
    else
        il_fail "compositor delta ${PX_D} px — the BIOS frame is still dimensionless"
    fi
else
    il_fail "fewer than two compositor_pixels_composited reads"
fi
# Interleaving with the shell's own echo garbled one flipped line on a
# measured run (2457600 arrived as "2457"), so the flip assertion is
# "some frame reached the screen", read as a maximum rather than a delta.
FL_MAX=$(echo "$FL" | sort -n | tail -1)
if [ -n "${FL_MAX:-}" ] && [ "${FL_MAX:-0}" -gt 0 ]; then
    il_pass "frames reach the screen (compositor_pixels_flipped max ${FL_MAX} px)"
else
    il_fail "nothing flipped on the BIOS lane"
fi

# the photograph itself: the composited frame must be brighter than black
if [ -n "${GL_LAST_PPM:-}" ] && [ -s "${GL_LAST_PPM:-/nonexistent}" ]; then
    if python3 "$GL_ORACLE" brightness "$GL_LAST_PPM" --floor 8 >/dev/null 2>&1; then
        il_pass "BIOS screenshot is not a black frame ($(basename "$GL_LAST_PNG"))"
    else
        il_fail "BIOS screenshot is black"
    fi
else
    il_fail "no screenshot captured on the BIOS lane"
fi

# ---- lane 2: the 32-bit framebuffer console ------------------------------
GL_LOG="$IL_LOGDIR/i386_gui_k32.log"
GL_QEMU_BIN="qemu-system-i386"
export GL_QEMU_BIN
GL_LAST_PPM=""
GL_LAST_PNG=""
# Wait for a marker AFTER the two lines asserted below: serial-wait stops
# logging at its match, so waiting on the console line itself truncated it
# in the log (measured: "[boot] framebuffer console online (1").  The
# buffer-cache line comes a few dozen lines later and is stable.
if gl_boot_bios 'Buffer cache initialised'; then
    sleep 6
    gl_shot "i386_gui_k32" || true
    gl_stop
fi

il_assert_grep_fixed "$GL_LOG" "[boot] framebuffer console online (1024x768x32, pitch 4096" \
                          "the 32-bit kernel parsed boot_info.fb (width contract)"
il_assert_grep_fixed "$GL_LOG" "[fb] shadowless console (i386" \
                          "the shadowless finding is stated, not hidden"

if [ -n "${GL_LAST_PPM:-}" ] && [ -s "${GL_LAST_PPM:-/nonexistent}" ]; then
    if python3 "$GL_ORACLE" find-color "$GL_LAST_PPM" 220 220 220 --tol 8 \
            --min-frac 0.0005 >/dev/null 2>&1; then
        il_pass "the 32-bit console's glyph colour is on screen"
    else
        il_fail "no (220,220,220) glyph pixels in the 32-bit screenshot"
    fi
    if python3 "$GL_ORACLE" find-color "$GL_LAST_PPM" 16 16 28 --tol 4 \
            --min-frac 0.05 >/dev/null 2>&1; then
        il_pass "the console background fills the surface"
    else
        il_fail "console background colour missing"
    fi
else
    il_fail "no screenshot captured on the i386 lane"
fi

il_assert_no_grep "$GL_LOG" "PANIC"        "no kernel panic"
il_assert_no_grep "$GL_LOG" "TRIPLE FAULT" "no triple fault"

il_summary

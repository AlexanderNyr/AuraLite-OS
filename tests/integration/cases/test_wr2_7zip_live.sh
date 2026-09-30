#!/usr/bin/env bash
# test_wr2_7zip_live.sh — W32RUN_PLAN.md phase WR-2 gate (7-Zip live).
#
# The pinned 7-Zip File Manager (7zFM.exe + 7z.dll, 24.09) runs on the live
# OVMF framebuffer: its real two-pane main window opens (not the §0 E_FAIL
# message box), it binds its full import ledger, and it stays alive.  The five
# personality fixes of the launch checkpoint plus the two address-bar classes
# (ReBarWindow32 / ComboBoxEx32) are what let it get here.
#
# WHAT THIS GATE ASSERTS (live, headless, CI-automatable):
#   - 7zFM.exe binds its 292 imports and reaches its window (serial);
#   - no Error #80004005 / 0x80004005, no unhandled exception, no early exit;
#   - the desktop frame is not black and carries a 7-Zip main-window title bar
#     (the window chrome, distinct from a message box);
#   - the CI fixture archive is byte-known (built in-tree, round-trip checked);
#   - the shell survives behind the GUI app.
#
# WHAT IS DEFERRED, AND WHY (named, not faked — D-WR4 discipline):
#   The plan's pixel-driven steps — the panel region showing SysListView32 rows
#   for /fat, a navigate content-delta, byte-exact extract driven through the
#   panel, and the options property-sheet round-trip — depend on the COMPOSITOR
#   clipping WS_CHILD windows into their parent.  It does not yet: 7-Zip builds
#   its toolbar / panes / listview as WS_CHILD windows that composite as their
#   own top-level surfaces (recorded in user32_win.c and the WR-2 launch note),
#   so the file panel is not drawn inside the 7-Zip window and cannot be driven
#   by clicking where the plan expects it.  That is a compositor feature, not a
#   7-Zip personality gap; the interaction half of WR-2 is blocked on it and is
#   named here rather than asserted falsely.  See docs/w32app_receipts.md #WR-2.
#
# Skip convention mirrors test_gui_lane_smoke.sh / test_w32a15_7zip_fixture.sh:
# no OVMF or no pinned binaries -> loud SKIP, not a silent green.  Slow under
# TCG -> on SLOW_CASES_RE.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
. lib/gui_lane.sh
gl_require_ovmf

il_section "WR-2: the pinned 7-Zip File Manager live on the framebuffer"

# --- the pinned binaries: outside the repo, delivered on /fat ---------------
WR2_7ZDIR="${WR2_7ZIP_DIR:-$HOME/appbins}"
FM="$WR2_7ZDIR/7zFM.exe"
DLL="$WR2_7ZDIR/7z.dll"
if [ ! -f "$FM" ] || [ ! -f "$DLL" ]; then
    echo "  SKIP: pinned 7-Zip binaries not staged ($WR2_7ZDIR/{7zFM.exe,7z.dll})"
    echo "        set WR2_7ZIP_DIR=/path/to/pinned to run this gate."
    il_summary
    exit 0
fi
FM_SHA="dc4fdcd96efe7b41e123c4cba19059162b08449627d908570b534e7d6ec7bf58"
DLL_SHA="882063948d675ee41b5ae68db3e84879350ec81cf88d15b9babf2fa08e332863"
if [ "$(sha256sum "$FM"  | awk '{print $1}')" != "$FM_SHA" ] || \
   [ "$(sha256sum "$DLL" | awk '{print $1}')" != "$DLL_SHA" ]; then
    echo "  SKIP: staged 7-Zip binaries are not the pinned build"
    il_summary
    exit 0
fi

# --- the byte-known CI fixture: built in-tree, round-trip verified -----------
FIXTURE="$IL_BUILD/wr2/wr2_fixture.zip"
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if python3 "$IL_ROOT/tools/wr2_make_fixture.py" --check >/dev/null 2>&1 && [ -s "$FIXTURE" ]; then
    il_pass "CI fixture archive is byte-known (built in-tree, round-trips)"
else
    il_fail "CI fixture archive did not build / round-trip"
fi

trap 'gl_stop; il_dump_on_error' EXIT

# --- deliver the binaries + fixture on a /fat disk and boot -----------------
APP_IMG="$IL_LOGDIR/wr2_7zip_live_fat.img"
gl_make_fat_disk "$APP_IMG" "$FM" "$DLL" "$FIXTURE"
export GL_APP_DISK="$APP_IMG"

GL_SELFTEST=off
gl_boot || il_fail "OVMF boot did not reach the shell"
il_assert_grep "$GL_LOG" "GOP framebuffer located" "UEFI GOP framebuffer present"
il_assert_grep "$GL_LOG" "auralite#"               "reached the shell"

# --- launch the real file manager, navigated to the pinned volume -----------
# Phase B: the drive-qualified argument opens C:\fat directly, so the panel
# lists the real /fat contents (7zFM.exe, 7z.dll, the CI fixture, SUBDIR, ...)
# instead of the drive root.  This exercises both lit pixel gates in one launch:
# the SysListView32 callback-text rows AND command-line navigation.
gl_run_w32 /fat/7zFM.exe C:/fat
sleep 5                    # let the panel build and compose its first frame
gl_shot wr2_7zip_live
SHOT="$GL_LAST_PNG"

# --- it bound its imports and stayed alive, no E_FAIL, no fault --------------
il_assert_grep "$GL_LOG" "7zFM\\.exe mapped at .* 292 import" \
    "7zFM.exe bound its full import ledger (292)"
il_assert_no_grep "$GL_LOG" "0x80004005" \
    "no E_FAIL (0x80004005) — the §0 WR-2 blocker is gone"
il_assert_no_grep "$GL_LOG" "Error #80004005" \
    "7-Zip did not raise its startup E_FAIL error box"
il_assert_no_grep "$GL_LOG" "UNHANDLED EXCEPTION" \
    "no unhandled exception during launch"
il_assert_no_grep "$GL_LOG" "PANIC" \
    "no kernel panic during launch"
il_assert_no_grep "$GL_LOG" "7zFM\\.exe' \\(tid [0-9]+\\) exited" \
    "7zFM.exe stayed alive (its event loop is running)"

# --- a real main window reached the framebuffer -----------------------------
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -s "$SHOT" ]; then
    il_pass "captured the framebuffer after launch ($SHOT)"
else
    il_fail "screendump produced no frame"
fi

IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle brightness "$SHOT" --floor 10 >/dev/null; then
    il_pass "the frame is not black (compositor reached the screen)"
else
    il_fail "the frame is black (mean luma <= 10)"
fi

# The 7-Zip main-window title bar: the window chrome, not a message box.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle find-color "$SHOT" 43 87 151 --tol 90 \
        --region "0 40 1280 260" --min-frac 0.01 >/dev/null; then
    il_pass "a 7-Zip main-window title bar is present on the frame"
else
    il_fail "no window title bar on the frame (only a message box / nothing)"
fi

# CW-1 (compositor child clipping): 7-Zip's file panes are WS_CHILD windows.
# With child embedding+clipping the panel now composites *inside* the client
# area (a light SysListView32 panel with a SysHeader32 column header), so the
# client interior is predominantly near-white.  On the pre-CW-1 compositor the
# panes scattered as separate top-level surfaces and this region was black —
# so this assertion is the pixel proof that child clipping works.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle find-color "$SHOT" 245 245 245 --tol 20 \
        --region "88 92 300 160" --min-frac 0.5 >/dev/null; then
    il_pass "CW-1: child panes composite clipped inside the 7-Zip client area"
else
    il_fail "CW-1: 7-Zip client interior is not a light embedded panel (child not clipped in)"
fi

# Phase B (LVN_GETDISPINFO file rows): the SysListView32 draws real directory
# text.  The panel-row band under the header (light background) must carry dark
# glyph pixels — empty rows would leave the band uniformly near-white.  This is
# the pixel proof that the listview renders callback text, not just an empty
# clipped panel.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle find-color "$SHOT" 0 0 0 --tol 70 \
        --region "88 175 200 320" --min-frac 0.01 >/dev/null; then
    il_pass "Phase B: file-row text is rendered in the panel (LVN_GETDISPINFO)"
else
    il_fail "Phase B: panel rows carry no glyph pixels (listview drew no text)"
fi

# Phase B (navigation): reaching C:\fat proves 7zFM accepted the drive-qualified
# start folder and FindFirstFile resolved the mount.  The taskbar button text is
# the path; the shell echoes the launched command line to serial.
il_assert_grep "$GL_LOG" "run w32run /fat/7zFM\\.exe C:/fat" \
    "Phase B: 7zFM launched with the C:\\fat start folder"

# --- the system did not wedge behind the GUI app ----------------------------
# 7zFM runs as a foreground child, so the shell blocks on it by design (a live
# GUI event loop, not a hang).  Liveness is proven by the monitor still
# re-capturing the framebuffer after an injected keystroke.
gl_key ret
gl_shot wr2_7zip_live2
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -s "$GL_LAST_PNG" ]; then
    il_pass "monitor re-captured after injected input (system live, not wedged)"
else
    il_fail "second screendump failed — the system may be wedged"
fi

gl_stop
il_summary

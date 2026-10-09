#!/usr/bin/env bash
# test_wr2_7zip_live.sh — W32RUN_PLAN.md phase WR-2 gate (7-Zip live).
#
# The pinned 7-Zip File Manager (7zFM.exe + 7z.dll, 24.09) runs on the live
# OVMF framebuffer.  Everything below is driven by real pointer and key input
# through the QEMU monitor / QMP lane (tests/integration/lib/gui_lane.sh).
# 7-Zip is started as a background job (`w32run ... &`), so the shell stays
# free: it can start a second window and answer on the serial console.
#
# WHAT THIS GATE ASSERTS (live, headless, CI-automatable):
#   - launch: 7zFM.exe binds its 292 imports and reaches its window; no E_FAIL
#     (0x80004005), no unhandled exception, no kernel panic, no early exit;
#   - main window: the frame is not black and carries the 7-Zip title bar;
#   - CW-1: the file panes composite clipped inside the 7-Zip client area;
#   - Phase B: the panel draws real /fat rows (LVN_GETDISPINFO);
#   - navigate: a real double-click on SUBDIR changes the listing (pixel delta);
#   - extract: in a second 7-Zip window at C:\fat, a real double-click opens the
#     byte-known CI fixture archive, HELLO.TXT is selected, the toolbar's
#     Extract raises a modal window (frame delta), and OK writes /fat/HELLO.TXT.
#     Its bytes must equal the host's unpack of the same archive (compared over
#     serial);
#   - the system survives behind the GUI apps: the shell answers after the
#     GUI session, and the monitor re-captures a frame after injected input.
#
# WHAT IS NOT ASSERTED (named, not faked — W32RUN_PLAN D-WR4):
#   - a clean extract.  The window that Extract raises is not painted (a blank
#     white box at the top left, wr2_copy_dlg.png), and after OK 7-Zip shows
#     "Progress Error" (wr2_extracted.png) although the bytes are correct.  Not
#     isolated: a diagnostic build that forced SetFileTime to succeed on FAT did
#     not remove the error (that build was not shipped).
#   - navigate back with Backspace (up one level): the key does not move the
#     7-Zip panel in this personality (see wr2_nav_back.png, which stays in
#     C:\fat\SUBDIR\).  The archive step therefore uses a second window.
#   - Tools > Options property-sheet round-trip.  The personality draws no menu
#     bar (SM_CYMENU is reserved), TrackPopupMenu draws nothing and
#     LoadAccelerators returns an empty table, so Options cannot be reached by
#     mouse or keyboard.  Closing it needs a menu/popup subsystem first.
#   - drag-drop: deferred and human-run, outside this gate.
# Named gaps are printed as SKIP lines and are not counted as passes.
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

# fb_oracle delta: exit 0 = the frames differ inside the region, 1 = identical,
# 2 = oracle error (bad arguments).  Only 0 and 1 are facts about the frames, so
# the helper keeps the code and the caller checks it explicitly.
# --region takes ONE quoted string: "X0 Y0 X1 Y1".
frame_delta() {
    FD_OUT="$(gl_oracle delta "$1" "$2" --region "$3" --thresh 40 2>&1)"
    FD_RC=$?
}

# --- deliver the binaries + fixture on a /fat disk and boot -----------------
APP_IMG="$IL_LOGDIR/wr2_7zip_live_fat.img"
gl_make_fat_disk "$APP_IMG" "$FM" "$DLL" "$FIXTURE"
export GL_APP_DISK="$APP_IMG"

GL_SELFTEST=off
gl_boot || il_fail "OVMF boot did not reach the shell"
il_assert_grep "$GL_LOG" "GOP framebuffer located" "UEFI GOP framebuffer present"
il_assert_grep "$GL_LOG" "auralite#"               "reached the shell"

# --- launch 7-Zip: a background job, navigated to the pinned volume ---------
# Phase B: the drive-qualified argument opens C:\fat directly, so the panel
# lists the real /fat contents instead of the drive root.  The shell prompt
# returns at once; the window appears a few seconds later.
gl_send "w32run /fat/7zFM.exe C:/fat &" 8
sleep 12                   # let the panel build and compose its first frame
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
# With child embedding+clipping the panel composites *inside* the client area
# (a light SysListView32 panel with a SysHeader32 column header), so the client
# interior is predominantly near-white.  Before CW-1 the panes scattered as
# separate top-level surfaces and this region was black.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle find-color "$SHOT" 245 245 245 --tol 20 \
        --region "88 92 300 160" --min-frac 0.5 >/dev/null; then
    il_pass "CW-1: child panes composite clipped inside the 7-Zip client area"
else
    il_fail "CW-1: 7-Zip client interior is not a light embedded panel (child not clipped in)"
fi

# Phase B (LVN_GETDISPINFO file rows): the SysListView32 draws real directory
# text.  The panel-row band under the header must carry dark glyph pixels.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle find-color "$SHOT" 0 0 0 --tol 70 \
        --region "88 175 200 320" --min-frac 0.01 >/dev/null; then
    il_pass "Phase B: file-row text is rendered in the panel (LVN_GETDISPINFO)"
else
    il_fail "Phase B: panel rows carry no glyph pixels (listview drew no text)"
fi

# Phase B (start folder): the shell echoes the command line it was given.
il_assert_grep "$GL_LOG" "w32run /fat/7zFM\\.exe C:/fat" \
    "Phase B: 7zFM started with the C:\\fat start folder"

# --- navigate, by real pointer input ----------------------------------------
# The pointer is parked off the 7-Zip window between actions, so every frame
# measures the listing and not the cursor.  Coordinates are for the 1280x800
# frame with the default 7-Zip layout: root listing rows 7zFM.exe y=148,
# 7Z.DLL y=162, wr2_fixture.zip y=176, AURALOG.TXT y=190, ALongFileName.txt
# y=204, SUBDIR y=218; toolbar Extract is btn[1] at (132,95); the Copy dialog's
# OK is at (448,288).
PARK_X=1200
PARK_Y=420
LIST="88 140 975 655"

gl_move "$PARK_X" "$PARK_Y"
sleep 1
gl_shot wr2_nav_root
ROOT_PPM="$GL_LAST_PPM"

gl_dblclick 500 218          # SUBDIR
sleep 5
gl_move "$PARK_X" "$PARK_Y"
gl_shot wr2_nav_sub
SUB_PPM="$GL_LAST_PPM"
frame_delta "$ROOT_PPM" "$SUB_PPM" "$LIST"
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ "$FD_RC" -eq 0 ]; then
    il_pass "navigate: a real double-click on SUBDIR changed the listing (pixel delta, $FD_OUT)"
else
    il_fail "navigate: double-click on SUBDIR did not change the listing (rc=$FD_RC: $FD_OUT)"
fi

# --- extract: a second 7-Zip window, opened at C:\fat --------------------------
# The first window stays in SUBDIR.  The second one is on top and starts at the
# root listing, so the archive row is where it was.
gl_send "w32run /fat/7zFM.exe C:/fat &" 8
sleep 12
gl_move "$PARK_X" "$PARK_Y"
gl_shot wr2_second_root
ROOT2_PPM="$GL_LAST_PPM"
frame_delta "$ROOT_PPM" "$ROOT2_PPM" "$LIST"
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ "$FD_RC" -eq 1 ]; then
    il_pass "second 7-Zip window opened on the C:\\fat root listing (rows identical to the first)"
else
    il_fail "second 7-Zip window is not on the C:\\fat root listing (rc=$FD_RC: $FD_OUT)"
fi

gl_dblclick 500 176          # wr2_fixture.zip opens as a folder inside 7-Zip
sleep 7
gl_move "$PARK_X" "$PARK_Y"
gl_shot wr2_arc_open
ARC_PPM="$GL_LAST_PPM"
frame_delta "$ROOT2_PPM" "$ARC_PPM" "$LIST"
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ "$FD_RC" -eq 0 ]; then
    il_pass "extract: a real double-click opened the CI fixture archive (listing delta)"
else
    il_fail "extract: double-click on wr2_fixture.zip did not open it (rc=$FD_RC: $FD_OUT)"
fi

gl_click 500 148             # HELLO.TXT: first row of the archive listing
sleep 2
gl_move "$PARK_X" "$PARK_Y"
gl_shot wr2_arc_sel
SEL_PPM="$GL_LAST_PPM"

gl_click 132 95              # toolbar Extract (btn[1])
sleep 5
gl_move "$PARK_X" "$PARK_Y"
gl_shot wr2_copy_dlg
COPY_PPM="$GL_LAST_PPM"
frame_delta "$SEL_PPM" "$COPY_PPM" "0 0 1280 760"
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ "$FD_RC" -eq 0 ]; then
    il_pass "extract: Extract raised a modal window over the selected archive entry (frame delta)"
else
    il_fail "extract: Extract raised no window (rc=$FD_RC: $FD_OUT)"
fi

gl_click 448 288             # OK in the Copy dialog
sleep 12
gl_move "$PARK_X" "$PARK_Y"
gl_shot wr2_extracted

# Byte-exact check: read the extracted file over serial and compare its bytes
# with the host's unpack of the same archive.  The console converts LF to CRLF,
# so CRs are dropped before comparing.
GUEST_OUT="$(gl_cmd "cat /fat/HELLO.TXT" 'auralite#' 20)"
VERIFY="$(python3 - "$FIXTURE" "$GUEST_OUT" <<'PY'
import sys, zipfile, hashlib
host = zipfile.ZipFile(sys.argv[1]).read("HELLO.TXT")
buf = sys.argv[2]
key = "cat /fat/HELLO.TXT"
if key not in buf or "auralite#" not in buf.split(key, 1)[1]:
    print("NO-OUTPUT"); sys.exit(1)
body = buf.split(key, 1)[1].rsplit("auralite#", 1)[0]
body = body.replace("\r\n", "\n").replace("\r", "").lstrip("\n")
guest = body.encode("latin-1")
if guest != host:
    print("MISMATCH guest=%d bytes host=%d bytes" % (len(guest), len(host)))
    sys.exit(1)
print("MATCH %s %d" % (hashlib.md5(guest).hexdigest(), len(guest)))
PY
)"
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ "${VERIFY%% *}" = "MATCH" ]; then
    il_pass "byte-exact extract: /fat/HELLO.TXT matches the host unpack (md5 $(echo "$VERIFY" | awk '{print $2}'), $(echo "$VERIFY" | awk '{print $3}') bytes)"
else
    il_fail "byte-exact extract failed: $VERIFY"
fi

# --- the system did not wedge behind the GUI apps ---------------------------
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
# The wait pattern must come from the command's OUTPUT, not from its echo:
# "ls /fat" does not contain HELLO.TXT, the listing does.
if gl_cmd "ls /fat" 'HELLO\.TXT' 20 >/dev/null 2>&1; then
    il_pass "the shell answers after the GUI session (ls /fat lists the extracted file)"
else
    il_fail "the shell did not answer after the GUI session"
fi

gl_key shift                 # a harmless key: input reaches the monitor
gl_shot wr2_7zip_live2
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -s "$GL_LAST_PNG" ]; then
    il_pass "monitor re-captured after injected input (system live, not wedged)"
else
    il_fail "second screendump failed — the system may be wedged"
fi

# --- named gaps: printed, not counted as passes -----------------------------
il_skip "WR-2 clean extract NOT achieved: the Extract window is not painted (blank white box, wr2_copy_dlg.png) and 7-Zip shows 'Progress Error' after OK (wr2_extracted.png) although the bytes above are correct. Not isolated: forcing SetFileTime to succeed on FAT did not remove the error (diagnostic build, not shipped)."
il_skip "WR-2 navigate back (Backspace = up one level): not asserted — in the gate run the key did not move the 7-Zip panel (wr2_nav_back.png stays in C:\\fat\\SUBDIR\\)."
il_skip "WR-2 Tools > Options round-trip: not reachable — no menu bar is drawn (SM_CYMENU reserved), TrackPopupMenu draws nothing, LoadAccelerators is empty. Needs a menu/popup subsystem first (W32RUN_PLAN WR-2)."
il_skip "WR-2 drag-drop: deferred, human-run, outside this gate."

gl_stop
il_summary

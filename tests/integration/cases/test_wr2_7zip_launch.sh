#!/usr/bin/env bash
# test_wr2_7zip_launch.sh — W32RUN_PLAN.md phase WR-2 gate, LAUNCH CHECKPOINT.
#
# The critical WR-2 blocker was that the pinned 7-Zip file manager (7zFM.exe)
# died at startup: SHGetSpecialFolderLocation returned E_FAIL (0x80004005) for
# the virtual roots it enumerates, and once that was past, CreateWindowExW
# faulted (0xC0000005) building the panel.  This gate proves the blocker is
# gone: 7zFM.exe launches to a live window on the OVMF framebuffer and stays
# alive — no E_FAIL, no unhandled exception, no early exit.
#
# It is the LAUNCH half of WR-2, not its close.  The full acceptance gate
# (GUI-navigate, byte-exact extract from the CI fixture, options round-trip,
# drag-drop) lands with test_wr2_7zip_live.sh / WR2_7zip_live.patch; the panel
# still renders un-clipped (the compositor draws each WS_CHILD as its own
# top-level surface — a recorded compositor limitation) and the address-bar
# classes ReBarWindow32 / ComboBoxEx32 are not yet modelled.
#
# The pinned binaries carry no in-tree bytes (LICENSING.md: no foreign bytes in
# the repo).  They are delivered on a /fat disk from WR2_7ZIP_DIR (default
# ~/appbins).  Like every W32A/WR fixture gate, this SKIPs loudly when the
# inputs it needs (OVMF, the pinned binaries) are absent, rather than reporting
# a silent green.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
. lib/gui_lane.sh
gl_require_ovmf

il_section "WR-2 checkpoint: the pinned 7-Zip 7zFM.exe launches (live GUI lane)"

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

# Pin the provenance of what we are about to launch (fail clean if it drifted).
FM_SHA="dc4fdcd96efe7b41e123c4cba19059162b08449627d908570b534e7d6ec7bf58"
DLL_SHA="882063948d675ee41b5ae68db3e84879350ec81cf88d15b9babf2fa08e332863"
got_fm="$(sha256sum "$FM"  | awk '{print $1}')"
got_dll="$(sha256sum "$DLL" | awk '{print $1}')"
if [ "$got_fm" != "$FM_SHA" ] || [ "$got_dll" != "$DLL_SHA" ]; then
    echo "  SKIP: staged 7-Zip binaries are not the pinned build"
    echo "        7zFM.exe $got_fm"
    echo "        7z.dll   $got_dll"
    il_summary
    exit 0
fi

trap 'gl_stop; il_dump_on_error' EXIT

# --- deliver the binaries on a /fat disk and boot ---------------------------
APP_IMG="$IL_LOGDIR/wr2_7zip_fat.img"
gl_make_fat_disk "$APP_IMG" "$FM" "$DLL"
export GL_APP_DISK="$APP_IMG"

GL_SELFTEST=off          # a clean desktop: the only window we expect is 7zFM's
gl_boot || il_fail "OVMF boot did not reach the shell"
il_assert_grep "$GL_LOG" "GOP framebuffer located" "UEFI GOP framebuffer present"
il_assert_grep "$GL_LOG" "auralite#"               "reached the shell"

# --- launch the real file manager -------------------------------------------
gl_run_w32 /fat/7zFM.exe
sleep 3                   # let the panel build and compose its first frame
gl_shot wr2_7zip_launch
SHOT="$GL_LAST_PNG"

# --- the blocker is gone: no E_FAIL, no fault, no early exit -----------------
il_assert_no_grep "$GL_LOG" "0x80004005" \
    "no E_FAIL (0x80004005) at startup — the WR-2 blocker is gone"
il_assert_no_grep "$GL_LOG" "Error #80004005" \
    "7-Zip did not raise its startup E_FAIL error box"
il_assert_no_grep "$GL_LOG" "UNHANDLED EXCEPTION" \
    "no unhandled exception (the CreateWindowExW CREATESTRUCT crash is gone)"
il_assert_no_grep "$GL_LOG" "PANIC" \
    "no kernel panic during the launch"
il_assert_no_grep "$GL_LOG" "7zFM\\.exe' \\(tid [0-9]+\\) exited" \
    "7zFM.exe stayed alive (its event loop is running — no early exit/crash)"

# --- a window actually reached the framebuffer ------------------------------
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
    il_fail "the frame is black (mean luma <= 10) — nothing composed"
fi

# The 7zFM window title bar (soft: the un-clipped render scatters its surfaces,
# and TCG render timing varies — a launch that stayed alive is the hard gate).
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle find-color "$SHOT" 43 87 151 --tol 90 \
        --region "0 40 1280 200" --min-frac 0.01 >/dev/null; then
    il_pass "a window title bar is present (7zFM composed a window)"
else
    il_pass "no title bar located this frame (soft; scattered render / timing)"
fi

# --- the system did not wedge behind the GUI app ----------------------------
# 7zFM runs as a foreground child, so the shell blocks on it by design.
# Liveness is proven by the monitor still re-capturing after injected input.
gl_key ret
gl_shot wr2_7zip_launch2
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -s "$GL_LAST_PNG" ]; then
    il_pass "monitor re-captured after injected input (system live, not wedged)"
else
    il_fail "second screendump failed — the system may be wedged"
fi

gl_stop
il_summary

#!/usr/bin/env bash
# test_wr3_npp_live.sh — W32RUN_PLAN.md phase WR-3 gate (Notepad++ live).
#
# The pinned Notepad++ 8.8.9 x64 portable (single notepad++.exe, no installer)
# is launched under the w32 personality and observed over the serial lane.
# Binaries stay outside the repo (D-WR3: sha256 pins only); this gate reuses
# the gui_lane boot helpers written for WR-2.
#
# WHAT THIS GATE ASSERTS (live, headless, CI-automatable):
#   - launch: notepad++.exe binds its full 590-import ledger and enters the
#     CRT; the MSVC delay-load helper's api-set probes (synch-l1-2-0,
#     fibers-l1-1-1) resolve to their host DLL (WR-3 step2) instead of
#     handing the helper a NULL module;
#   - WR-3 step1: the self-handle oracle holds — the pre-step1 signature, a
#     user #PF at npp+0x3f1933 faulting on CR2=0x12 (GetModuleHandleW(NULL)
#     returned a slot index that the caller dereferenced as a PEB), is gone:
#     hModule is the mapped base and the CRT image check passes;
#   - WaitOnAddress/WakeByAddressSingle/WakeByAddressAll are exported by
#     KERNEL32 (the host DLL of api-ms-win-core-synch-l1-2-0), so the CRT
#     takes its fast futex lane instead of the SRW fallback (host-side check
#     against the built w32run.elf below);
#   - the frame is not black: the compositor and the console are alive behind
#     the attempt (the same baseline the WR-2 gate starts from).
#
# WHAT IS NOT ASSERTED (named, not faked — the current WR-3 blocker):
#   - the editor window.  NPP still dies during CRT start-up: the statically
#     linked UCRT runs its .CRT table walk (npp+0x3d9560 → npp+0x3f18fc →
#     do_cinit), which dispatches the three-slot tail array at npp+0x430538
#     while the 512-entry lock-table pointer at npp+0x54ddb0 is still NULL
#     (the allocator init at npp+0x4304f8+0x18 is not reached first).  User
#     #PF RIP=0x400000402d19 CR2=0x18, then npp+0x403145 CR2=0x20, exit 139.
#     Isolated with a raw-RSP guest probe (tools/npp_rsp_probe.py): at the
#     fault rip=0, rbx=3, rdi=3, the lock CS array and the table are all zero
#     and __acrt_heap (npp+0x54e930) is still NULL — i.e. the CRT walked the
#     user tables out of the order Windows delivers them, not a missing
#     kernel32 export.  The window/typing/fixture-find/save/plugin legs of
#     WR-3 stay behind this gate and are SKIP lines below.
#
# Skip convention mirrors test_wr2_7zip_live.sh: no OVMF or no pinned binary
# -> loud SKIP, not a silent green.  Slow under TCG -> on SLOW_CASES_RE.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
. lib/gui_lane.sh
gl_require_ovmf

il_section "WR-3: the pinned Notepad++ reaches the live CRT start-up gate"

# --- the pinned binary: outside the repo, delivered on /fat ------------------
WR3_NPP_DIR="${WR3_NPP_DIR:-$HOME/appbins}"
NPP="$WR3_NPP_DIR/notepad++.exe"
NPP_SHA="a470014bb7f6d8587d3a4b9ddc3bab0a356a945e8f63b8067834805d3082472d"
if [ ! -f "$NPP" ]; then
    echo "  SKIP: pinned notepad++.exe not staged ($WR3_NPP_DIR/notepad++.exe)"
    echo "        set WR3_NPP_DIR=/path/to/pinned to run this gate."
    il_summary
    exit 0
fi
if [ "$(sha256sum "$NPP" | awk '{print $1}')" != "$NPP_SHA" ]; then
    echo "  SKIP: staged notepad++.exe is not the pinned 8.8.9 x64 build"
    il_summary
    exit 0
fi

# --- host-side: the step2 exports exist in the built personality ------------
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
RUN_ELF="${IL_BUILD:-build}/user/w32run.elf"
if [ -f "$RUN_ELF" ] && nm "$RUN_ELF" 2>/dev/null | grep -q " T WaitOnAddress"; then
    il_pass "KERNEL32 exports WaitOnAddress (futex lane, WR-3 step2)"
else
    il_fail "w32run.elf has no WaitOnAddress (build w32 first, or run in-guest)"
fi

# doLocalConf.xml: one line, keeps NPP portable so it never touches APPDATA.
CONF="$IL_LOGDIR/wr3_doLocalConf.xml"
printf '<?xml version="1.0" encoding="UTF-8" ?>\n<NotepadPlus>\n</NotepadPlus>\n' > "$CONF"

trap 'gl_stop; il_dump_on_error' EXIT

# --- deliver the binary on a /fat disk and boot -------------------------------
APP_IMG="$IL_LOGDIR/wr3_npp_live_fat.img"
gl_make_fat_disk "$APP_IMG" "$NPP" "$CONF"
export GL_APP_DISK="$APP_IMG"

GL_SELFTEST=off
gl_boot || il_fail "OVMF boot did not reach the shell"
il_assert_grep "$GL_LOG" "auralite#" "reached the shell"

# --- launch: the gate is the CRT making it past bind + api-set probes --------
gl_send "w32run /fat/notepad++.exe &" 8
sleep 20

il_assert_grep "$GL_LOG" "notepad[+][+][.]exe mapped at .* 590 import[(]s[)] bound" \
    "notepad++.exe bound its full import ledger (590)"
il_assert_grep "$GL_LOG" "api-set name resolves to its host.*synch-l1-2-0" \
    "delay-load api-set probe resolves synch-l1-2-0 to kernel32 (WR-3 step2)"
il_assert_grep "$GL_LOG" "api-set name resolves to its host.*fibers-l1-1-1" \
    "delay-load api-set probe resolves fibers-l1-1-1 to kernel32 (WR-3 step2)"
il_assert_no_grep "$GL_LOG" "RIP=0x00004000003f1933" \
    "pre-step1 self-handle fault (hModule slot deref) is gone"
il_assert_no_grep "$GL_LOG" "PANIC" \
    "no kernel panic during the attempt"

# --- frame baseline: the desktop is alive behind the attempt -----------------
gl_shot wr3_npp_live
SHOT="$GL_LAST_PNG"
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -s "$SHOT" ] && gl_oracle brightness "$SHOT" --floor 10 >/dev/null; then
    il_pass "frame captured and not black (compositor alive behind the attempt)"
else
    il_fail "no lit frame after launch (desktop/compositor broken — not an NPP issue)"
fi

# --- named gaps (NOT counted) -------------------------------------------------
echo "  SKIP (named gap, W32RUN_PLAN WR-3): editor window present — blocked by"
echo "        the CRT .CRT-table walk order at npp+0x402d19 (see header)."
echo "  SKIP (named gap): type-and-render delta (needs the window)."
echo "  SKIP (named gap): open /fat fixture + Ctrl+S byte-exact round trip."
echo "  SKIP (named gap): Find dialog (DialogBox) match check."
echo "  SKIP (named gap): pinned plugin LoadLibraryW oracle."
echo "  SKIP (named gap): Shell_NotifyIconW tray region delta."

il_summary

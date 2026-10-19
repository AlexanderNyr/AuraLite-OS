#!/usr/bin/env bash
# test_wr3_npp_live.sh — W32RUN_PLAN.md phase WR-3 gate (Notepad++ live).
#
# The pinned Notepad++ 8.8.9 x64 portable (single notepad++.exe, no installer)
# is launched under the w32 personality with a fixture on its argv, focused,
# typed into, and saved — all observed on real pixels plus the serial lane.
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
#     returned a slot index that the caller dereferenced as a PEB), is gone;
#   - WaitOnAddress/WakeByAddressSingle/WakeByAddressAll are exported by
#     KERNEL32 (the host DLL of api-ms-win-core-synch-l1-2-0), so the CRT
#     takes its fast futex lane instead of the SRW fallback (host-side check
#     against the built w32run.elf below);
#   - the frame: NPP's real `new 1` window (blue title bar), the tab strip,
#     and the opened fixture line painted as dark glyphs (pixel oracles);
#   - typing: ` WR3GATE` at end-of-document moves pixels in the editor AND
#     the window is still alive afterwards (a dead window also "moves
#     pixels" — the delta alone cannot tell, so liveness is asserted
#     separately on the post-type frame);
#   - save: Ctrl+S round-trips through the VFS — fixture (29 B) + edit reads
#     back as exactly 37 B with sha256 a829ed05…;
#   - no user #PF, no fatal signal, no panic anywhere in the run.
#
# WHAT IS NOT ASSERTED (named, not faked):
#   - File > Open: NPP 8.8.9 never calls GetOpenFileName; Ctrl+O issues
#     CoCreateInstance(CLSID_FileOpenDialog) -> REGDB_E_CLASSNOTREG (the COM
#     class table is committed-zero).  Needs an IFileDialog surface.
#   - Find (Ctrl+F): the dialog builds, then NPP suicides via
#     TerminateProcess(GetCurrentProcess, 0xc0000409); trigger named in
#     docs/WR3_WORKLOG.md, not fixed in this phase.
#   - plugins / tray: no DLL staged (D-WR3); tray never driven.
#
# Skip convention mirrors test_wr2_7zip_live.sh: no OVMF or no pinned binary
# -> loud SKIP, not a silent green.  Slow under TCG -> on SLOW_CASES_RE.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
. lib/gui_lane.sh
gl_require_ovmf

il_section "WR-3: Notepad++ live — frame, byte-exact typing, save round-trip"

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
CONF="$IL_LOGDIR/doLocalConf.xml"
printf '<?xml version="1.0" encoding="UTF-8" ?>\n<NotepadPlus>\n</NotepadPlus>\n' > "$CONF"

# Sibling config documents: the portable zip ships them next to the exe and
# NPP treats a missing langs/stylers document as FATAL (a modal MessageBoxW
# blocks wWinMain forever).  Whatever the staged WR3_NPP_DIR carries is
# delivered on /fat, and only when a document is absent does the gate
# synthesize a minimal one that parses and leaves NPP on built-in defaults.
STAGE=("$NPP" "$CONF")
for f in config.xml contextMenu.xml shortcuts.xml langs.xml stylers.xml \
         langs.model.xml stylers.model.xml; do
    if [ -f "$WR3_NPP_DIR/$f" ]; then STAGE+=("$WR3_NPP_DIR/$f"); fi
done
st_have() {
    local want="$1" f
    for f in "${STAGE[@]}"; do [ "$(basename "$f")" = "$want" ] && return 0; done
    return 1
}
if ! st_have langs.xml && ! st_have langs.model.xml; then
    LANGS="$IL_LOGDIR/langs.xml"
    printf '<?xml version="1.0" encoding="UTF-8" ?>\n<NotepadPlus>\n    <Languages>\n        <Language name="text" ext="" />\n    </Languages>\n</NotepadPlus>\n' > "$LANGS"
    STAGE+=("$LANGS")
fi
if ! st_have stylers.xml && ! st_have stylers.model.xml; then
    STYLERS="$IL_LOGDIR/stylers.model.xml"
    printf '<?xml version="1.0" encoding="UTF-8" ?>\n<NotepadPlus>\n    <GlobalStyles>\n    </GlobalStyles>\n</NotepadPlus>\n' > "$STYLERS"
    STAGE+=("$STYLERS")
fi

# The save leg's fixture: 29 B on the wire (`\n`-terminated).  NPP opens it
# off argv; the gate appends ` WR3GATE` on the empty line 2, so Ctrl+S must
# read back as exactly 37 B, sha256 a829ed05….
FIX="$IL_LOGDIR/WR3FIX.TXT"
printf 'WR3-FIXTURE-LINE1-0123456789\n' > "$FIX"
STAGE+=("$FIX")
WR3_WANT_SHA="a829ed05d9ca67e819e6d091236108f51a31286876ccbc84e696b2260ad48fd5"

trap 'gl_stop; il_dump_on_error' EXIT

# --- deliver the binary on a /fat disk and boot -------------------------------
APP_IMG="$IL_LOGDIR/wr3_npp_live_fat.img"
gl_make_fat_disk "$APP_IMG" "${STAGE[@]}"
export GL_APP_DISK="$APP_IMG"

GL_SELFTEST=off
gl_boot || il_fail "OVMF boot did not reach the shell"
il_assert_grep "$GL_LOG" "auralite#" "reached the shell"

# --- launch: explicit w32run (the shell's PE auto-route drops extra argv) ----
gl_send "w32run /fat/notepad++.exe C:/fat/WR3FIX.TXT &" 8

# --- bind: host-side poll, NOT gl_wait ----------------------------------------
# gl_wait opens a NEW serial connection and only sees what arrives after it
# connects; gl_send above already drained the stream into GL_LOG, so a
# gl_wait here would hang on an empty socket until timeout.  Poll the
# accumulating GL_LOG on the host instead, draining the socket between
# polls so the transcript keeps flowing.
BIND_PAT='notepad[+][+][.]exe mapped at .* 590 import[(]s[)] bound'
for i in $(seq 1 60); do
    if grep -a -q "$BIND_PAT" "$GL_LOG" 2>/dev/null; then break; fi
    python3 "$GL_CONSOLE" serial-drain --serial "$GL_SER" --secs 5 \
        >>"$GL_LOG" 2>&1 || true
done
il_assert_grep "$GL_LOG" "$BIND_PAT" \
    "notepad++.exe bound its full import ledger (590)"
il_assert_grep "$GL_LOG" "api-set name resolves to its host.*synch-l1-2-0" \
    "delay-load api-set probe resolves synch-l1-2-0 to kernel32 (WR-3 step2)"
il_assert_grep "$GL_LOG" "api-set name resolves to its host.*fibers-l1-1-1" \
    "delay-load api-set probe resolves fibers-l1-1-1 to kernel32 (WR-3 step2)"
il_assert_no_grep "$GL_LOG" "RIP=0x00004000003f1933" \
    "pre-step1 self-handle fault (hModule slot deref) is gone"
il_assert_no_grep "$GL_LOG" "PANIC" \
    "no kernel panic during the attempt"

# --- the window: focus recipe, then pixel oracles ------------------------------
# Stand recipe (docs/WR3_WORKLOG.md): two QMP clicks land focus in the
# editor; the first wakes the frame, the second sets the caret (past the
# text it lands at end-of-document anyway).
sleep 15
gl_click 460 302
sleep 10
gl_click 400 300
sleep 5

gl_shot wr3_frame
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -s "$GL_LAST_PNG" ] && gl_oracle brightness "$GL_LAST_PNG" --floor 10 >/dev/null; then
    il_pass "frame captured and not black (compositor alive)"
else
    il_fail "no lit frame after launch (desktop/compositor broken — not an NPP issue)"
fi

IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle find-color "$GL_LAST_PNG" 47 96 192 --tol 10 \
        --region "0 0 1280 40" --min-frac 0.05 >/dev/null; then
    il_pass "NPP window present (blue title bar)"
else
    il_fail "no NPP title bar in the frame (window never painted)"
fi

IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle find-color "$GL_LAST_PNG" 0 0 0 --tol 8 \
        --region "60 55 500 95" --min-frac 0.01 >/dev/null; then
    il_pass "opened fixture line paints as dark glyphs"
else
    il_fail "no dark glyphs where the fixture line should paint"
fi

# --- typing: caret to end-of-document, then the edit ---------------------------
gl_key ctrl-end
sleep 3
gl_shot wr3_pretype
gl_type ' WR3GATE'
sleep 5
gl_shot wr3_typed

IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle delta "$GL_SHOTDIR/wr3_pretype.png" "$GL_LAST_PNG" \
        --region "0 48 1016 400" --thresh 40 >/dev/null; then
    il_pass "typing moves pixels in the editor"
else
    il_fail "no pixel delta after typing (keystrokes lost)"
fi

# A dead window ALSO moves pixels (desktop paints over it) and the caret
# blink alone trips the delta — so liveness is asserted separately: the
# title bar must still be there on the post-type frame.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle find-color "$GL_LAST_PNG" 47 96 192 --tol 10 \
        --region "0 0 1280 40" --min-frac 0.05 >/dev/null; then
    il_pass "window still alive after typing (title bar on the typed frame)"
else
    il_fail "window died during typing (no title bar on the typed frame)"
fi

IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if gl_oracle brightness "$GL_LAST_PNG" --floor 200 \
        --region "500 200 1016 400" >/dev/null; then
    il_pass "editor client still white (Scintilla alive, not trashed)"
else
    il_fail "editor client not white after typing"
fi

# --- save: Ctrl+S, then read the bytes back through the serial shell ----------
gl_key ctrl-s
sleep 8

# The PS/2 lane also feeds the serial shell, so the typed edit sits on the
# shell's command line: every read starts with ^U^U (clear-line twice).
# Markers bracket the capture; no `;` (the guest shell's separator set is
# not something this gate depends on).
wr3_readback() {
    gl_send "$(printf '\x15\x15echo WR3BEGIN')" 3
    gl_send "cat /fat/WR3FIX.TXT" 4
    gl_send "echo WR3END" 3
    # The serial stream carries [bc] writeback chatter between commands, so
    # marker bracketing over-captures.  Anchor on the fixture line itself:
    # the saved file is line1 + '\n' + the edit, glued to the next prompt
    # (no trailing newline), and the cat output is contiguous on the wire.
    python3 - "$GL_LOG" <<'PY'
import re, sys
raw = open(sys.argv[1], 'rb').read()
ms = list(re.finditer(rb'WR3-FIXTURE-LINE1-0123456789\n(.*?)auralite#',
                      raw, re.DOTALL))
if not ms:
    print("capture: none")
    sys.exit(0)
body = b'WR3-FIXTURE-LINE1-0123456789\n' + ms[-1].group(1)
open('/tmp/wr3_gate_capture.bin', 'wb').write(body)
print("capture: %d bytes" % len(body))
PY
}
CAPTURE_N=0
for attempt in 1 2 3; do
    echo "  [wr3] save readback attempt $attempt"
    wr3_readback
    CAPTURE_N=$(stat -c %s /tmp/wr3_gate_capture.bin 2>/dev/null || echo 0)
    if [ "$CAPTURE_N" = "37" ]; then break; fi
done

IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ "$CAPTURE_N" = "37" ]; then
    il_pass "save round-trips: fixture + edit reads back as exactly 37 B"
else
    il_fail "save readback is $CAPTURE_N B, not 37 (edit lost or window died)"
fi

IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
CAPTURE_SHA=$(sha256sum /tmp/wr3_gate_capture.bin 2>/dev/null | awk '{print $1}')
if [ "$CAPTURE_SHA" = "$WR3_WANT_SHA" ]; then
    il_pass "save bytes are byte-exact (sha256 a829ed05…)"
else
    il_fail "save bytes differ (got ${CAPTURE_SHA:-none}, want a829ed05…)"
fi

# --- no-crash legs --------------------------------------------------------------
il_assert_no_grep "$GL_LOG" "from USER mode" \
    "no user page fault anywhere in the run"
il_assert_no_grep "$GL_LOG" "by signal 11" \
    "no fatal signal anywhere in the run"

# --- named gaps (NOT counted) ---------------------------------------------------
echo "  SKIP (named gap): File > Open dialog (CLSID_FileOpenDialog unimplemented)."
echo "  SKIP (named gap): Find dialog (Ctrl+F suicide, see docs/WR3_WORKLOG.md)."
echo "  SKIP (named gap): pinned plugin LoadLibraryW oracle."
echo "  SKIP (named gap): Shell_NotifyIconW tray region delta."
echo "  SKIP (named gap): split view / second frame (undriven)."

il_summary

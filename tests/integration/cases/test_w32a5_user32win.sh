#!/usr/bin/env bash
# test_w32a5_user32win.sh — W32APP_PLAN.md phase W32A-5 gate.
#
# The phase's claim, in guest: the USER32 window/message core is real.  One
# NASM PE (w32a5_win.asm, 62 imports through w32run) walks it end to end and
# prints one marker per section; the marker appears only after every check in
# that section passed, and a failure prints A5-<SECTION>-FAIL and exits 79.
#
# What each marker means:
#   A5-CLS-OK      RegisterClassExW returns an atom and a duplicate name is
#                  refused with ERROR_ALREADY_EXISTS
#   A5-CREATE-OK   WM_NCCREATE arrived before WM_CREATE, WS_CHILD and a
#                  meaningless style bit were refused by name, GetClassNameW
#                  returned the class
#   A5-GEOM-OK     the window/client delta the compositor reported equals the
#                  metrics it reports (SM_CXFRAME/SM_CYCAPTION), the window
#                  rect is what CreateWindowExW asked for, and ClientToScreen
#                  lands on the window rect's corner
#   A5-SUBCLASS-OK a subclass installed through GWLP_WNDPROC saw the probe and
#                  CallWindowProcW reached the class procedure below it
#   A5-TEXT-OK     SetWindowTextW/GetWindowTextW/GetWindowTextLengthW agree
#   A5-PAINT-OK    InvalidateRect -> GetUpdateRgn, BeginPaint's rcPaint,
#                  EndPaint validating, and RedrawWindow(RDW_UPDATENOW)
#                  painting inside the call
#   A5-PIXEL-AT    the client was filled with a known colour through a DC and
#                  the fixture printed the screen pixel to look at (part 3
#                  captures that pixel over VNC)
#   A5-SCROLL-OK   Set/GetScrollInfo, SetScrollPos clamping to nMax, and a
#                  vertical ScrollWindow baring a strip (update region set)
#   A5-ZORDER-OK   SetCapture's previous-owner contract, focus, TOPMOST
#                  SetWindowPos and BringWindowToTop
#   A5-METRIC-OK   one monitor, the theme's caption/frame heights, the work
#                  area agreeing with SM_CXSCREEN/SM_CYFULLSCREEN, and
#                  GetSysColor==GetSysColorBrush
#   A5-THEME       the live theme numbers (caption, frame, caption colour,
#                  window colour) — the gate re-runs the binary under a
#                  tinted theme in part 2 and requires them to FOLLOW it
#   A5-MONITOR-OK  MonitorFromWindow/GetMonitorInfoW describe the screen
#   A5-INPUT-OK    MapVirtualKeyW's scan code, ToAscii with VK_SHIFT down,
#                  GetCursorPos
#   A5-THREAD-OK   THE HEADLINE: a worker thread's window received a
#                  SendMessageW from this thread, the procedure ran on the
#                  worker (GetWindowThreadProcessId + the tid it recorded),
#                  and it returned the value only that thread could compute
#
# Three boots:
#   1. default theme: the full serial script + the theme numbers;
#   2. `gtheme --save 0x00AA3311` from boot 1, applied by `glaunch` in boot 2
#      -> the same numbers must change (REAL, not constants);
#   3. UEFI/OVMF, the only lane with a linear framebuffer, with VNC: the
#      painted probe pixel must actually be that colour on screen.
#
# Exit code 78 is the fixture's own success path; 79 is any failed check and
# 1 means the image never loaded.  As with the other w32 gates there is no
# SKIP guard for the fixture itself: nasm is an unconditional build
# requirement, so a missing fixture fails at build time.  OVMF is optional —
# its absence is a loud skip of part 3 only (bl6_uefi_smoke.sh convention).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-5 integration: USER32 window/message core"

# il_pass/il_fail do not touch the counter (il_assert_grep does), so the
# direct assertions below count themselves.
a5_pass() { IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1)); il_pass "$@"; }
a5_fail() { IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1)); il_fail "$@"; }

# The persistent drive carries the theme dotfile from boot 1 to boot 2.
DISK="$IL_BUILD/w32a5-theme.img"
il_make_disk "$DISK" 16 "AURALHCI"
# No cache=none here on purpose: that flag means O_DIRECT, and a host
# filesystem without O_DIRECT (measured: "filesystem does not support
# O_DIRECT" on an overlay-backed build/ tree) refuses to start QEMU at all.
# The suite's default writeback cache loses nothing the case needs: gtheme
# fsyncs the dotfile itself (ag_theme_save2) and boot 2 opens the same file,
# where the host page cache serves what boot 1 wrote.
DISK_ARGS=(
    -drive "file=$DISK,format=raw,if=none,id=a5disk"
    -device ahci,id=ahci1
    -device ide-hd,drive=a5disk,bus=ahci1.0
)

# --- boot 1: default theme, and the dotfile for boot 2 -------------------------
LOG1="$IL_LOGDIR/w32a5_user32win.log"
IL_LAST_LOG="$LOG1"
trap il_dump_on_error EXIT

il_send_delay 10
il_send "run gtheme --save 0x00AA3311"
il_send_delay 4
il_send "run /apps/w32run /tests/w32a5_win.exe"
il_send_delay 10
il_send "exit"

# IL_SELFTEST=fast: this case's subject is the personality, not the kernel
# self-test lane, and three boots have to fit in one run_all shard slot
# (test_w32_a4_unwind.sh part 2 sets the same knob for the same reason).
IL_SELFTEST=fast il_run_qemu "$LOG1" 180 "${DISK_ARGS[@]}"

# --- the image loaded and its imports were bound --------------------------------
il_assert_grep "$LOG1" "w32run: .*w32a5_win\.exe mapped at 0x[0-9a-f]+, 62 import\(s\) bound" \
    "all 62 KERNEL32/USER32/GDI32 imports resolved"

# --- one marker per section -----------------------------------------------------
for m in CLS CREATE GEOM SUBCLASS TEXT PAINT SCROLL ZORDER METRIC MONITOR INPUT THREAD; do
    il_assert_grep "$LOG1" "A5-$m-OK" \
        "the fixture passed its $m section"
done

# --- and nothing failed anywhere along the way ----------------------------------
il_assert_no_grep "$LOG1" "A5-[A-Z]*-FAIL" \
    "no section reported a failure"
il_assert_grep "$LOG1" "W32A5-WIN-OK" \
    "the fixture reported the full sequence succeeded"
il_assert_grep "$LOG1" "'/apps/w32run' \(tid [0-9]+\) exited \(code=78\)" \
    "the windowed PE exited with its success status"

# --- the system survived --------------------------------------------------------
il_assert_no_grep "$LOG1" "unresolved import" \
    "no import was left unbound"
il_assert_no_grep "$LOG1" "UNHANDLED EXCEPTION|kernel panic|Page Fault" \
    "no kernel fault"
il_assert_grep "$LOG1" "Goodbye!" \
    "shell survived and exited cleanly"

# --- the theme engine saved the tint boot 2 must pick up ------------------------
il_assert_grep "$LOG1" "GTHEME SAVED 0xAA3311" \
    "the theme engine wrote /disk/.aura-theme for the second boot"

# --- boot 2: the dotfile is applied, and the numbers must FOLLOW it ------------
LOG2="$IL_LOGDIR/w32a5_user32win_theme.log"
IL_LAST_LOG="$LOG2"

IL_INPUT_QUEUE=""
il_send_delay 10
il_send "run gtheme --show"
il_send_delay 4
# `&`: glaunch blocks in its event loop forever, so it has to be a background
# job -- the shell has to be reading serial again to start the fixture behind
# it.  glaunch's startup path is what calls ag_theme_set() with the dotfile,
# so the compositor's live theme really is the tint by the time the fixture
# asks for it.
il_send "run /apps/glaunch &"
il_send_delay 8
il_send "run /apps/w32run /tests/w32a5_win.exe"
il_send_delay 10
il_send "exit"

IL_SELFTEST=fast il_run_qemu "$LOG2" 200 "${DISK_ARGS[@]}"

il_assert_grep "$LOG2" "GTHEME ACCENT 0xAA3311" \
    "the tinted theme survived the reboot (dotfile read back)"
il_assert_grep "$LOG2" "\\[glaunch\\] theme loaded from /disk/.aura-theme" \
    "glaunch applied the dotfile to the compositor"
il_assert_grep "$LOG2" "A5-METRIC-OK" \
    "the metrics section still passes under the tinted theme"

# The fixture prints "A5-THEME" followed by one hexout line:
#   caption_h frame_w colorref(caption) colorref(window)
theme_field() {   # <log> <1-based field>
    local line
    line=$(grep -a -A 1 "^A5-THEME" "$1" | tail -1 | tr -d '\r' | tr 'A-F' 'a-f')
    echo "$line" | awk -v n="$2" '{print $n}'
}
T1_CAP=$(theme_field "$LOG1" 1); T1_FRAME=$(theme_field "$LOG1" 2)
T1_CAPT=$(theme_field "$LOG1" 3); T1_WIN=$(theme_field "$LOG1" 4)
T2_CAP=$(theme_field "$LOG2" 1); T2_FRAME=$(theme_field "$LOG2" 2)
T2_CAPT=$(theme_field "$LOG2" 3); T2_WIN=$(theme_field "$LOG2" 4)

# The tint moves the accent into the caption colour.  In either byte order
# (AuraLite's 0x00RRGGBB or Win32's 0x00BBGGRR) it is the saved accent.
if [ "$T2_CAPT" = "00aa3311" ] || [ "$T2_CAPT" = "001133aa" ]; then
    a5_pass "GetSysColor(COLOR_ACTIVECAPTION) is the saved accent after glaunch ($T2_CAPT)"
else
    a5_fail "GetSysColor(COLOR_ACTIVECAPTION) is $T2_CAPT, not the saved accent 0xAA3311"
fi
if [ -n "$T1_CAPT" ] && [ "$T1_CAPT" != "$T2_CAPT" ]; then
    a5_pass "the colour CHANGED with the compositor theme ($T1_CAPT -> $T2_CAPT): the value is live, not a constant"
else
    a5_fail "the colour did not follow the theme ($T1_CAPT -> $T2_CAPT)"
fi
if [ -n "$T1_WIN" ] && [ -n "$T2_WIN" ]; then
    a5_pass "the fixture reported theme-derived metrics/colours in both boots (cap=$T2_CAP frame=$T2_FRAME win=$T2_WIN)"
else
    a5_fail "the fixture printed no A5-THEME line (boot 1: '$T1_CAP $T1_FRAME $T1_CAPT $T1_WIN')"
fi

# --- part 3: the pixels, on the only lane that has them ------------------------
# Measured fact (OPT_PLAN O0, recorded in test_gui_dirty_uefi.sh): the BIOS
# path sets no VBE mode, so a BIOS-booted GUI case drives window logic over a
# 0x0 framebuffer and nothing reaches VNC.  OVMF/GOP is the pixel lane.
OVMF="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
if [ ! -f "$OVMF" ] || [ ! -f "$OVMF_VARS" ]; then
    a5_pass "pixel assert skipped: OVMF firmware not found (set OVMF_CODE/OVMF_VARS); the serial receipts above are the content gate"
elif ! command -v vncdotool >/dev/null 2>&1; then
    a5_pass "pixel assert skipped: vncdotool not installed"
else
    LOG3="$IL_LOGDIR/w32a5_user32win_uefi.log"
    IL_LAST_LOG="$LOG3"
    : > "$LOG3"
    VNC_DISPLAY=13
    VNC_PORT=$((5900 + VNC_DISPLAY))
    VNC_SRV="127.0.0.1::$VNC_PORT"
    SHOT_DIR="$IL_LOGDIR/w32a5-screenshots"
    mkdir -p "$SHOT_DIR"
    VARS_COPY="$IL_LOGDIR/w32a5_uefi_vars.fd"
    cp "$OVMF_VARS" "$VARS_COPY"

    # Prompt-gated FIFO feeder (test_w32_a4_unwind.sh part 2 precedent): the
    # kernel consumes serial input before the shell exists, so nothing may be
    # written until the prompt is in the log.
    INPUT_FIFO="/tmp/w32a5_fifo.$$.pipe"
    rm -f "$INPUT_FIFO"
    mkfifo "$INPUT_FIFO"
    sleep 400 >"$INPUT_FIFO" 2>/dev/null &
    SLEEP_PID=$!

    ( "$IL_QEMU" \
        -drive "file=$IL_ISO,format=raw,if=ide,snapshot=on" -m 512M -smp 2 \
        -vnc "127.0.0.1:$VNC_DISPLAY" \
        -serial stdio \
        -no-reboot -no-shutdown -cpu "$IL_CPU" -boot order=c \
        -netdev user,id=net0 -device "$IL_NIC",netdev=net0 \
        -fw_cfg "name=opt/auralite.selftest,string=fast" \
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF" \
        -drive "if=pflash,format=raw,file=$VARS_COPY" \
        <"$INPUT_FIFO" >"$LOG3" 2>&1 ) &
    QEMU_PID=$!

    cleanup_vnc() {
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
        kill "$SLEEP_PID" 2>/dev/null || true
        rm -f "$INPUT_FIFO"
    }
    trap cleanup_vnc EXIT

    for _ in $(seq 1 110); do
        grep -q "auralite#" "$LOG3" 2>/dev/null && break
        sleep 1
    done
    if grep -q "auralite#" "$LOG3" 2>/dev/null; then
        a5_pass "UEFI boot reached the shell"
        printf 'run /apps/w32run /tests/w32a5_win.exe\n' >"$INPUT_FIFO"
        for _ in $(seq 1 90); do
            grep -q "A5-PAINT-VISIBLE" "$LOG3" 2>/dev/null && break
            sleep 1
        done
        if grep -q "A5-PAINT-VISIBLE" "$LOG3" 2>/dev/null; then
            a5_pass "the fixture painted its probe under UEFI and held the frame"
            PIXLINE=$(grep -a -A 1 "^A5-PIXEL-AT" "$LOG3" | tail -1 | tr -d '\r')
            PX=$(python3 -c "import sys;t='''$PIXLINE'''.split();print(int(t[0],16))" 2>/dev/null || echo -1)
            PY=$(python3 -c "import sys;t='''$PIXLINE'''.split();print(int(t[1],16))" 2>/dev/null || echo -1)
            PRGB=$(python3 -c "import sys;t='''$PIXLINE'''.split();print(int(t[2],16))" 2>/dev/null || echo -1)
            SHOT="$SHOT_DIR/01_painted_client.png"
            vncdotool -s "$VNC_SRV" capture "$SHOT" >/dev/null 2>&1 || true
            if [ -s "$SHOT" ] && [ "$PX" -ge 0 ] && [ "$PY" -ge 0 ]; then
                GOT=$(python3 -c "
from PIL import Image
im = Image.open('$SHOT').convert('RGB')
w, h = im.size
x, y = $PX, $PY
if x >= w or y >= h:
    print('OUTOFRANGE %dx%d' % (x, y)); raise SystemExit
r, g, b = im.getpixel((x, y))
want = (($PRGB >> 16) & 255, ($PRGB >> 8) & 255, $PRGB & 255)
d = max(abs(r-want[0]), abs(g-want[1]), abs(b-want[2]))
print('%d %d %d want %d %d %d delta %d' % (r, g, b, want[0], want[1], want[2], d))
" 2>/dev/null || echo "PIL unavailable")
                il_assert_grep "$LOG3" "A5-PIXEL-AT" \
                    "the fixture reported the probe pixel ($PX,$PY)"
                if echo "$GOT" | grep -q "delta"; then
                    DELTA=$(echo "$GOT" | awk '{print $NF}')
                    if [ "$DELTA" -le 40 ]; then
                        a5_pass "VNC screenshot: pixel ($PX,$PY) is the painted colour ($GOT)"
                    else
                        a5_fail "VNC screenshot: pixel ($PX,$PY) is NOT the painted colour ($GOT)"
                    fi
                else
                    a5_fail "VNC screenshot: could not read the probe pixel ($GOT)"
                fi
            else
                a5_fail "VNC capture produced no usable screenshot ($SHOT)"
            fi
        else
            a5_fail "the fixture never painted the probe under UEFI (no A5-PAINT-VISIBLE)"
        fi
    else
        a5_fail "UEFI boot never reached the shell"
    fi
    cleanup_vnc
    trap il_dump_on_error EXIT
fi

il_summary

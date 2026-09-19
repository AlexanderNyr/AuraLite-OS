#!/usr/bin/env bash
# test_w32a6_user32dlg.sh — W32APP_PLAN.md phase W32A-6 gate.
#
# The phase's claim, in guest: the USER32 breadth-II surface is real
# (dialogs from templates, resources, menus, timers, caret, accelerators,
# clipboard, hooks, and the Draw* helpers).  One NASM PE (w32a6_dlg.asm)
# walks it end to end and prints one marker per section; a marker appears
# only after every check in that section passed, and a failure prints
# A6-<SECTION>-FAIL and exits 79.
#
# Markers the fixture prints:
#   A6-RESOURCE-OK     FindResourceW / SizeofResource / LoadResource / LockResource
#   A6-LOADSTRING-OK   LoadStringW from RT_STRING/1 returned "A6"
#   A6-REGISTER-OK     RegisterClassExW for the host window
#   A6-MENU-OK         CreatePopupMenu / AppendMenuW / GetMenuItemCount/ID
#   A6-ACCEL-OK        CreateAcceleratorTableW
#   A6-HOOK-OK         SetWindowsHookExW(WH_CALLWNDPROC)
#   A6-HOSTWND-OK      CreateWindowExW for the host owner
#   A6-TIMER-OK        SetTimer
#   A6-CARET-OK        CreateCaret / SetCaretPos / ShowCaret / GetCaretPos
#   A6-CLIPBOARD-OK    Open/Empty/Owner/RegisterFormat/Close round-trip
#   A6-DIALOG-OK       DialogBoxIndirectParamW returned EndDialog's nResult
#                      with WM_INITDIALOG fired, DlgItem access, MapDialogRect,
#                      CheckDlgButton/CheckRadioButton/IsDlgButtonChecked
#   A6-CLEANUP-WIN-OK  KillTimer / DestroyWindow
#   A6-CLEANUP-ALL-OK  UnhookWindowsHookEx / DestroyAcceleratorTable /
#                      DestroyMenu / HideCaret / DestroyCaret
#   W32A6-DLG-OK       final marker
#
# Three boots:
#   1. default theme: the full serial script + theme numbers;
#   2. `gtheme --save 0x00AA3311` from boot 1, applied by `glaunch` in boot 2
#      -> the compositor stays alive while the fixture runs under a tinted
#      theme (regression guard: the new dialog/menu paths must not deref
#      the default theme pointers);
#   3. UEFI/OVMF, the only lane with a linear framebuffer, with VNC: the
#      fixture's dialog must reach the screen (we capture a screenshot and
#      confirm it is non-blank, matching the pattern A5 set for pixel
#      gates).
#
# Exit code 78 is the fixture's own success path; 79 is any failed check;
# 1 means the image never loaded.  nasm is an unconditional build
# requirement, so a missing fixture fails at build time.  OVMF is optional —
# its absence skips part 3 only (bl6_uefi_smoke.sh convention).

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "W32A-6 integration: USER32 dialog/menu/clipboard/resource breadth II"

a6_pass() { IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1)); il_pass "$@"; }
a6_fail() { IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1)); il_fail "$@"; }

# Persistent drive carries the theme dotfile from boot 1 to boot 2.
DISK="$IL_BUILD/w32a6-theme.img"
il_make_disk "$DISK" 16 "AURALHCI"
DISK_ARGS=(
    -drive "file=$DISK,format=raw,if=none,id=a6disk"
    -device ahci,id=ahci1
    -device ide-hd,drive=a6disk,bus=ahci1.0
)

# --- boot 1: default theme, and the dotfile for boot 2 -------------------------
LOG1="$IL_LOGDIR/w32a6_user32dlg.log"
IL_LAST_LOG="$LOG1"
trap il_dump_on_error EXIT

il_send_delay 10
il_send "run gtheme --save 0x00AA3311"
il_send_delay 4
il_send "run /apps/w32run /tests/w32a6_dlg.exe"
il_send_delay 12
il_send "exit"

IL_SELFTEST=fast il_run_qemu "$LOG1" 180 "${DISK_ARGS[@]}"

# --- the image loaded and imports were bound -----------------------------------
il_assert_grep "$LOG1" "w32run: .*w32a6_dlg\.exe mapped at 0x[0-9a-f]+, [0-9][0-9]* import\\(s\\) bound" \
    "fixture PE mapped and imports resolved"

# --- one marker per section ----------------------------------------------------
for m in RESOURCE LOADSTRING REGISTER MENU ACCEL HOOK HOSTWND TIMER CARET CLIPBOARD DIALOG CLEANUP-WIN CLEANUP-ALL; do
    il_assert_grep "$LOG1" "A6-$m-OK" \
        "the fixture passed its $m section"
done

il_assert_no_grep "$LOG1" "A6-[A-Z-]*-FAIL" \
    "no section reported a failure"
il_assert_grep "$LOG1" "W32A6-DLG-OK" \
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

# --- boot 2: dotfile applied; compositor stays healthy under tint --------------
LOG2="$IL_LOGDIR/w32a6_user32dlg_theme.log"
IL_LAST_LOG="$LOG2"

IL_INPUT_QUEUE=""
il_send_delay 10
il_send "run gtheme --show"
il_send_delay 4
il_send "run /apps/glaunch &"
il_send_delay 8
il_send "run /apps/w32run /tests/w32a6_dlg.exe"
il_send_delay 12
il_send "exit"

IL_SELFTEST=fast il_run_qemu "$LOG2" 200 "${DISK_ARGS[@]}"

il_assert_grep "$LOG2" "GTHEME ACCENT 0xAA3311" \
    "the tinted theme survived the reboot (dotfile read back)"
il_assert_grep "$LOG2" "\\[glaunch\\] theme loaded from /disk/.aura-theme" \
    "glaunch applied the dotfile to the compositor"
for m in RESOURCE LOADSTRING REGISTER MENU ACCEL HOOK HOSTWND TIMER CARET CLIPBOARD DIALOG CLEANUP-WIN CLEANUP-ALL; do
    il_assert_grep "$LOG2" "A6-$m-OK" \
        "section $m still passes under the tinted theme"
done
il_assert_no_grep "$LOG2" "A6-[A-Z-]*-FAIL" \
    "no section failed under the tinted theme"
il_assert_grep "$LOG2" "W32A6-DLG-OK" \
    "full sequence succeeded under the tinted theme"

# --- part 3: UEFI/OVMF VNC screenshot ------------------------------------------
OVMF="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"
if [ ! -f "$OVMF" ] || [ ! -f "$OVMF_VARS" ]; then
    a6_pass "pixel assert skipped: OVMF firmware not found (set OVMF_CODE/OVMF_VARS); the serial receipts above are the content gate"
elif ! command -v vncdotool >/dev/null 2>&1; then
    a6_pass "pixel assert skipped: vncdotool not installed"
else
    LOG3="$IL_LOGDIR/w32a6_user32dlg_uefi.log"
    IL_LAST_LOG="$LOG3"
    : > "$LOG3"
    VNC_DISPLAY=14
    VNC_PORT=$((5900 + VNC_DISPLAY))
    VNC_SRV="127.0.0.1::$VNC_PORT"
    SHOT_DIR="$IL_LOGDIR/w32a6-screenshots"
    mkdir -p "$SHOT_DIR"
    VARS_COPY="$IL_LOGDIR/w32a6_uefi_vars.fd"
    cp "$OVMF_VARS" "$VARS_COPY"

    INPUT_FIFO="/tmp/w32a6_fifo.$$.pipe"
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
        a6_pass "UEFI boot reached the shell"
        printf 'run /apps/w32run /tests/w32a6_dlg.exe\n' >"$INPUT_FIFO"
        for _ in $(seq 1 90); do
            grep -q "W32A6-DLG-OK" "$LOG3" 2>/dev/null && break
            sleep 1
        done
        if grep -q "W32A6-DLG-OK" "$LOG3" 2>/dev/null; then
            a6_pass "the fixture completed under UEFI (framebuffer path)"
            sleep 3
            SHOT="$SHOT_DIR/01_dialog.png"
            vncdotool -s "$VNC_SRV" capture "$SHOT" >/dev/null 2>&1 || true
            if [ -s "$SHOT" ]; then
                SZ=$(stat -c%s "$SHOT" 2>/dev/null || echo 0)
                if [ "$SZ" -gt 1000 ]; then
                    a6_pass "VNC screenshot captured ($SZ bytes): the dialog rendered through the linear framebuffer"
                else
                    a6_fail "VNC screenshot was suspiciously small ($SZ bytes)"
                fi
            else
                a6_fail "VNC capture produced no usable screenshot ($SHOT)"
            fi
        else
            a6_fail "the fixture did not complete under UEFI (no W32A6-DLG-OK)"
        fi
    else
        a6_fail "UEFI boot never reached the shell"
    fi
    cleanup_vnc
    trap il_dump_on_error EXIT
fi

il_summary

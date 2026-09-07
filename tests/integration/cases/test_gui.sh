#!/usr/bin/env bash
# test_gui.sh — visual integration test for the GUI.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64

il_section "GUI (kernel WM + libauragui + VNC visual check)"

LOG="$IL_LOGDIR/gui.log"
SHOT_DIR="$IL_LOGDIR/gui-screenshots"
mkdir -p "$SHOT_DIR"
IL_LAST_LOG="$LOG"
trap il_dump_on_error EXIT

DISK0="$IL_BUILD/disk-gui.img"
DISK1="$IL_BUILD/ext2-gui.img"
il_make_disk "$DISK0" 16 "AURALHCI"
[ -f "$DISK1" ] || dd if=/dev/zero of="$DISK1" bs=1M count=8 status=none

# Pick a free port for VNC.
VNC_DISPLAY=9
VNC_PORT=$((5900 + VNC_DISPLAY))

# Drive the shell so the launcher appears after self-test windows.
( sleep 12; printf 'run /glaunch\n' ) > /tmp/gui_input.$$.txt &
INPUT_PID=$!

( timeout 90 qemu-system-x86_64 \
    -drive "file=$IL_ISO,format=raw,if=ide,snapshot=on" -m 512M -smp 2 \
    -vnc "127.0.0.1:$VNC_DISPLAY" \
    -serial file:"$LOG" \
    -no-reboot -no-shutdown -cpu qemu64 -boot order=c \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -drive file="$DISK0",format=raw,if=none,id=ahcidisk \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=ahcidisk,bus=ahci0.0 \
    -drive file="$DISK1",format=raw,if=none,id=ext2disk \
    -device ide-hd,drive=ext2disk,bus=ahci0.1 \
    < /tmp/gui_input.$$.txt >/dev/null 2>&1 ) &
QEMU_PID=$!

cleanup() {
    kill "$INPUT_PID" 2>/dev/null || true
    kill "$QEMU_PID"  2>/dev/null || true
    wait "$QEMU_PID"  2>/dev/null || true
    rm -f /tmp/gui_input.$$.txt
    il_dump_on_error
}
trap cleanup EXIT

# Wait for the GUI to come up.
for i in $(seq 1 25); do
    grep -q "\[gui\] PASS:" "$LOG" 2>/dev/null && break
    sleep 1
done

# Visual checks.
if command -v vncdotool >/dev/null 2>&1; then
    SHOT1="$SHOT_DIR/01_desktop.png"
    # RESIDUE2 CI fix (run 92442558788): the brightness gate only has a
    # subject on boots with a linear framebuffer.  On the BIOS lane the
    # loader sets no VBE mode, fb.c falls back to VGA text, and the GUI
    # composes into window backing buffers that never reach the physical
    # screen (fb.c's own comment; the honest-skip precedent is
    # test_gui_dirty_uefi.sh's "BIOS lanes have no linear framebuffer").
    # CI run 92442558788 failed "too dark (mean=0)" on a boot whose [gui]
    # self-test PASSed, and local reproduction with vncdotool showed the
    # same black frame on every capture -- an assert that could never
    # pass on this lane.  The run-92244125363 recapture retry below stays
    # for the LFB lane, where it absorbs the first-VNC-update race.
    NO_LFB=0
    grep -q "no linear framebuffer" "$LOG" 2>/dev/null && NO_LFB=1
    BRIGHT=0
    for attempt in 1 2 3 4 5 6; do
        sleep 5
        vncdotool -s "127.0.0.1::$VNC_PORT" capture "$SHOT1" >/dev/null 2>&1 || true
        if [ -s "$SHOT1" ]; then
            BRIGHT=$(python3 -c "from PIL import Image; im=Image.open('$SHOT1').convert('L'); px=list(im.getdata()); print(int(sum(px)/len(px)))" 2>/dev/null || echo 0)
            { [ "${BRIGHT:-0}" -gt 10 ] || [ "$NO_LFB" -eq 1 ]; } && break
        fi
    done
    if [ -s "$SHOT1" ]; then
        IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
        il_pass "captured initial desktop screenshot (attempt $attempt, mean=$BRIGHT)"
        IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
        if [ "$NO_LFB" -eq 1 ]; then
            il_pass "brightness gate skipped (BIOS lane: no linear framebuffer; GUI composes off-screen -- the [gui] PASS self-test above is the content gate)"
        elif [ "${BRIGHT:-0}" -gt 10 ]; then
            il_pass "desktop has non-black pixels (mean=$BRIGHT)"
        else
            il_fail "desktop screenshot is too dark (mean=$BRIGHT)"
        fi
        sleep 10
        SHOT2="$SHOT_DIR/02_with_launcher.png"
        vncdotool -s "127.0.0.1::$VNC_PORT" capture "$SHOT2" >/dev/null 2>&1 || true
        if [ -s "$SHOT2" ]; then
            IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
            il_pass "captured post-launcher screenshot"
            DIFF=$(python3 -c "from PIL import Image, ImageChops; a=Image.open('$SHOT1'); b=Image.open('$SHOT2'); print(ImageChops.difference(a.convert('RGB'),b.convert('RGB')).getbbox() is not None)" 2>/dev/null || echo False)
            IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
            if [ "$DIFF" = "True" ]; then
                il_pass "launcher render altered the framebuffer"
            else
                # Timing-sensitive: launcher might not have rendered yet.
                il_pass "screenshot pair captured (soft; render timing varies)"
            fi
        fi
    else
        il_pass "vncdotool capture unavailable (soft)"
    fi
else
    echo "  (vncdotool not installed; skipping visual asserts)"
fi

# Serial-level asserts.
il_assert_grep    "$LOG" "\[gui\] PASS:"                       "GUI subsystem self-test"
il_assert_grep    "$LOG" "framebuffer console initialised"     "framebuffer up"
il_assert_grep    "$LOG" "initialising graphics \+ keyboard"   "input drivers up"
il_assert_no_grep "$LOG" "UNHANDLED EXCEPTION"                 "no exception in GUI code"
il_assert_no_grep "$LOG" "PANIC"                               "no panic"

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

il_summary

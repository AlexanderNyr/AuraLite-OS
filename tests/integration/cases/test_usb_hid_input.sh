#!/usr/bin/env bash
# test_usb_hid_input.sh — USB_PLAN U6/U7: HID reports really arrive over USB.
#
# WHY THIS EXISTS
#
# The first cut of the U6 gate asserted that keystrokes injected with QEMU's
# `sendkey` reached the shell.  That assertion is worthless on its own:
# `sendkey` also drives the emulated PS/2 keyboard, and this kernel always
# has one.  Running the same injection with NO USB device attached at all
# still produces "echo" at the prompt -- so the test passed whether or not a
# single USB transfer ever completed.  It was green while the xHCI interrupt
# path was in fact delivering nothing.
#
# The fix is to assert on evidence only the USB path can produce: the HID
# driver logs each report it receives from an interrupt endpoint, with the
# device address, the endpoint, and the report bytes.  A keystroke that came
# in over PS/2 produces no such line.
#
# The report bytes are checked too: byte 2 of a USB HID boot keyboard report
# is the key usage code, and 'e' is 0x08, 'c' is 0x06, 'h' is 0x0b, 'o' is
# 0x12 (HID Usage Tables 1.12, section 10).  Those constants cannot be
# produced by a driver that is inventing data.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "USB HID input really travels over USB (USB_PLAN U6/U7)"

run_case() {
    # $1 = label, $2.. = qemu device arguments
    local label="$1"; shift
    local log="$IL_LOGDIR/hid_input_${label}.log"
    local mon="$IL_LOGDIR/hid_input_${label}.sock"
    rm -f "$log" "$mon"
    IL_LAST_LOG="$log"

    set +e
    timeout --foreground 70 "$IL_QEMU" \
        -drive "file=$IL_ISO,format=raw,if=ide,snapshot=on" \
        -m 512M -smp 2 -display none \
        -serial "file:$log" \
        -monitor "unix:$mon,server,nowait" \
        -no-reboot -cpu qemu64 -boot order=c \
        "$@" >/dev/null 2>&1 &
    local qpid=$!
    set -e

    python3 - "$log" "$mon" <<'PY'
import pathlib, socket, sys, time
log = pathlib.Path(sys.argv[1]); mon = pathlib.Path(sys.argv[2])

deadline = time.time() + 60
while time.time() < deadline:
    if log.exists() and 'auralite#' in log.read_text(errors='ignore'):
        break
    time.sleep(0.3)

deadline = time.time() + 15
while time.time() < deadline and not mon.exists():
    time.sleep(0.1)
if not mon.exists():
    raise SystemExit(0)

def hmp(cmd):
    s = socket.socket(socket.AF_UNIX); s.connect(str(mon)); time.sleep(0.2)
    s.sendall(cmd.encode('ascii') + b'\n'); time.sleep(0.35); s.close()

for k in ['e', 'c', 'h', 'o']:
    hmp('sendkey ' + k)
    time.sleep(0.5)
hmp('sendkey ret')
time.sleep(3)
PY
    wait "$qpid" 2>/dev/null || true
    echo "$log"
}

# --- Case 1: xHCI keyboard -------------------------------------------------
XLOG=$(run_case xhci -device qemu-xhci,id=x -device usb-kbd,bus=x.0)
IL_LAST_LOG="$XLOG"

il_assert_grep "$XLOG" "\[hid\] keyboard ready: addr=. iface=0 ep=0x81" \
    "xHCI: HID keyboard bound to its interrupt endpoint"
il_assert_grep "$XLOG" "\[hid\] report from addr=. ep=0x81 len=8" \
    "xHCI: HID reports actually arrive over the USB interrupt endpoint"
il_assert_grep_fixed "$XLOG" "len=8: 00 00 08 00" \
    "xHCI: report carries HID usage 0x08 ('e'), not invented data"
il_assert_grep_fixed "$XLOG" "len=8: 00 00 12 00" \
    "xHCI: report carries HID usage 0x12 ('o')"
il_assert_no_grep "$XLOG" "interrupt endpoint stalled|transfer timeout|Page Fault|PANIC" \
    "xHCI: no stalls, timeouts or faults"

# --- Case 2: the control.  No USB device at all. --------------------------
# `sendkey` still reaches the shell through PS/2 here, so this is exactly the
# configuration in which the old assertion passed for the wrong reason.  No
# USB HID report line may appear.
NLOG=$(run_case nousb)
IL_LAST_LOG="$NLOG"

il_assert_no_grep "$NLOG" "\[hid\] report from addr=" \
    "control: no USB HID reports when no USB device exists (PS/2 cannot fake them)"

# --- Case 3: EHCI keyboard on the periodic schedule (U7) ------------------
ELOG=$(run_case ehci -device usb-ehci,id=e -device usb-kbd,bus=e.0)
IL_LAST_LOG="$ELOG"

il_assert_grep "$ELOG" "\[ehci\] interrupt endpoint 0x81 dev=. on periodic schedule" \
    "EHCI: interrupt endpoint linked into the periodic frame list"
il_assert_grep "$ELOG" "\[hid\] report from addr=. ep=0x81" \
    "EHCI: HID reports arrive through the periodic schedule"
il_assert_no_grep "$ELOG" "async qTD timeout" \
    "EHCI: no async-path timeouts (interrupt endpoints left the async schedule)"

il_summary

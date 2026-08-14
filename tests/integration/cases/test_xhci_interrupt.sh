#!/usr/bin/env bash
# test_xhci_interrupt.sh — USB_PLAN U6 gate: real interrupt endpoints.
#
# The deleted stub zero-filled the caller's buffer and returned success, so
# an xHCI keyboard reported "ready" and then delivered nothing, forever.
# Two things therefore have to be proved, and the second is as important as
# the first:
#
#   1. injected keystrokes arrive, with the right characters;
#   2. NO input appears when nothing is injected -- the failure a
#      zero-filling stub would have hidden, and which a driver that
#      mis-parses a report can produce as phantom keys.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "xHCI interrupt endpoints + HID input (USB_PLAN U6)"

LOG="$IL_LOGDIR/xhci_interrupt.log"
MON="$IL_LOGDIR/xhci_interrupt.hmp.sock"
rm -f "$LOG" "$MON"
IL_LAST_LOG="$LOG"
trap 'rm -f "$MON"; il_dump_on_error' EXIT

set +e
timeout --foreground 60 "$IL_QEMU" \
    -drive "file=$IL_ISO,format=raw,if=ide,snapshot=on" \
    -m 512M -smp 2 -display none \
    -serial "file:$LOG" \
    -monitor "unix:$MON,server,nowait" \
    -no-reboot -cpu qemu64 -boot order=c \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 &
QPID=$!
set -e

python3 - "$LOG" "$MON" <<'PY'
import pathlib, socket, sys, time
log = pathlib.Path(sys.argv[1]); mon = pathlib.Path(sys.argv[2])

def wait_for(pat, timeout=40):
    end = time.time() + timeout
    while time.time() < end:
        if log.exists() and pat in log.read_text(errors='ignore'):
            return True
        time.sleep(0.25)
    return False

end = time.time() + 20
while time.time() < end and not mon.exists():
    time.sleep(0.1)
if not mon.exists():
    raise SystemExit('monitor socket did not appear')

if not wait_for('keyboard ready', 45):
    raise SystemExit('xHCI keyboard never became ready')

# Give the shell a moment to start reading, then confirm that an idle
# keyboard produces nothing at all.
if not wait_for('auralite#', 45):
    raise SystemExit('shell did not start')
time.sleep(3)
before = log.read_text(errors='ignore')

def hmp(cmd):
    s = socket.socket(socket.AF_UNIX); s.connect(str(mon)); time.sleep(0.15)
    s.sendall(cmd.encode('ascii') + b'\n'); time.sleep(0.35); s.close()

# Type "echo" then Enter, one key at a time through the emulated HID device.
for k in ['e', 'c', 'h', 'o']:
    hmp('sendkey ' + k)
    time.sleep(0.25)
hmp('sendkey ret')
time.sleep(2)

after = log.read_text(errors='ignore')
pathlib.Path(sys.argv[1] + '.before').write_text(before)
pathlib.Path(sys.argv[1] + '.after').write_text(after)
PY

wait "$QPID" 2>/dev/null || true

# The endpoint must have been configured as an interrupt endpoint, not
# quietly treated as bulk.
il_assert_grep "$LOG" "\[hid\] keyboard ready: addr=. iface=0 ep=0x81" \
    "HID keyboard bound to its interrupt endpoint"

# 1. Reports actually arrived over the USB interrupt endpoint.
#
# This deliberately does NOT assert that "echo" reached the shell, which is
# what the first version of this test did.  QEMU's `sendkey` also drives the
# emulated PS/2 keyboard, so that assertion passes even with no USB device
# attached at all -- it was green while this path delivered nothing.  See
# test_usb_hid_input.sh, which keeps a no-USB control case to prove it.
il_assert_grep "$LOG" "\[hid\] report from addr=. ep=0x81" \
    "HID reports arrive over the xHCI interrupt endpoint (not via PS/2)"
il_assert_grep_fixed "$LOG" "len=8: 00 00 08 00" \
    "report carries the real HID usage code for 'e' (0x08)"

# 2. The inverse: the shell saw nothing while the keyboard sat idle.
#
#    Compare the text *after* the shell prompt in the pre-injection
#    snapshot: anything there would be input the OS invented.  Matching on
#    line shapes instead (as the first cut of this test did) is wrong --
#    it flagged "hello", which is the output of the /bin/hello boot
#    self-test, not keyboard input.
IL_ASSERT_COUNT=$((IL_ASSERT_COUNT + 1))
if [ -f "$LOG.before" ]; then
    idle_tail=$(sed -n 's/.*auralite#//p' "$LOG.before" | tr -d ' \r\n')
else
    idle_tail="MISSING"
fi
if [ -z "$idle_tail" ]; then
    il_pass "no phantom input while the keyboard was idle"
else
    il_fail "phantom input appeared before any key was injected: '$idle_tail'"
fi

il_assert_no_grep "$LOG" "interrupt endpoint stalled|transfer timeout|Page Fault|PANIC" \
    "no stalls, timeouts or faults"

rm -f "$LOG.before" "$LOG.after"
il_summary

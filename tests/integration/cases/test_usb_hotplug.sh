#!/usr/bin/env bash
# test_usb_hotplug.sh — runtime USB HID attach/detach via QEMU monitor.

set -u
cd "$(dirname "$0")/.."
. lib/lib.sh
il_init
il_have qemu-system-x86_64 python3

il_section "USB runtime hotplug attach/detach (xHCI HID)"

LOG="$IL_LOGDIR/usb_hotplug.log"
MON="$IL_LOGDIR/usb_hotplug.hmp.sock"
rm -f "$LOG" "$MON"
IL_LAST_LOG="$LOG"
trap 'rm -f "$MON"; il_dump_on_error' EXIT

set +e
timeout --foreground 50 "$IL_QEMU" \
    -cdrom "$IL_ISO" \
    -m 512M \
    -smp 2 \
    -display none \
    -serial "file:$LOG" \
    -monitor "unix:$MON,server,nowait" \
    -no-reboot \
    -cpu qemu64 \
    -boot order=d \
    -netdev user,id=net0 \
    -device e1000,netdev=net0 \
    -device qemu-xhci,id=xhci &
QPID=$!
set -e

python3 - "$LOG" "$MON" <<'PY'
import pathlib, socket, sys, time
log = pathlib.Path(sys.argv[1])
mon = pathlib.Path(sys.argv[2])

def wait_for(pattern, timeout=25):
    deadline = time.time() + timeout
    while time.time() < deadline:
        txt = log.read_text(errors='ignore') if log.exists() else ''
        if pattern in txt:
            return True
        time.sleep(0.25)
    return False

# Wait for monitor socket and AuraLite's hotplug thread.
deadline = time.time() + 15
while time.time() < deadline and not mon.exists():
    time.sleep(0.1)
if not mon.exists():
    raise SystemExit('monitor socket did not appear')
if not wait_for('hotplug monitor started', 30):
    raise SystemExit('AuraLite hotplug monitor did not start')

def hmp(cmd):
    s = socket.socket(socket.AF_UNIX)
    s.connect(str(mon))
    time.sleep(0.1)
    s.sendall(cmd.encode('ascii') + b'\n')
    time.sleep(0.2)
    s.close()

hmp('device_add usb-kbd,bus=xhci.0,port=1,id=hotkbd')
if not wait_for('keyboard ready: addr=', 20):
    raise SystemExit('hotplug keyboard did not attach')
hmp('device_del hotkbd')
if not wait_for('device removed addr=', 15):
    raise SystemExit('hotplug keyboard did not detach')
PY

# Let QEMU terminate through timeout, then continue regardless of rc.
set +e
wait "$QPID"
set -e

il_assert_grep "$LOG" "\[usb\] hotplug monitor started" "hotplug monitor started"
il_assert_grep "$LOG" "\[usb\] hotplug: new root device" "new USB device detected after boot"
il_assert_grep "$LOG" "\[hid\] keyboard ready: addr=" "hotplug HID keyboard attached"
il_assert_grep "$LOG" "\[usb\] hotplug: device removed" "USB detach detected"
il_assert_no_grep "$LOG" "Page Fault|kernel panic|hotplug: root enumeration failed|SET_ADDRESS failed" \
    "no hotplug attach/detach faults"

il_summary
